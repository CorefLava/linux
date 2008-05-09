/**********************************************************************/
/*   This file contains much of the glue between the Solaris code in  */
/*   dtrace.c  and  the linux kernel. We emulate much of the missing  */
/*   functionality, or map into the kernel.			      */
/*   								      */
/*   This file/driver is defined as GPL to allow us to find a way to  */
/*   gain  access  to  the symbol table used by the kallsyms driver.  */
/*   This  is  not correct - since it represents a fight between the  */
/*   CDDL and the GPL, and more investigation will be needed to find  */
/*   a valid way to avoid breaking the spirit of any license.	      */
/*   								      */
/*   Date: April 2008						      */
/*   Author: Paul D. Fox					      */
/*   								      */
/*   License: GPL 2						      */
/**********************************************************************/

//#pragma ident	"@(#)fbt.c	1.11	04/12/18 SMI"

#include <dtrace_linux.h>
#include <sys/dtrace_impl.h>
#include <sys/dtrace.h>
#include <linux/cpumask.h>

#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/list.h>

MODULE_AUTHOR("Paul D. Fox");
//MODULE_LICENSE("CDDL");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DTRACE/Function Boundary Tracing Driver");

# define modctl module

/**********************************************************************/
/*   Stuff we stash away from /proc/kallsyms.			      */
/**********************************************************************/
static struct map {
	char		*m_name;
	unsigned long	*m_ptr;
	} syms[] = {
	{"kallsyms_op", NULL},
	{"kallsyms_num_syms", NULL},
	{"kallsyms_addresses", NULL},
	{"kallsyms_expand_symbol", NULL},
	{"get_symbol_offset", NULL},
	{"kallsyms_lookup_name", NULL},
	{"modules", NULL},
	{0}
	};
static int xkallsyms_num_syms;
static long *xkallsyms_addresses;
static void *xkallsyms_op;
static unsigned int (*xkallsyms_expand_symbol)(int, char *);
static unsigned int (*xget_symbol_offset)(int);
static unsigned long (*xkallsyms_lookup_name)(char *);
static void *xmodules;

#define	FBT_PUSHL_EBP		0x55
#define	FBT_MOVL_ESP_EBP0_V0	0x8b
#define	FBT_MOVL_ESP_EBP1_V0	0xec
#define	FBT_MOVL_ESP_EBP0_V1	0x89
#define	FBT_MOVL_ESP_EBP1_V1	0xe5
#define	FBT_REX_RSP_RBP		0x48

#define	FBT_POPL_EBP		0x5d
#define	FBT_RET			0xc3
#define	FBT_RET_IMM16		0xc2
#define	FBT_LEAVE		0xc9

#ifdef __amd64
#define	FBT_PATCHVAL		0xcc
#else
#define	FBT_PATCHVAL		0xf0
#endif

#define	FBT_ENTRY	"entry"
#define	FBT_RETURN	"return"
#define	FBT_ADDR2NDX(addr)	((((uintptr_t)(addr)) >> 4) & fbt_probetab_mask)
#define	FBT_PROBETAB_SIZE	0x8000		/* 32k entries -- 128K total */

typedef struct fbt_probe {
	struct fbt_probe *fbtp_hashnext;
	uint8_t		*fbtp_patchpoint;
	int8_t		fbtp_rval;
	uint8_t		fbtp_patchval;
	uint8_t		fbtp_savedval;
	uintptr_t	fbtp_roffset;
	dtrace_id_t	fbtp_id;
	char		*fbtp_name;
	struct modctl	*fbtp_ctl;
	int		fbtp_loadcnt;
	int		fbtp_symndx;
	int		fbtp_primary;
	struct fbt_probe *fbtp_next;
} fbt_probe_t;

static dev_info_t		*fbt_devi;
static dtrace_provider_id_t	fbt_id;
static fbt_probe_t		**fbt_probetab;
static int			fbt_probetab_size;
static int			fbt_probetab_mask;
static int			fbt_verbose = 0;

