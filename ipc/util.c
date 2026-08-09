/*
 * linux/ipc/util.c
 * Copyright (C) 1992 Krishna Balasubramanian
 *
 * Sep 1997 - Call suser() last after "normal" permission checks so we
 *            get BSD style process accounting right.
 *            Occurs in several places in the IPC code.
 *            Chris Evans, <chris@ferret.lmh.ox.ac.uk>
 * Nov 1999 - ipc helper functions, unified SMP locking
 *	      Manfred Spraul <manfred@colorfullife.com>
 * Oct 2002 - One lock per IPC id. RCU ipc_free for lock-free grow_ary().
 *            Mingming Cao <cmm@us.ibm.com>
 * Mar 2006 - support for audit of ipc object properties
 *            Dustin Kirkland <dustin.kirkland@us.ibm.com>
 * Jun 2006 - namespaces ssupport
 *            OpenVZ, SWsoft Inc.
 *            Pavel Emelianov <xemul@openvz.org>
 *
 * General sysv ipc locking scheme:
 *	rcu_read_lock()
 *          obtain the ipc object (kern_ipc_perm) by looking up the id in an idr
 *	    tree.
 *	    - perform initial checks (capabilities, auditing and permission,
 *	      etc).
 *	    - perform read-only operations, such as STAT, INFO commands.
 *	      acquire the ipc lock (kern_ipc_perm.lock) through
 *	      ipc_lock_object()
 *		- perform data updates, such as SET, RMID commands and
 *		  mechanism-specific operations (semop/semtimedop,
 *		  msgsnd/msgrcv, shmat/shmdt).
 *	    drop the ipc lock, through ipc_unlock_object().
 *	rcu_read_unlock()
 *
 *  The ids->rwsem must be taken when:
 *	- creating, removing and iterating the existing entries in ipc
 *	  identifier sets.
 *	- iterating through files under /proc/sysvipc/
 *
 *  Note that sems have a special fast path that avoids kern_ipc_perm.lock -
 *  see sem_lock().
 */

#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/init.h>
#include <linux/msg.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/capability.h>
#include <linux/highuid.h>
#include <linux/security.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/audit.h>
#include <linux/nsproxy.h>
#include <linux/rwsem.h>
#include <linux/memory.h>
#include <linux/ipc_namespace.h>

#include <asm/unistd.h>

#include "util.h"

#ifdef CONFIG_KRG_IPC
#include <kddm/kddm.h>
#include "ipcmap_io_linker.h"
#include "ipc_handler.h"
#include "msg_handler.h"
#include "sem_handler.h"
#include "shm_handler.h"
#endif

struct ipc_proc_iface {
	const char *path;
	const char *header;
	int ids;
	int (*show)(struct seq_file *, void *);
};

/**
 * ipc_init - initialise ipc subsystem
 *
 * The various sysv ipc resources (semaphores, messages and shared
 * memory) are initialised.
 *
 * A callback routine is registered into the memory hotplug notifier
 * chain: since msgmni scales to lowmem this callback routine will be
 * called upon successful memory add / remove to recompute msmgni.
 */
static int __init ipc_init(void)
{
	sem_init();
	msg_init();
	shm_init();
	return 0;
}
device_initcall(ipc_init);

/**
 * ipc_init_ids	- initialise ipc identifiers
 * @ids: ipc identifier set
 *
 * Set up the sequence range to use for the ipc identifier range (limited
 * below ipc_mni) then initialise the ids idr.
 */
void ipc_init_ids(struct ipc_ids *ids)
{
	ids->in_use = 0;
	ids->seq = 0;
	init_rwsem(&ids->rwsem);
	idr_init(&ids->ipcs_idr);
#ifdef CONFIG_KRG_IPC
	ids->krgops = NULL;
#endif
	ids->max_idx = -1;
	ids->last_idx = -1;
#ifdef CONFIG_CHECKPOINT_RESTORE
	ids->next_id = -1;
#endif
}

#ifdef CONFIG_PROC_FS
static const struct file_operations sysvipc_proc_fops;
/**
 * ipc_init_proc_interface -  create a proc interface for sysipc types using a seq_file interface.
 * @path: Path in procfs
 * @header: Banner to be printed at the beginning of the file.
 * @ids: ipc id table to iterate.
 * @show: show routine.
 */
