#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>
#include <asm/percpu.h>

#ifndef __ASSEMBLY__
struct task_struct;

DECLARE_PER_CPU(struct task_struct *, current_task);

static __always_inline struct task_struct *get_current(void)
{
	return this_cpu_read_stable(current_task);
}

#ifdef CONFIG_KRG_EPM
/*
 * Keep the Kerrighed effective-current semantics while avoiding a direct
 * dereference of an incomplete struct task_struct from this header.
 */
struct task_struct *krg_get_current(void);
struct task_struct **krg_current_ptr(void);

#define krg_current (*krg_current_ptr())
#define current krg_get_current()

#define krg_current_save(tmp) do {  \
		(tmp) = krg_current;  \
		krg_current = NULL; \
	} while (0)
#define krg_current_restore(tmp) do { \
		krg_current = (tmp);    \
	} while (0)

#else /* !CONFIG_KRG_EPM */
#define current get_current()
#endif /* !CONFIG_KRG_EPM */

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CURRENT_H */