static int
fbt_invop(uintptr_t addr, uintptr_t *stack, uintptr_t rval)
{
	uintptr_t stack0, stack1, stack2, stack3, stack4;
	fbt_probe_t *fbt = fbt_probetab[FBT_ADDR2NDX(addr)];

	for (; fbt != NULL; fbt = fbt->fbtp_hashnext) {
		if ((uintptr_t)fbt->fbtp_patchpoint == addr) {
			if (fbt->fbtp_roffset == 0) {
				/*
				 * When accessing the arguments on the stack,
				 * we must protect against accessing beyond
				 * the stack.  We can safely set NOFAULT here
				 * -- we know that interrupts are already
				 * disabled.
				 */
				DTRACE_CPUFLAG_SET(CPU_DTRACE_NOFAULT);
				CPU->cpu_dtrace_caller = stack[0];
				stack0 = stack[1];
				stack1 = stack[2];
				stack2 = stack[3];
				stack3 = stack[4];
				stack4 = stack[5];
				DTRACE_CPUFLAG_CLEAR(CPU_DTRACE_NOFAULT |
				    CPU_DTRACE_BADADDR);

				dtrace_probe(fbt->fbtp_id, stack0, stack1,
				    stack2, stack3, stack4);

				CPU->cpu_dtrace_caller = NULL;
			} else {
#ifdef __amd64
				/*
				 * On amd64, we instrument the ret, not the
				 * leave.  We therefore need to set the caller
				 * to assure that the top frame of a stack()
				 * action is correct.
				 */
				DTRACE_CPUFLAG_SET(CPU_DTRACE_NOFAULT);
				CPU->cpu_dtrace_caller = stack[0];
				DTRACE_CPUFLAG_CLEAR(CPU_DTRACE_NOFAULT |
				    CPU_DTRACE_BADADDR);
#endif

				dtrace_probe(fbt->fbtp_id, fbt->fbtp_roffset,
				    rval, 0, 0, 0);
				CPU->cpu_dtrace_caller = NULL;
			}

			return (fbt->fbtp_rval);
		}
	}

	return (0);
}
static int
get_refcount(struct module *mp)
{	int	sum = 0;
	int	i;

	for (i = 0; i < NR_CPUS; i++)
		sum += local_read(&mp->ref[i].count);
	return sum;
}