void __init ipc_init_proc_interface(const char *path, const char *header,
		int ids, int (*show)(struct seq_file *, void *))
{
	struct proc_dir_entry *pde;
	struct ipc_proc_iface *iface;

	iface = kmalloc(sizeof(*iface), GFP_KERNEL);
	if (!iface)
		return;
	iface->path	= path;
	iface->header	= header;
	iface->ids	= ids;
	iface->show	= show;

	pde = proc_create_data(path,
			       S_IRUGO,        /* world readable */
			       NULL,           /* parent dir */
			       &sysvipc_proc_fops,
			       iface);
	if (!pde) {
		kfree(iface);
	}
}
#endif

/**
 * ipc_findkey	- find a key in an ipc identifier set
 * @ids: ipc identifier set
 * @key: key to find
 *	
 * Returns the locked pointer to the ipc structure if found or NULL
 * otherwise. If key is found ipc points to the owning ipc structure
 *
 * Called with ipc_ids.rwsem held.
 */
static struct kern_ipc_perm *ipc_findkey(struct ipc_ids *ids, key_t key)
{
	struct kern_ipc_perm *ipc;
	int next_id;
	int total;

#ifdef CONFIG_KRG_IPC
	if (is_krg_ipc(ids)) {
		ipc = ids->krgops->ipc_findkey(ids, key);
		if (IS_ERR(ipc))
			ipc = NULL;
		return ipc;
	}
#endif

	for (total = 0, next_id = 0; total < ids->in_use; next_id++) {
		ipc = idr_find(&ids->ipcs_idr, next_id);

		if (ipc == NULL)
			continue;

		if (ipc->key != key) {
			total++;
			continue;
		}

		rcu_read_lock();
		ipc_lock_object(ipc);
		return ipc;
	}

	return NULL;
}

/*
 * Insert new IPC object into idr tree, and set sequence number and id
 * in the correct order.
 * Especially:
 * - the sequence number must be set before inserting the object into the idr,
 *   because the sequence number is accessed without a lock.
 * - the id can/must be set after inserting the object into the idr.
 *   All accesses must be done after getting kern_ipc_perm.lock.
 *
 * The caller must own kern_ipc_perm.lock.of the new object.
 * On error, the function returns a (negative) error code.
 *
 * To conserve sequence number space, especially with extended ipc_mni,
 * the sequence number is incremented only when the returned ID is less than
 * the last one.
 */
static inline int ipc_idr_alloc(struct ipc_ids *ids, struct kern_ipc_perm *new)
{
	int idx, next_id = -1;

#ifdef CONFIG_CHECKPOINT_RESTORE
	next_id = ids->next_id;
	ids->next_id = -1;
#endif

	/*
	 * As soon as a new object is inserted into the idr,
	 * ipc_obtain_object_idr() or ipc_obtain_object_check() can find it,
	 * and the lockless preparations for ipc operations can start.
	 * This means especially: permission checks, audit calls, allocation
	 * of undo structures, ...
	 *
	 * Thus the object must be fully initialized, and if something fails,
	 * then the full tear-down sequence must be followed.
	 * (i.e.: set new->deleted, reduce refcount, call_rcu())
	 */

	if (next_id < 0) { /* !CHECKPOINT_RESTORE or next_id is unset */
		int max_idx;

		max_idx = max(ids->in_use*3/2, ipc_min_cycle);
		max_idx = min(max_idx, ipc_mni);

		/* allocate the idx, with a NULL struct kern_ipc_perm */
		idx = idr_alloc_cyclic(&ids->ipcs_idr, NULL, 0, max_idx,
					GFP_NOWAIT);

		if (idx >= 0) {
			/*
			 * idx got allocated successfully.
			 * Now calculate the sequence number and set the
			 * pointer for real.
			 */
			if (idx <= ids->last_idx) {
				ids->seq++;
				if (ids->seq >= ipcid_seq_max())
					ids->seq = 0;
			}
			ids->last_idx = idx;

			new->seq = ids->seq;
			/* no need for smp_wmb(), this is done
			 * inside idr_replace, as part of
			 * rcu_assign_pointer
			 */
			idr_replace(&ids->ipcs_idr, new, idx);
		}
	} else {
		new->seq = ipcid_to_seqx(next_id);
		idx = idr_alloc(&ids->ipcs_idr, new, ipcid_to_idx(next_id),
				0, GFP_NOWAIT);
	}
	if (idx >= 0)
		new->id = (new->seq << ipcmni_seq_shift()) + idx;
	return idx;
}

