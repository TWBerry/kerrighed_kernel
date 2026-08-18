/** Kerrighed MM servers.
 *  @file mm_server.c
 *
 *  Copyright (C) 2008, Renaud Lottiaux, Kerlabs.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/userfaultfd_k.h>
#include <linux/mmu_context.h>
#include <linux/syscalls.h>

#include <net/krgrpc/rpc.h>
#include "mm_struct.h"
#include "mm_server.h"

static int krg_remote_mm_enter(struct mm_struct *mm)
{
	if (current->mm)
		return -EBUSY;
	current->krg_mm_remote_apply++;
	use_mm(mm);
	return 0;
}

static void krg_remote_mm_leave(struct mm_struct *mm)
{
	unuse_mm(mm);
	current->krg_mm_remote_apply--;
}

static int handle_do_mmap_region(struct rpc_desc *desc, void *msgIn, size_t size)
{
	struct mm_vma_msg *msg = msgIn;
	struct mm_struct *mm = krg_get_mm(msg->mm_id);
	LIST_HEAD(uf);
	unsigned long ret;
	int err;
	if (!mm) return 0;
	err = krg_remote_mm_enter(mm);
	if (err) goto out;
	down_write(&mm->mmap_sem);
	ret = mmap_region(NULL, msg->start, msg->len, msg->vm_flags,
			  msg->pgoff, &uf);
	up_write(&mm->mmap_sem);
	userfaultfd_unmap_complete(mm, &uf);
	krg_remote_mm_leave(mm);
	err = IS_ERR_VALUE(ret) ? (int)ret : 0;
out:
	krg_put_mm(msg->mm_id);
	return err;
}

static int handle_do_mremap(struct rpc_desc *desc, void *msgIn, size_t size)
{
	struct mm_vma_msg *msg = msgIn;
	struct mm_struct *mm = krg_get_mm(msg->mm_id);
	unsigned long ret;
	int err;
	if (!mm) return 0;
	err = krg_remote_mm_enter(mm);
	if (err) goto out;
	ret = sys_mremap(msg->start, msg->old_len, msg->new_len,
			 msg->flags, msg->new_addr);
	krg_remote_mm_leave(mm);
	if (ret != msg->result)
		err = IS_ERR_VALUE(ret) ? (int)ret : -EFAULT;
out:
	krg_put_mm(msg->mm_id);
	return err;
}

static int handle_do_brk(struct rpc_desc *desc, void *msgIn, size_t size)
{
	struct mm_vma_msg *msg = msgIn;
	struct mm_struct *mm = krg_get_mm(msg->mm_id);
	unsigned long ret;
	int err;
	if (!mm) return 0;
	err = krg_remote_mm_enter(mm);
	if (err) goto out;
	ret = sys_brk(msg->brk);
	krg_remote_mm_leave(mm);
	if (ret != msg->brk)
		err = -ENOMEM;
out:
	krg_put_mm(msg->mm_id);
	return err;
}

static int handle_expand_stack(struct rpc_desc *desc, void *msgIn, size_t size)
{
	struct mm_vma_msg *msg = msgIn;
	struct mm_struct *mm = krg_get_mm(msg->mm_id);
	struct vm_area_struct *vma;
	int err;
	if (!mm) return -EINVAL;
	err = krg_remote_mm_enter(mm);
	if (err) goto out;
	down_write(&mm->mmap_sem);
	vma = find_vma(mm, msg->start);
	err = vma ? expand_stack(vma, msg->result) : -EINVAL;
	up_write(&mm->mmap_sem);
	krg_remote_mm_leave(mm);
out:
	krg_put_mm(msg->mm_id);
	return err;
}

static int handle_do_mprotect(struct rpc_desc *desc, void *msgIn, size_t size)
{
	struct mm_vma_msg *msg = msgIn;
	struct mm_struct *mm = krg_get_mm(msg->mm_id);
	long ret;
	int err;
	if (!mm) return 0;
	err = krg_remote_mm_enter(mm);
	if (err) goto out;
	ret = sys_mprotect(msg->start, msg->len, msg->prot);
	krg_remote_mm_leave(mm);
	err = (int)ret;
out:
	krg_put_mm(msg->mm_id);
	return err;
}

/** Handler for remote munmap.
 *  @author Renaud Lottiaux
 */
int handle_do_munmap (struct rpc_desc* desc,
		      void *msgIn, size_t size)
{
	struct vm_area_struct *vma;
	struct mm_munmap_msg *msg = msgIn;
	struct mm_struct *mm;

	mm = krg_get_mm(msg->mm_id);

	if (!mm)
		return 0;

	vma = find_vma(mm, msg->start);
	if (vma)
		zap_page_range(vma, msg->start, msg->len, NULL);

	krg_put_mm(msg->mm_id);

	return 0;
}



/* MM handler Initialisation */

void mm_server_init (void)
{
	rpc_register_int(RPC_MM_MMAP_REGION, handle_do_mmap_region, 0);
	rpc_register_int(RPC_MM_MREMAP, handle_do_mremap, 0);
	rpc_register_int(RPC_MM_MUNMAP, handle_do_munmap, 0);
	rpc_register_int(RPC_MM_DO_BRK, handle_do_brk, 0);
	rpc_register_int(RPC_MM_EXPAND_STACK, handle_expand_stack, 0);
	rpc_register_int(RPC_MM_MPROTECT, handle_do_mprotect, 0);
}



/* MM server Finalization */

void mm_server_finalize (void)
{
}