struct module *
get_module(int n)
{	struct module *modp;
	struct list_head *head = (struct list_head *) xmodules;

//printk("get_module(%d) head=%p\n", n, head);
	if (head == NULL)
		return NULL;

	list_for_each_entry(modp, head, list) {
		if (n-- == 0) {
			return modp;
		}
	}
	return NULL;
}
/*ARGSUSED*/
static void
fbt_provide_module(void *arg, struct modctl *ctl)
{	int	i;
	struct module *mp = (struct module *) ctl;
	char *modname = mp->name;
	char	*str = mp->strtab;
	char	*name;
	int	 size;
	fbt_probe_t *fbt, *retfbt;

TODO();
printk("ctl=%p name=%s\n", ctl, modname);
# if 0
	struct module *mp = ctl->mod_mp;
	char *str = mp->strings;
	int nsyms = mp->nsyms;
	Shdr *symhdr = mp->symhdr;
	char *name;
	size_t symsize;

	/*
	 * Employees of dtrace and their families are ineligible.  Void
	 * where prohibited.
	 */
	if (strcmp(modname, "dtrace") == 0)
		return;

TODO();
	if (ctl->mod_requisites != NULL) {
		struct modctl_list *list;

		list = (struct modctl_list *)ctl->mod_requisites;

		for (; list != NULL; list = list->modl_next) {
			if (strcmp(list->modl_modp->mod_modname, "dtrace") == 0)
				return;
		}
	}

TODO();
	/*
	 * KMDB is ineligible for instrumentation -- it may execute in
	 * any context, including probe context.
	 */
	if (strcmp(modname, "kmdbmod") == 0)
		return;

	if (str == NULL || symhdr == NULL || symhdr->sh_addr == NULL) {
		/*
		 * If this module doesn't (yet) have its string or symbol
		 * table allocated, clear out.
		 */
		return;
	}

	symsize = symhdr->sh_entsize;

	if (mp->fbt_nentries) {
		/*
		 * This module has some FBT entries allocated; we're afraid
		 * to screw with it.
		 */
		return;
	}
# endif

printk("num_symtab=%d\n", mp->num_symtab);
	for (i = 1; i < mp->num_symtab; i++) {
		uint8_t *instr, *limit;
		Elf_Sym *sym = (Elf_Sym *) &mp->symtab[i];

		name = str + sym->st_name;
		if (sym->st_name == NULL || *name == '\0')
			continue;

		/***********************************************/
		/*   Linux re-encodes the symbol types.	       */
		/***********************************************/
		if (sym->st_info != 't' && sym->st_info != 'T')
			continue;

#if 0
		if (ELF_ST_TYPE(sym->st_info) != STT_FUNC)
			continue;


		/*
		 * Weak symbols are not candidates.  This could be made to
		 * work (where weak functions and their underlying function
		 * appear as two disjoint probes), but it's not simple.
		 */
		if (ELF_ST_BIND(sym->st_info) == STB_WEAK)
			continue;
#endif

		if (strstr(name, "dtrace_") == name &&
		    strstr(name, "dtrace_safe_") != name) {
			/*
			 * Anything beginning with "dtrace_" may be called
			 * from probe context unless it explitly indicates
			 * that it won't be called from probe context by
			 * using the prefix "dtrace_safe_".
			 */
			continue;
		}

		if (strstr(name, "kdi_") == name ||
		    strstr(name, "kprobe") == name) {
			/*
			 * Anything beginning with "kdi_" is a part of the
			 * kernel debugger interface and may be called in
			 * arbitrary context -- including probe context.
			 */
			continue;
		}

		/*
		 * Due to 4524008, _init and _fini may have a bloated st_size.
		 * While this bug was fixed quite some time ago, old drivers
		 * may be lurking.  We need to develop a better solution to
		 * this problem, such that correct _init and _fini functions
		 * (the vast majority) may be correctly traced.  One solution
		 * may be to scan through the entire symbol table to see if
		 * any symbol overlaps with _init.  If none does, set a bit in
		 * the module structure that this module has correct _init and
		 * _fini sizes.  This will cause some pain the first time a
		 * module is scanned, but at least it would be O(N) instead of
		 * O(N log N)...
		 */
		if (strcmp(name, "_init") == 0)
			continue;

		if (strcmp(name, "_fini") == 0)
			continue;

		/*
		 * In order to be eligible, the function must begin with the
		 * following sequence:
		 *
		 * 	pushl	%esp
		 *	movl	%esp, %ebp
		 *
		 * Note that there are two variants of encodings that generate
		 * the movl; we must check for both.  For 64-bit, we would
		 * normally insist that a function begin with the following
		 * sequence:
		 *
		 *	pushq	%rbp
		 *	movq	%rsp, %rbp
		 *
		 * However, the compiler for 64-bit often splits these two
		 * instructions -- and the first instruction in the function
		 * is often not the pushq.  As a result, on 64-bit we look
		 * for any "pushq %rbp" in the function and we instrument
		 * this with a breakpoint instruction.
		 */
		instr = (uint8_t *)sym->st_value;
		limit = (uint8_t *)(sym->st_value + sym->st_size);

printk("sym %d: %s ty=%x %p %p %d\n", i, name,sym->st_info,
instr, limit, sym->st_size);
if (i > 100) break;
#ifdef __amd64
		while (instr < limit) {
printk("disasm: %p %02x\n", instr, *instr);
			if (*instr == FBT_PUSHL_EBP)
				break;

			if ((size = dtrace_instr_size(instr)) <= 0)
				break;

			instr += size;
		}

		if (instr >= limit || *instr != FBT_PUSHL_EBP) {
			/*
			 * We either don't save the frame pointer in this
			 * function, or we ran into some disassembly
			 * screw-up.  Either way, we bail.
			 */
TODO();
printk("size=%d *instr=%02x %02x %d\n", size, *instr, FBT_PUSHL_EBP, limit-instr);
			continue;
		}
#else
		if (instr[0] != FBT_PUSHL_EBP)
			continue;

		if (!(instr[1] == FBT_MOVL_ESP_EBP0_V0 &&
		    instr[2] == FBT_MOVL_ESP_EBP1_V0) &&
		    !(instr[1] == FBT_MOVL_ESP_EBP0_V1 &&
		    instr[2] == FBT_MOVL_ESP_EBP1_V1))
			continue;
#endif

TODO();
		fbt = kmem_zalloc(sizeof (fbt_probe_t), KM_SLEEP);
TODO();
printk("fbt=%p\n", fbt);
		
		fbt->fbtp_name = name;
TODO();
		fbt->fbtp_id = dtrace_probe_create(fbt_id, modname,
		    name, FBT_ENTRY, 3, fbt);
TODO();
		fbt->fbtp_patchpoint = instr;
		fbt->fbtp_ctl = ctl;
		fbt->fbtp_loadcnt = get_refcount(mp);
		fbt->fbtp_rval = DTRACE_INVOP_PUSHL_EBP;
		fbt->fbtp_savedval = *instr;
		fbt->fbtp_patchval = FBT_PATCHVAL;

		fbt->fbtp_hashnext = fbt_probetab[FBT_ADDR2NDX(instr)];
		fbt->fbtp_symndx = i;
		fbt_probetab[FBT_ADDR2NDX(instr)] = fbt;

# if 0
		mp1->fbt_nentries++;
# endif

TODO();
		retfbt = NULL;
again:
		if (instr >= limit) {
TODO();
			continue;
		}

		/*
		 * If this disassembly fails, then we've likely walked off into
		 * a jump table or some other unsuitable area.  Bail out of the
		 * disassembly now.
		 */
		if ((size = dtrace_instr_size(instr)) <= 0)
			continue;

#ifdef __amd64
		/*
		 * We only instrument "ret" on amd64 -- we don't yet instrument
		 * ret imm16, largely because the compiler doesn't seem to
		 * (yet) emit them in the kernel...
		 */
		if (*instr != FBT_RET) {
			instr += size;
TODO();
			goto again;
		}
#else
		if (!(size == 1 &&
		    (*instr == FBT_POPL_EBP || *instr == FBT_LEAVE) &&
		    (*(instr + 1) == FBT_RET ||
		    *(instr + 1) == FBT_RET_IMM16))) {
			instr += size;
			goto again;
		}
#endif

		/*
		 * We have a winner!
		 */
TODO();
		fbt = kmem_zalloc(sizeof (fbt_probe_t), KM_SLEEP);
		fbt->fbtp_name = name;

		if (retfbt == NULL) {
			fbt->fbtp_id = dtrace_probe_create(fbt_id, modname,
			    name, FBT_RETURN, 3, fbt);
		} else {
			retfbt->fbtp_next = fbt;
			fbt->fbtp_id = retfbt->fbtp_id;
		}

		retfbt = fbt;
		fbt->fbtp_patchpoint = instr;
		fbt->fbtp_ctl = ctl;
		fbt->fbtp_loadcnt = get_refcount(mp);

#ifndef __amd64
		if (*instr == FBT_POPL_EBP) {
			fbt->fbtp_rval = DTRACE_INVOP_POPL_EBP;
		} else {
			ASSERT(*instr == FBT_LEAVE);
			fbt->fbtp_rval = DTRACE_INVOP_LEAVE;
		}
		fbt->fbtp_roffset =
		    (uintptr_t)(instr - (uint8_t *)sym->st_value) + 1;

#else
		ASSERT(*instr == FBT_RET);
		fbt->fbtp_rval = DTRACE_INVOP_RET;
		fbt->fbtp_roffset =
		    (uintptr_t)(instr - (uint8_t *)sym->st_value);
#endif

		fbt->fbtp_savedval = *instr;
		fbt->fbtp_patchval = FBT_PATCHVAL;
		fbt->fbtp_hashnext = fbt_probetab[FBT_ADDR2NDX(instr)];
		fbt->fbtp_symndx = i;
		fbt_probetab[FBT_ADDR2NDX(instr)] = fbt;

# if 0
		mp->fbt_nentries++;
# endif

		instr += size;
		goto again;
	}
}