#ifdef CONFIG_KRG_IPC
bool ipc_used(struct ipc_namespace *ns)
{
	bool used = false;
	int i;
	struct ipc_ids *ids;

	for (i = 0; i < ARRAY_SIZE(ns->ids); i++) {
		ids = &ns->ids[i];

		down_read(&ids->rwsem);
		used |= ipc_get_maxid(ids) != -1;
		up_read(&ids->rwsem);
	}

	return used;
}

static void krg_idr_get_new(struct ipc_ids *ids, struct kern_ipc_perm *new, int *idx)
{
	if (is_krg_ipc(ids)) {
		int ipcid, lid;

		ipcid = krg_ipc_get_new_id(ids);
		if (ipcid == -1) {
			*idx = -ENOMEM;
			return;
		}

		lid = ipcid_to_idx(ipcid);
		err = idr_get_new_above(&ids->ipcs_idr, new, lid, idx);
		if (!err && lid != *idx) {
			idr_remove(&ids->ipcs_idr, *idx);
			*idx = -EINVAL;
		}
	} else
		*idx = ipc_idr_alloc(ids, new);
}

static int ipc_reserveid(struct ipc_ids *ids, struct kern_ipc_perm *new,
			 int requested_id)
{
	uid_t euid;
	gid_t egid;
	int lid, id, err;

	spin_lock_init(&new->lock);
	new->deleted = 0;
	rcu_read_lock();

	spin_lock(&new->lock);

	lid = ipcid_to_idx(requested_id);

	err = krg_ipc_get_this_id(ids, lid);
	if (err)
		goto out;

	err = idr_pre_get(&ids->ipcs_idr, GFP_KERNEL);
	if (!err) {
		err = -ENOMEM;
		goto out;
	}

	err = idr_get_new_above(&ids->ipcs_idr, new, lid, &id);
	if (err)
		goto out_free_krg_id;

	if (lid != id) {
		err = -EINVAL;
		goto out_free_idr_id;
	}

	ids->in_use++;

	current_euid_egid(&euid, &egid);
	new->cuid = new->uid = euid;
	new->gid = new->cgid = egid;

	new->seq = (requested_id - lid) / SEQ_MULTIPLIER;

	if (ids->seq <= new->seq)
		ids->seq = new->seq+1;

	if (ids->seq > ids->seq_max)
		ids->seq = 0;

	new->id = requested_id;

	return requested_id;

out_free_idr_id:
	idr_remove(&ids->ipcs_idr, id);
out_free_krg_id:
	krg_ipc_rmid(ids, lid);
out:
	spin_unlock(&new->lock);
	rcu_read_unlock();

	return err;
}
#endif

/**
 *	ipc_addid 	-	add an IPC identifier
 *	@ids: IPC identifier set
 *	@new: new IPC permission set
 *	@size: limit for the number of used ids
 *
 *	Add an entry 'new' to the IPC ids idr. The permissions object is
 *	initialised and the first free entry is set up and the id assigned
 *	is returned. The 'new' entry is returned in a locked state on success.
 *	On failure the entry is not locked and a negative err-code is returned.
 *
 *	Called with ipc_ids.rwsem held as a writer.
 */
 
#ifdef CONFIG_KRG_IPC
int ipc_addid(struct ipc_ids *ids, struct kern_ipc_perm *new, int limit,
	      int requested_id)
#else
int ipc_addid(struct ipc_ids *ids, struct kern_ipc_perm *new, int limit)
#endif
{
	kuid_t euid;
	kgid_t egid;
	int idx;

	if (limit > ipc_mni)
		limit = ipc_mni;

	if (ids->in_use >= limit)
		return -ENOSPC;

#ifdef CONFIG_KRG_IPC
	if (requested_id != -1)
		return ipc_reserveid(ids, new, requested_id);
#endif

	idr_preload(GFP_KERNEL);

	spin_lock_init(&new->lock);
	new->deleted = false;
	rcu_read_lock();
	spin_lock(&new->lock);

	current_euid_egid(&euid, &egid);
	new->cuid = new->uid = euid;
	new->gid = new->cgid = egid;

#ifdef CONFIG_KRG_IPC
	krg_idr_get_new(ids, new, &idx);
#else
	idx = ipc_idr_alloc(ids, new);
#endif
	if (idx < 0) {
		spin_unlock(&new->lock);
		rcu_read_unlock();
		return idx;
	}

	ids->in_use++;
	if (idx > ids->max_idx)
		ids->max_idx = idx;
	return idx;
}

