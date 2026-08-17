#ifndef __KRG_NAMESPACE_H__
#define __KRG_NAMESPACE_H__

#include <linux/nsproxy.h>
#include <linux/completion.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <asm/atomic.h>

struct task_struct;

/*
 * Cluster namespace lifetime state.
 *
 * The original Kerrighed structure also carried struct rpc_communicator.
 * The Linux 3.10 port uses a global KRGRPC communication model, so transport
 * attachment is intentionally deferred until the hotplug/connection lifecycle
 * is restored on top of that model.
 */
struct krg_namespace {
	atomic_t count;
	struct nsproxy root_nsproxy;
	struct user_namespace *root_user_ns;
	struct task_struct *root_task;
	struct completion root_task_in_exit;
	struct completion root_task_continue_exit;
	struct rcu_head rcu;
	struct work_struct free_work;
};

int copy_krg_ns(struct task_struct *task, struct nsproxy *new);
void free_krg_ns(struct krg_namespace *ns);

struct krg_namespace *find_get_krg_ns(void);

static inline void get_krg_ns(struct krg_namespace *ns)
{
	atomic_inc(&ns->count);
}

static inline void put_krg_ns(struct krg_namespace *ns)
{
	if (atomic_dec_and_test(&ns->count))
		free_krg_ns(ns);
}

bool can_create_krg_ns(unsigned long flags);

int krg_set_cluster_creator(void __user *arg);
int hotplug_namespace_init(void);

/* Restored with the hotplug cluster lifecycle, not namespace core. */
void krg_ns_root_exit(struct krg_namespace *ns);

#endif /* __KRG_NAMESPACE_H__ */