/*ARGSUSED*/
static void
fbt_destroy(void *arg, dtrace_id_t id, void *parg)
{
	fbt_probe_t *fbt = parg, *next, *hash, *last;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = ctl;
	int ndx;

	do {
		if (mp != NULL && get_refcount(mp) == fbt->fbtp_loadcnt) {
			if ((get_refcount(mp) == fbt->fbtp_loadcnt &&
			    mp->state == MODULE_STATE_LIVE)) {
# if 0
				((struct module *)
				    (ctl->mod_mp))->fbt_nentries--;
# endif
			}
		}

		/*
		 * Now we need to remove this probe from the fbt_probetab.
		 */
		ndx = FBT_ADDR2NDX(fbt->fbtp_patchpoint);
		last = NULL;
		hash = fbt_probetab[ndx];

		while (hash != fbt) {
			ASSERT(hash != NULL);
			last = hash;
			hash = hash->fbtp_hashnext;
		}

		if (last != NULL) {
			last->fbtp_hashnext = fbt->fbtp_hashnext;
		} else {
			fbt_probetab[ndx] = fbt->fbtp_hashnext;
		}

		next = fbt->fbtp_next;
		kmem_free(fbt, sizeof (fbt_probe_t));

		fbt = next;
	} while (fbt != NULL);
}