#ifdef CONFIG_KRG_IPC
int local_ipc_reserveid(struct ipc_ids* ids, struct kern_ipc_perm* new,
                        int limit)
{
	int original_idx, idx, err;

	if (limit > ipc_mni)
		limit = ipc_mni;

	if (ids->in_use >= limit) {
		/* IPC quota is not clusterwide, returning an error here
		   might lead to kernel crash within the IO linker */
		printk("%s:%d - Number of Kerrighed IPC objects is locally"
		       " exceeding quota (%d >= %d)\n",
		       __PRETTY_FUNCTION__, __LINE__,
		       ids->in_use, limit);
		/*return -ENOSPC;*/
	}

	err = idr_pre_get(&ids->ipcs_idr, GFP_KERNEL);
	if (!err)
		return -ENOMEM;

	spin_lock_init(&new->lock);

	new->deleted = 0;

	rcu_read_lock();

	spin_lock(&new->lock);

	original_idx = ipcid_to_idx(new->id);

	BUG_ON(new->id != SEQ_MULTIPLIER * new->seq + original_idx);

	err = idr_get_new_above(&ids->ipcs_idr, new, original_idx, &idx);

	if (err)
		goto error;

	if (original_idx != idx) {
		idr_remove(&ids->ipcs_idr, idx);
		err = -EINVAL;
		goto error;
	}

	ids->in_use++;

	if (ids->seq <= new->seq)
		ids->seq = new->seq+1;

	if (ids->seq > ids->seq_max)
		ids->seq = 0;

	return 0;

error:
	spin_unlock(&new->lock);
	return err;
}
#endif

/**
 * ipcget_new -	create a new ipc object
 * @ns: ipc namespace
 * @ids: ipc identifer set
 * @ops: the actual creation routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget, sys_semget() and sys_shmget()
 * when the key is IPC_PRIVATE.
 */
static int ipcget_new(struct ipc_namespace *ns, struct ipc_ids *ids,
		struct ipc_ops *ops, struct ipc_params *params)
{
	int err;

	down_write(&ids->rwsem);
	err = ops->getnew(ns, params);
	up_write(&ids->rwsem);
	return err;
}

/**
 * ipc_check_perms - check security and permissions for an ipc object
 * @ns: ipc namespace
 * @ipcp: ipc permission set
 * @ops: the actual security routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget(), sys_semget() and sys_shmget()
 * when the key is not IPC_PRIVATE and that key already exists in the
 * ds IDR.
 *
 * On success, the ipc id is returned.
 *
 * It is called with ipc_ids.rwsem and ipcp->lock held.
 */
static int ipc_check_perms(struct ipc_namespace *ns,
			   struct kern_ipc_perm *ipcp,
			   struct ipc_ops *ops,
			   struct ipc_params *params)
{
	int err;

	if (ipcperms(ns, ipcp, params->flg))
		err = -EACCES;
	else {
		err = ops->associate(ipcp, params->flg);
		if (!err)
			err = ipcp->id;
	}

	return err;
}

/**
 * ipcget_public - get an ipc object or create a new one
 * @ns: ipc namespace
 * @ids: ipc identifer set
 * @ops: the actual creation routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget, sys_semget() and sys_shmget()
 * when the key is not IPC_PRIVATE.
 * It adds a new entry if the key is not found and does some permission
 * / security checkings if the key is found.
 *
 * On success, the ipc id is returned.
 */
static int ipcget_public(struct ipc_namespace *ns, struct ipc_ids *ids,
		struct ipc_ops *ops, struct ipc_params *params)
{
	struct kern_ipc_perm *ipcp;
	int flg = params->flg;
	int err;

	/*
	 * Take the lock as a writer since we are potentially going to add
	 * a new entry + read locks are not "upgradable"
	 */
	down_write(&ids->rwsem);
	ipcp = ipc_findkey(ids, params->key);
	if (ipcp == NULL) {
		/* key not used */
		if (!(flg & IPC_CREAT))
			err = -ENOENT;
		else
			err = ops->getnew(ns, params);
	} else {
		/* ipc object has been locked by ipc_findkey() */

		if (flg & IPC_CREAT && flg & IPC_EXCL)
			err = -EEXIST;
		else {
			err = 0;
			if (ops->more_checks)
				err = ops->more_checks(ipcp, params);
			if (!err)
				/*
				 * ipc_check_perms returns the IPC id on
				 * success
				 */
				err = ipc_check_perms(ns, ipcp, ops, params);
		}
		ipc_unlock(ipcp);
	}
	up_write(&ids->rwsem);