/*ARGSUSED*/
static void
fbt_enable(void *arg, dtrace_id_t id, void *parg)
{
	fbt_probe_t *fbt = parg;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = (struct module *) ctl;

# if 0
	ctl->mod_nenabled++;
# endif

	if (mp->state != MODULE_STATE_LIVE) {
		if (fbt_verbose) {
			cmn_err(CE_NOTE, "fbt is failing for probe %s "
			    "(module %s unloaded)",
			    fbt->fbtp_name, mp->name);
		}

		return;
	}

	/*
	 * Now check that our modctl has the expected load count.  If it
	 * doesn't, this module must have been unloaded and reloaded -- and
	 * we're not going to touch it.
	 */
	if (get_refcount(mp) != fbt->fbtp_loadcnt) {
		if (fbt_verbose) {
			cmn_err(CE_NOTE, "fbt is failing for probe %s "
			    "(module %s reloaded)",
			    fbt->fbtp_name, mp->name);
		}

		return;
	}

	for (; fbt != NULL; fbt = fbt->fbtp_next)
		*fbt->fbtp_patchpoint = fbt->fbtp_patchval;
}

/*ARGSUSED*/
static void
fbt_disable(void *arg, dtrace_id_t id, void *parg)
{
	fbt_probe_t *fbt = parg;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = (struct module *) ctl;

# if 0
	ASSERT(ctl->mod_nenabled > 0);
	ctl->mod_nenabled--;

	if (!ctl->mod_loaded || (ctl->mod_loadcnt != fbt->fbtp_loadcnt))
		return;
# else
	if (mp->state != MODULE_STATE_LIVE ||
	    get_refcount(mp) != fbt->fbtp_loadcnt)
		return;
# endif

	for (; fbt != NULL; fbt = fbt->fbtp_next)
		*fbt->fbtp_patchpoint = fbt->fbtp_savedval;
}

/*ARGSUSED*/
static void
fbt_suspend(void *arg, dtrace_id_t id, void *parg)
{
	fbt_probe_t *fbt = parg;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = (struct module *) ctl;

# if 0
	ASSERT(ctl->mod_nenabled > 0);

	if (!ctl->mod_loaded || (ctl->mod_loadcnt != fbt->fbtp_loadcnt))
		return;
# else
	if (mp->state != MODULE_STATE_LIVE ||
	    get_refcount(mp) != fbt->fbtp_loadcnt)
		return;
# endif

	for (; fbt != NULL; fbt = fbt->fbtp_next)
		*fbt->fbtp_patchpoint = fbt->fbtp_savedval;
}

/*ARGSUSED*/
static void
fbt_resume(void *arg, dtrace_id_t id, void *parg)
{
	fbt_probe_t *fbt = parg;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = (struct module *) ctl;

# if 0
	ASSERT(ctl->mod_nenabled > 0);

	if (!ctl->mod_loaded || (ctl->mod_loadcnt != fbt->fbtp_loadcnt))
		return;
# else
	if (mp->state != MODULE_STATE_LIVE ||
	    get_refcount(mp) != fbt->fbtp_loadcnt)
		return;
# endif

	for (; fbt != NULL; fbt = fbt->fbtp_next)
		*fbt->fbtp_patchpoint = fbt->fbtp_patchval;
}

/*ARGSUSED*/
static void
fbt_getargdesc(void *arg, dtrace_id_t id, void *parg, dtrace_argdesc_t *desc)
{
	fbt_probe_t *fbt = parg;
	struct modctl *ctl = fbt->fbtp_ctl;
	struct module *mp = (struct module *) ctl;
	ctf_file_t *fp = NULL, *pfp;
	ctf_funcinfo_t f;
	int error;
	ctf_id_t argv[32], type;
	int argc = sizeof (argv) / sizeof (ctf_id_t);
	const char *parent;

	if (mp->state != MODULE_STATE_LIVE ||
	    get_refcount(mp) != fbt->fbtp_loadcnt)
		return;

	if (fbt->fbtp_roffset != 0 && desc->dtargd_ndx == 0) {
		(void) strcpy(desc->dtargd_native, "int");
		return;
	}

# if 0
	if ((fp = ctf_modopen(mp, &error)) == NULL) {
		/*
		 * We have no CTF information for this module -- and therefore
		 * no args[] information.
		 */
		goto err;
	}
# endif

	TODO();
	if (fp == NULL)
		goto err;
# if 0

	/*
	 * If we have a parent container, we must manually import it.
	 */
	if ((parent = ctf_parent_name(fp)) != NULL) {
		TODO();
		struct modctl *mod;

		/*
		 * We must iterate over all modules to find the module that
		 * is our parent.
		 */
		for (mod = &modules; mod != NULL; mod = mod->mod_next) {
			if (strcmp(mod->mod_filename, parent) == 0)
				break;
		}

		if (mod == NULL)
			goto err;

		if ((pfp = ctf_modopen(mod->mod_mp, &error)) == NULL)
			goto err;

		if (ctf_import(fp, pfp) != 0) {
			ctf_close(pfp);
			goto err;
		}

		ctf_close(pfp);
	}
# endif

	if (ctf_func_info(fp, fbt->fbtp_symndx, &f) == CTF_ERR)
		goto err;

	if (fbt->fbtp_roffset != 0) {
		if (desc->dtargd_ndx > 1)
			goto err;

		ASSERT(desc->dtargd_ndx == 1);
		type = f.ctc_return;
	} else {
		if (desc->dtargd_ndx + 1 > f.ctc_argc)
			goto err;

		if (ctf_func_args(fp, fbt->fbtp_symndx, argc, argv) == CTF_ERR)
			goto err;

		type = argv[desc->dtargd_ndx];
	}

	if (ctf_type_name(fp, type, desc->dtargd_native,
	    DTRACE_ARGTYPELEN) != NULL) {
		ctf_close(fp);
		return;
	}
err:
	if (fp != NULL)
		ctf_close(fp);

	desc->dtargd_ndx = DTRACE_ARGNONE;
}

static dtrace_pattr_t fbt_attr = {
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_ISA },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_UNKNOWN },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_UNKNOWN },
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_ISA },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_ISA },
};