	return err;
}

/**
 * ipc_rmid - remove an ipc identifier
 * @ids: ipc identifier set
 * @ipcp: ipc perm structure containing the identifier to remove
 *
 * ipc_ids.rwsem (as a writer) and the spinlock for this ID are held
 * before this function is called, and remain locked on the exit.
 */
 
void ipc_rmid(struct ipc_ids *ids, struct kern_ipc_perm *ipcp)
{
	int idx = ipcid_to_idx(ipcp->id);

	idr_remove(&ids->ipcs_idr, idx);

	ids->in_use--;
	ipcp->deleted = true;

	if (unlikely(idx == ids->max_idx)) {
		do {
			idx--;
			if (idx == -1)
				break;
		} while (!idr_find(&ids->ipcs_idr, idx));
		ids->max_idx = idx;
	}
}

/**
 * ipc_rcu_alloc - allocate ipc and rcu space
 * @size: size desired
 *
 * Allocate memory for the rcu header structure +  the object.
 * Returns the pointer to the object or NULL upon failure.
 */
void *ipc_rcu_alloc(int size)
{
	/*
	 * We prepend the allocation with the rcu struct
	 */
	struct ipc_rcu *out = kvmalloc(sizeof(struct ipc_rcu) + size, GFP_KERNEL);
	if (unlikely(!out))
		return NULL;
	atomic_set(&out->refcount, 1);
	return out + 1;
}

int ipc_rcu_getref(void *ptr)
{
	struct ipc_rcu *p = ((struct ipc_rcu *)ptr) - 1;

	return atomic_inc_not_zero(&p->refcount);
}

void ipc_rcu_putref(void *ptr, void (*func)(struct rcu_head *head))
{
	struct ipc_rcu *p = ((struct ipc_rcu *)ptr) - 1;

	if (!atomic_dec_and_test(&p->refcount))
		return;

	call_rcu(&p->rcu, func);
}

void ipc_rcu_free(struct rcu_head *head)
{
	struct ipc_rcu *p = container_of(head, struct ipc_rcu, rcu);

	kvfree(p);
}

/**
 * ipcperms - check ipc permissions
 * @ns: ipc namespace
 * @ipcp: ipc permission set
 * @flag: desired permission set
 *
 * Check user, group, other permissions for access
 * to ipc resources. return 0 if allowed
 *
 * @flag will most probably be 0 or S_...UGO from <linux/stat.h>
 */
int ipcperms(struct ipc_namespace *ns, struct kern_ipc_perm *ipcp, short flag)
{
	kuid_t euid = current_euid();
	int requested_mode, granted_mode;

	audit_ipc_obj(ipcp);
	requested_mode = (flag >> 6) | (flag >> 3) | flag;
	granted_mode = ipcp->mode;
	if (uid_eq(euid, ipcp->cuid) ||
	    uid_eq(euid, ipcp->uid))
		granted_mode >>= 6;
	else if (in_group_p(ipcp->cgid) || in_group_p(ipcp->gid))
		granted_mode >>= 3;
	/* is there some bit set in requested_mode but not in granted_mode? */
	if ((requested_mode & ~granted_mode & 0007) && 
	    !ns_capable(ns->user_ns, CAP_IPC_OWNER))
		return -1;

	return security_ipc_permission(ipcp, flag);
}

/*
 * Functions to convert between the kern_ipc_perm structure and the
 * old/new ipc_perm structures
 */

/**
 * kernel_to_ipc64_perm	- convert kernel ipc permissions to user
 * @in: kernel permissions
 * @out: new style ipc permissions
 *
 * Turn the kernel object @in into a set of permissions descriptions
 * for returning to userspace (@out).
 */
void kernel_to_ipc64_perm(struct kern_ipc_perm *in, struct ipc64_perm *out)
{
	out->key	= in->key;
	out->uid	= from_kuid_munged(current_user_ns(), in->uid);
	out->gid	= from_kgid_munged(current_user_ns(), in->gid);
	out->cuid	= from_kuid_munged(current_user_ns(), in->cuid);
	out->cgid	= from_kgid_munged(current_user_ns(), in->cgid);
	out->mode	= in->mode;
	out->seq	= in->seq;
}