static dtrace_pops_t fbt_pops = {
	NULL,
	fbt_provide_module,
	fbt_enable,
	fbt_disable,
	fbt_suspend,
	fbt_resume,
	fbt_getargdesc,
	NULL,
	NULL,
	fbt_destroy
};
static void
fbt_cleanup(dev_info_t *devi)
{
	dtrace_invop_remove(fbt_invop);
	if (fbt_id)
		dtrace_unregister(fbt_id);

//	ddi_remove_minor_node(devi, NULL);
	kmem_free(fbt_probetab, fbt_probetab_size * sizeof (fbt_probe_t *));
	fbt_probetab = NULL;
	fbt_probetab_mask = 0;
}

# if 0
static int
fbt_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	if (fbt_probetab_size == 0)
		fbt_probetab_size = FBT_PROBETAB_SIZE;

	fbt_probetab_mask = fbt_probetab_size - 1;
	fbt_probetab =
	    kmem_zalloc(fbt_probetab_size * sizeof (fbt_probe_t *), KM_SLEEP);

	dtrace_invop_add(fbt_invop);

	if (ddi_create_minor_node(devi, "fbt", S_IFCHR, 0,
	    DDI_PSEUDO, NULL) == DDI_FAILURE ||
	    dtrace_register("fbt", &fbt_attr, DTRACE_PRIV_KERNEL, 0,
	    &fbt_pops, NULL, &fbt_id) != 0) {
		fbt_cleanup(devi);
		return (DDI_FAILURE);
	}

	ddi_report_dev(devi);
	fbt_devi = devi;

	return (DDI_SUCCESS);
}

static int
fbt_detach(dev_info_t *devi, ddi_detach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	if (dtrace_unregister(fbt_id) != 0)
		return (DDI_FAILURE);

	fbt_cleanup(devi);

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
fbt_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	int error;

	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = (void *)fbt_devi;
		error = DDI_SUCCESS;
		break;
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		error = DDI_SUCCESS;
		break;
	default:
		error = DDI_FAILURE;
	}
	return (error);
}

/*ARGSUSED*/
static int
fbt_open(dev_t *devp, int flag, int otyp, cred_t *cred_p)
{
	return (0);
}

static struct cb_ops fbt_cb_ops = {
	fbt_open,		/* open */
	nodev,			/* close */
	nulldev,		/* strategy */
	nulldev,		/* print */
	nodev,			/* dump */
	nodev,			/* read */
	nodev,			/* write */
	nodev,			/* ioctl */
	nodev,			/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	nochpoll,		/* poll */
	ddi_prop_op,		/* cb_prop_op */
	0,			/* streamtab  */
	D_NEW | D_MP		/* Driver compatibility flag */
};

static struct dev_ops fbt_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* refcnt */
	fbt_info,		/* get_dev_info */
	nulldev,		/* identify */
	nulldev,		/* probe */
	fbt_attach,		/* attach */
	fbt_detach,		/* detach */
	nodev,			/* reset */
	&fbt_cb_ops,		/* driver operations */
  	NULL,			/* bus operations */
	nodev			/* dev power */
};
# endif

/**********************************************************************/
/*   Module interface to the kernel.				      */
/**********************************************************************/
static int fbt_ioctl(struct inode *inode, struct file *file,
                     unsigned int cmd, unsigned long arg)
{	int	ret;

	return -EIO;
}
static int
fbt_open(struct module *mp, int *error)
{
	return 0;
}
static int
fbt_read(ctf_file_t *fp, int fd)
{
	return -EIO;
}
/**********************************************************************/
/*   User  is  writing to us to tell us where the kprobes symtab is.  */
/*   We  do this to avoid tripping over the GPL vs CDDL issue and to  */
/*   avoid  a tight compile-time coupling against bits of the kernel  */
/*   which are deemed private.					      */
/**********************************************************************/

static ssize_t 
fbt_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *pos)
{	int	n;
	int	orig_count = count;
	char	*bufend = buf + count;
	char	*cp;
	char	*symend;
	struct map *mp;

//printk("write: '%*.*s'\n", count, count, buf);
	/***********************************************/
	/*   Allow  for 'nm -p' format so we can just  */
	/*   do:				       */
	/*   grep XXX /proc/kallsyms > /dev/fbt	       */
	/***********************************************/
	while (buf < bufend) {
		count = bufend - buf;
		if ((cp = memchr(buf, ' ', count)) == NULL ||
		     cp + 3 >= bufend ||
		     cp[1] == ' ' ||
		     cp[2] != ' ') {
			return -EIO;
		}

		if ((cp = memchr(buf, '\n', count)) == NULL) {
			return -EIO;
		}
		symend = cp--;
		while (cp > buf && cp[-1] != ' ')
			cp--;
		n = symend - cp;
//printk("sym='%*.*s'\n", n, n, cp);
		for (mp = syms; mp->m_name; mp++) {
			if (strlen(mp->m_name) == n &&
			    memcmp(mp->m_name, cp, n) == 0)
			    	break;
		}
		if (mp->m_name != NULL) {
			mp->m_ptr = simple_strtoul(buf, NULL, 16);
			printk("fbt: got %s=%p\n", mp->m_name, mp->m_ptr);
		}
		buf = symend + 1;
	}

	if (syms[1].m_ptr)
		xkallsyms_num_syms = *(int *) syms[1].m_ptr;
	xkallsyms_addresses 	= syms[2].m_ptr;
	xkallsyms_expand_symbol = syms[3].m_ptr;
	xget_symbol_offset 	= syms[4].m_ptr;
	xkallsyms_lookup_name 	= syms[5].m_ptr;
	xmodules 		= syms[6].m_ptr;

	/***********************************************/
	/*   Dump out the symtab for debugging.	       */
	/***********************************************/
# if 0
	if (xkallsyms_num_syms > 0 && xkallsyms_addresses) {
		int	i;
		unsigned int off = 0;
		for (i = 0; i < 10 && i < xkallsyms_num_syms; i++) {
			unsigned long addr = xkallsyms_addresses[i];
			char buf[512];
			off = xkallsyms_expand_symbol(off, buf);
			printk("%d: %p '%s'\n", i, addr, buf);
			}
	}
# endif
	if (xget_symbol_offset && xkallsyms_expand_symbol) {
		int	i;
		unsigned int off = 0;
		for (i = 0; i < 2; i++) {
			unsigned long addr = (*xget_symbol_offset)(i);
			char buf[512];
			off = xkallsyms_expand_symbol(addr, buf);
			printk("%d: %p '%s'\n", i, addr, buf);
			}
	}

	return orig_count;
}

static const struct file_operations fbt_fops = {
        .ioctl = fbt_ioctl,
        .open = fbt_open,
        .read = fbt_read,
        .write = fbt_write,
};

static struct miscdevice fbt_dev = {
        MISC_DYNAMIC_MINOR,
        "fbt",
        &fbt_fops
};
int fbt_init(void)
{	int	ret;

	ret = misc_register(&fbt_dev);
	if (ret) {
		printk(KERN_WARNING "fbt: Unable to register misc device\n");
		return ret;
		}

	/***********************************************/
	/*   Helper not presently implemented :-(      */
	/***********************************************/
	printk(KERN_WARNING "fbt loaded: /dev/fbt now available\n");
/*{char buf[256];
extern int kallsyms_lookup_name(char *);
extern int kallsyms_lookup_size_offset(char *);
extern int kallsyms_op;
unsigned long p;

//p = kallsyms_lookup_name("kallsyms_init");
printk("fbt: kallsyms_op = %p\n", kallsyms_lookup_size_offset);
}
*/
	if (fbt_probetab_size == 0)
		fbt_probetab_size = FBT_PROBETAB_SIZE;

	fbt_probetab_mask = fbt_probetab_size - 1;
	fbt_probetab =
	    kmem_zalloc(fbt_probetab_size * sizeof (fbt_probe_t *), KM_SLEEP);

	dtrace_invop_add(fbt_invop);
	
	dtrace_register("fbt", &fbt_attr, DTRACE_PRIV_KERNEL, 0,
	    &fbt_pops, NULL, &fbt_id);

	return 0;
}
void fbt_exit(void)
{
	fbt_cleanup(NULL);

	printk(KERN_WARNING "fbt driver unloaded.\n");
	misc_deregister(&fbt_dev);
}