/**
 * ipc64_perm_to_ipc_perm - convert new ipc permissions to old
 * @in: new style ipc permissions
 * @out: old style ipc permissions
 *
 * Turn the new style permissions object @in into a compatibility
 * object and store it into the @out pointer.
 */
void ipc64_perm_to_ipc_perm(struct ipc64_perm *in, struct ipc_perm *out)
{
	out->key	= in->key;
	SET_UID(out->uid, in->uid);
	SET_GID(out->gid, in->gid);
	SET_UID(out->cuid, in->cuid);
	SET_GID(out->cgid, in->cgid);
	out->mode	= in->mode;
	out->seq	= in->seq;
}

/**
 * ipc_obtain_object
 * @ids: ipc identifier set
 * @id: ipc id to look for
 *
 * Look for an id in the ipc ids idr and return associated ipc object.
 *
 * Call inside the RCU critical section.
 * The ipc object is *not* locked on exit.
 */
struct kern_ipc_perm *ipc_obtain_object(struct ipc_ids *ids, int id)
{
	struct kern_ipc_perm *out;
	int idx = ipcid_to_idx(id);

	out = idr_find(&ids->ipcs_idr, idx);
	if (!out)
		return ERR_PTR(-EINVAL);

	return out;
}

/**
 * ipc_lock - lock an ipc structure without rwsem held
 * @ids: ipc identifier set
 * @id: ipc id to look for
 *
 * Look for an id in the ipc ids idr and lock the associated ipc object.
 *
 * The ipc object is locked on successful exit.
 */
#ifdef CONFIG_KRG_IPC
struct kern_ipc_perm *local_ipc_lock(struct ipc_ids *ids, int id)
#else
struct kern_ipc_perm *ipc_lock(struct ipc_ids *ids, int id)
#endif
{
	struct kern_ipc_perm *out;

	rcu_read_lock();
	out = ipc_obtain_object(ids, id);
	if (IS_ERR(out))
		goto err1;

	spin_lock(&out->lock);

	/* ipc_rmid() may have already freed the ID while ipc_lock
	 * was spinning: here verify that the structure is still valid
	 */
	if (ipc_valid_object(out))
		return out;

	spin_unlock(&out->lock);
	out = ERR_PTR(-EINVAL);
err1:
	rcu_read_unlock();
	return out;
}

/**
 * ipc_obtain_object_check
 * @ids: ipc identifier set
 * @id: ipc id to look for
 *
 * Similar to ipc_obtain_object() but also checks
 * the ipc object reference counter.
 *
 * Call inside the RCU critical section.
 * The ipc object is *not* locked on exit.
 */
struct kern_ipc_perm *ipc_obtain_object_check(struct ipc_ids *ids, int id)
{
	struct kern_ipc_perm *out = ipc_obtain_object(ids, id);

	if (IS_ERR(out))
		goto out;

	if (ipc_checkid(out, id))
		return ERR_PTR(-EIDRM);
out:
	return out;
}

#ifdef CONFIG_KRG_IPC
struct kern_ipc_perm *ipc_lock(struct ipc_ids *ids, int id)
{
	if (is_krg_ipc(ids))
		return ids->krgops->ipc_lock(ids, id);

	return local_ipc_lock(ids, id);
}
#endif

#ifdef CONFIG_KRG_IPC
void local_ipc_unlock(struct kern_ipc_perm *perm)
{
	ipc_unlock_object(perm);
	rcu_read_unlock();
}

void ipc_unlock(struct kern_ipc_perm *perm)
{
	if (perm->krgops)
		perm->krgops->ipc_unlock(perm);
	else
		local_ipc_unlock(perm);
}
#endif

/**
 * ipcget - Common sys_*get() code
 * @ns: namsepace
 * @ids: ipc identifier set
 * @ops: operations to be called on ipc object creation, permission checks
 *       and further checks
 * @params: the parameters needed by the previous operations.
 *
 * Common routine called by sys_msgget(), sys_semget() and sys_shmget().
 */
int ipcget(struct ipc_namespace *ns, struct ipc_ids *ids,
			struct ipc_ops *ops, struct ipc_params *params)
{
#ifdef CONFIG_KRG_IPC
	params->requested_id = -1;
#endif
	if (params->key == IPC_PRIVATE)
		return ipcget_new(ns, ids, ops, params);
	else
		return ipcget_public(ns, ids, ops, params);
}

/**
 * ipc_update_perm - update the permissions of an ipc object
 * @in:  the permission given as input.
 * @out: the permission of the ipc to set.
 */
int ipc_update_perm(struct ipc64_perm *in, struct kern_ipc_perm *out)
{
	kuid_t uid = make_kuid(current_user_ns(), in->uid);
	kgid_t gid = make_kgid(current_user_ns(), in->gid);
	if (!uid_valid(uid) || !gid_valid(gid))
		return -EINVAL;

	out->uid = uid;
	out->gid = gid;
	out->mode = (out->mode & ~S_IRWXUGO)
		| (in->mode & S_IRWXUGO);

	return 0;
}

/**
 * ipcctl_pre_down_nolock - retrieve an ipc and check permissions for some IPC_XXX cmd
 * @ns:  ipc namespace
 * @ids:  the table of ids where to look for the ipc
 * @id:   the id of the ipc to retrieve
 * @cmd:  the cmd to check
 * @perm: the permission to set
 * @extra_perm: one extra permission parameter used by msq
 *
 * This function does some common audit and permissions check for some IPC_XXX
 * cmd and is called from semctl_down, shmctl_down and msgctl_down.
 * It must be called without any lock held and
 *  - retrieves the ipc with the given id in the given table.
 *  - performs some audit and permission check, depending on the given cmd
 *  - returns a pointer to the ipc object or otherwise, the corresponding error.
 *
 * Call holding the both the rwsem and the rcu read lock.
 */
struct kern_ipc_perm *ipcctl_pre_down_nolock(struct ipc_namespace *ns,
					struct ipc_ids *ids, int id, int cmd,
					struct ipc64_perm *perm, int extra_perm)
{
	kuid_t euid;
	int err = -EPERM;
	struct kern_ipc_perm *ipcp;

	ipcp = ipc_obtain_object_check(ids, id);
	if (IS_ERR(ipcp)) {
		err = PTR_ERR(ipcp);
		goto err;
	}

	audit_ipc_obj(ipcp);
	if (cmd == IPC_SET)
		audit_ipc_set_perm(extra_perm, perm->uid,
				   perm->gid, perm->mode);

	euid = current_euid();
	if (uid_eq(euid, ipcp->cuid) || uid_eq(euid, ipcp->uid)  ||
	    ns_capable(ns->user_ns, CAP_SYS_ADMIN))
		return ipcp; /* successful lookup */
err:
	return ERR_PTR(err);
}

#ifdef CONFIG_ARCH_WANT_IPC_PARSE_VERSION


/**
 * ipc_parse_version - ipc call version
 * @cmd: pointer to command
 *
 * Return IPC_64 for new style IPC and IPC_OLD for old style IPC.
 * The @cmd value is turned from an encoding command and version into
 * just the command code.
 */
int ipc_parse_version(int *cmd)
{
	if (*cmd & IPC_64) {
		*cmd ^= IPC_64;
		return IPC_64;
	} else {
		return IPC_OLD;
	}
}

#endif /* CONFIG_ARCH_WANT_IPC_PARSE_VERSION */

#ifdef CONFIG_PROC_FS
struct ipc_proc_iter {
	struct ipc_namespace *ns;
	struct ipc_proc_iface *iface;
};

/*
 * This routine locks the ipc structure found at least at position pos.
 */
static struct kern_ipc_perm *sysvipc_find_ipc(struct ipc_ids *ids, loff_t pos,
					      loff_t *new_pos)
{
	struct kern_ipc_perm *ipc;
#ifdef CONFIG_KRG_IPC
	int total;

	total = ipc_get_maxid(ids);

	for (; pos <= total && pos < ipc_mni; pos++) {
		ipc = ipc_lock(ids, pos);
		if (!IS_ERR(ipc)) {
			*new_pos = pos + 1;
			return ipc;
		}
	}
#else
	int total, id;

	total = 0;
	for (id = 0; id < pos && total < ids->in_use; id++) {
		ipc = idr_find(&ids->ipcs_idr, id);
		if (ipc != NULL)
			total++;
	}

	if (total >= ids->in_use)
		return NULL;

	for (; pos < ipc_mni; pos++) {
		ipc = idr_find(&ids->ipcs_idr, pos);
		if (ipc != NULL) {
			*new_pos = pos + 1;
			rcu_read_lock();
			ipc_lock_object(ipc);
			return ipc;
		}
	}
#endif

	/* Out of range - return NULL to terminate iteration */
	return NULL;
}

static void *sysvipc_proc_next(struct seq_file *s, void *it, loff_t *pos)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct kern_ipc_perm *ipc = it;

	/* If we had an ipc id locked before, unlock it */
	if (ipc && ipc != SEQ_START_TOKEN)
		ipc_unlock(ipc);

	return sysvipc_find_ipc(&iter->ns->ids[iface->ids], *pos, pos);
}

/*
 * File positions: pos 0 -> header, pos n -> ipc id = n - 1.
 * SeqFile iterator: iterator value locked ipc pointer or SEQ_TOKEN_START.
 */
static void *sysvipc_proc_start(struct seq_file *s, loff_t *pos)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct ipc_ids *ids;

	ids = &iter->ns->ids[iface->ids];

	/*
	 * Take the lock - this will be released by the corresponding
	 * call to stop().
	 */
	down_read(&ids->rwsem);

	/* pos < 0 is invalid */
	if (*pos < 0)
		return NULL;

	/* pos == 0 means header */
	if (*pos == 0)
		return SEQ_START_TOKEN;

	/* Find the (pos-1)th ipc */
	return sysvipc_find_ipc(ids, *pos - 1, pos);
}

static void sysvipc_proc_stop(struct seq_file *s, void *it)
{
	struct kern_ipc_perm *ipc = it;
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct ipc_ids *ids;

	/* If we had a locked structure, release it */
	if (ipc && ipc != SEQ_START_TOKEN)
		ipc_unlock(ipc);

	ids = &iter->ns->ids[iface->ids];
	/* Release the lock we took in start() */
	up_read(&ids->rwsem);
}

static int sysvipc_proc_show(struct seq_file *s, void *it)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;

	if (it == SEQ_START_TOKEN)
		return seq_puts(s, iface->header);

	return iface->show(s, it);
}

static const struct seq_operations sysvipc_proc_seqops = {
	.start = sysvipc_proc_start,
	.stop  = sysvipc_proc_stop,
	.next  = sysvipc_proc_next,
	.show  = sysvipc_proc_show,
};

static int sysvipc_proc_open(struct inode *inode, struct file *file)
{
	int ret;
	struct seq_file *seq;
	struct ipc_proc_iter *iter;

	ret = -ENOMEM;
	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		goto out;

	ret = seq_open(file, &sysvipc_proc_seqops);
	if (ret)
		goto out_kfree;

	seq = file->private_data;
	seq->private = iter;

	iter->iface = PDE_DATA(inode);
	iter->ns    = get_ipc_ns(current->nsproxy->ipc_ns);
out:
	return ret;
out_kfree:
	kfree(iter);
	goto out;
}

static int sysvipc_proc_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct ipc_proc_iter *iter = seq->private;
	put_ipc_ns(iter->ns);
	return seq_release_private(inode, file);
}

static const struct file_operations sysvipc_proc_fops = {
	.open    = sysvipc_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = sysvipc_proc_release,
};
#endif /* CONFIG_PROC_FS */

#ifdef CONFIG_KRG_IPC
void unlink_queue(struct sem_array *sma, struct sem_queue *q)
{
	list_del(&q->list);
	if (q->nsops > 1)
		sma->complex_count--;
}

void msg_rcu_free(struct rcu_head *head)
{
	struct ipc_rcu *p = container_of(head, struct ipc_rcu, rcu);
	struct msg_queue *msq = ipc_rcu_to_struct(p);

	security_msg_queue_free(msq);
	ipc_rcu_free(head);
}

void sem_rcu_free(struct rcu_head *head)
{
	struct ipc_rcu *p = container_of(head, struct ipc_rcu, rcu);
	struct sem_array *sma = ipc_rcu_to_struct(p);

	security_sem_free(sma);
	ipc_rcu_free(head);
}

int is_krg_ipc(struct ipc_ids *ids)
{
	if (ids->krgops)
		return 1;

	return 0;
}

int init_keripc(void)
{
	printk("KrgIPC initialisation : start\n");

	ipcmap_object_cachep = kmem_cache_create("ipcmap_object",
						 sizeof(ipcmap_object_t),
						 0, SLAB_PANIC, NULL);
	register_io_linker (IPCMAP_LINKER, &ipcmap_linker);

	ipc_handler_init();

	msg_handler_init();

	sem_handler_init();

	shm_handler_init();

	printk("KrgIPC initialisation done\n");

	return 0;
}

void cleanup_keripc(void)
{
	ipc_handler_finalize();
}

#endif
