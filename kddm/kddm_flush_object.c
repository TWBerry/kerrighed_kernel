/** KDDM flush object
 *  @file kddm_flush_object.c
 *
 *  Implementation of KDDM flush object function.
 *
 *  Copyright (C) 2001-2006, INRIA, Universite de Rennes 1, EDF.
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 */
#include <linux/module.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <kerrighed/debug.h>

#include <kddm/kddm.h>
#include <kddm/object_server.h>
#include "protocol_action.h"


/** Remove an object from local physical memory.
 *  @author Renaud Lottiaux
 *
 *  @param set        KDDM set hosting the object.
 *  @param objid      Identifier of the object to flush.
 *  @param dest       Identifier of the node to send object to if needed.
 *  @return           0 if everything OK, -1 otherwise.
 *
 *  Remove an object from local memory and send it to the given node if
 *  needed. At least one copy is kept somewhere in the cluster memory. The
 *  object is never swaped to disk through this function.
 */
int _kddm_flush_object(struct kddm_set *set,
		       objid_t objid,
		       kerrighed_node_t dest)
{
	kerrighed_node_t dest_from_copyset;
	struct kddm_obj *obj_entry;
	int res = -1;

	inc_flush_object_counter(set);

	obj_entry = __get_kddm_obj_entry(set, objid);
	if (obj_entry == NULL)
		return res;

try_again:
	switch (OBJ_STATE(obj_entry)) {
	case READ_COPY:
		if (object_frozen_or_pinned(obj_entry, set)) {
			__sleep_on_kddm_obj(set, obj_entry, objid, 0);
			goto try_again;
		}

		/* There exist another copy in the cluster.
		   Just invalidate the local one */
		destroy_kddm_obj_entry(set, obj_entry, objid, 0);
		send_invalidation_ack(set, objid, get_prob_owner(obj_entry));
		res = 0;
		goto exit_no_unlock;

	case READ_OWNER:
		if (object_frozen_or_pinned(obj_entry, set)) {
			__sleep_on_kddm_obj(set, obj_entry, objid, 0);
			goto try_again;
		}

		REMOVE_FROM_SET(COPYSET(obj_entry), kerrighed_node_id);
		if (SET_IS_EMPTY(COPYSET(obj_entry))) {
			/* I'm owner of the only existing object in the
			 * cluster. Let's inject it ! */
			goto send_copy;
		}
		/* There exist at least another copy. Send ownership */
		dest_from_copyset = choose_injection_node_in_copyset(obj_entry);
		BUG_ON (dest_from_copyset == -1);
		send_change_ownership_req(set, obj_entry, objid,
					  dest_from_copyset,
					  &obj_entry->master_obj);

		/* Wait for ack... The object is invalidated by the ack
		   handler */

		__sleep_on_kddm_obj(set, obj_entry, objid, 0);

		destroy_kddm_obj_entry(set, obj_entry, objid, 0);
		res = 0;
		goto exit_no_unlock;

	case WRITE_GHOST:
	case WRITE_OWNER:
		/* Local copy is the only one. Let's inject it ! */
 send_copy:
		if (object_frozen_or_pinned(obj_entry, set)) {
			__sleep_on_kddm_obj(set, obj_entry, objid, 0);
			goto try_again;
		}

		send_copy_on_write(set, obj_entry, objid, dest,
				   KDDM_REMOVE_ON_ACK);
		res = 0;
		goto exit_no_unlock;

	case WAIT_ACK_INV:
	case WAIT_OBJ_RM_DONE:
	case WAIT_OBJ_RM_ACK:
	case WAIT_OBJ_RM_ACK2:
		res = 0;
		break;

	case INV_OWNER:
	case INV_COPY:
	case INV_NO_COPY:
	case WAIT_ACK_WRITE:
	case WAIT_CHG_OWN_ACK:
	case WAIT_RECEIVED_ACK:
	case WAIT_OBJ_READ:
	case WAIT_OBJ_WRITE:
	case INV_FILLING:
		break;

	default:
		STATE_MACHINE_ERROR(set->id, objid, obj_entry);
		break;
	}
	put_kddm_obj_entry(set, obj_entry, objid);

exit_no_unlock:

	return res;
}
EXPORT_SYMBOL(_kddm_flush_object);



int kddm_flush_object(struct kddm_ns *ns, kddm_set_id_t set_id, objid_t objid,
		      kerrighed_node_t dest)
{
	struct kddm_set *set;
	int res;

	set = _find_get_kddm_set (ns, set_id);
	res = _kddm_flush_object(set, objid, dest);
	put_kddm_set(set);

	return res;
}
EXPORT_SYMBOL(kddm_flush_object);


/*
 * The original Kerrighed tree used __for_each_kddm_object_safe(), whose
 * iterator ABI was removed during the Linux 3.10 KDDM port.  Reintroducing
 * that old iterator ABI would undo the current KDDM tree design.
 *
 * Snapshot object ids while the current iterator owns the set/table locks,
 * then flush objects one-by-one outside the walk.  __get_kddm_obj_entry()
 * reacquires the normal object/path lock, so removal cannot invalidate the
 * tree iterator and we preserve the original flush-set semantics.
 */
struct kddm_flush_set_entry {
	struct list_head list;
	objid_t objid;
};

struct kddm_flush_set_snapshot {
	struct list_head objects;
	int oom;
};

static int collect_kddm_flush_objid(unsigned long objid, void *object,
				    void *data)
{
	struct kddm_flush_set_snapshot *snapshot = data;
	struct kddm_flush_set_entry *entry;

	if (snapshot->oom)
		return 0;

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		snapshot->oom = 1;
		return 0;
	}

	entry->objid = objid;
	list_add_tail(&entry->list, &snapshot->objects);
	return 0;
}

static void __kddm_flush_set_object(struct kddm_set *set, objid_t objid,
				    int (*f)(struct kddm_set *,
				             objid_t, struct kddm_obj *,
				             void *),
				    void *data)
{
	kerrighed_node_t dest, new_owner;
	struct kddm_obj *obj_entry;

	obj_entry = __get_kddm_obj_entry(set, objid);
	if (!obj_entry)
		return;

retry:
	if (object_frozen_or_pinned(obj_entry, set)) {
		__sleep_on_kddm_obj(set, obj_entry, objid, 0);
		goto retry;
	}

	switch (OBJ_STATE(obj_entry)) {
	case READ_COPY:
		send_invalidation_ack(set, objid, get_prob_owner(obj_entry));
		destroy_kddm_obj_entry(set, obj_entry, objid, 0);
		return;

	case READ_OWNER:
		REMOVE_FROM_SET(COPYSET(obj_entry), kerrighed_node_id);
		if (SET_IS_EMPTY(COPYSET(obj_entry)))
			goto send_copy;

		new_owner = f(set, objid, obj_entry, data);
		if (__krgnode_isset(new_owner, COPYSET(obj_entry)))
			dest = new_owner;
		else
			dest = choose_injection_node_in_copyset(obj_entry);
		BUG_ON(dest == KERRIGHED_NODE_ID_NONE);

		send_change_ownership_req(set, obj_entry, objid, dest,
				  &obj_entry->master_obj);
		__sleep_on_kddm_obj(set, obj_entry, objid, 0);
		destroy_kddm_obj_entry(set, obj_entry, objid, 0);
		return;

	case WRITE_GHOST:
	case WRITE_OWNER:
send_copy:
		dest = f(set, objid, obj_entry, data);
		BUG_ON(dest == KERRIGHED_NODE_ID_NONE);
		send_copy_on_write(set, obj_entry, objid, dest,
				   KDDM_REMOVE_ON_ACK);
		return;

	case WAIT_ACK_INV:
	case WAIT_OBJ_RM_DONE:
	case WAIT_OBJ_RM_ACK:
	case WAIT_OBJ_RM_ACK2:
	case WAIT_ACK_WRITE:
	case WAIT_CHG_OWN_ACK:
	case WAIT_RECEIVED_ACK:
	case WAIT_OBJ_READ:
	case WAIT_OBJ_WRITE:
	case INV_FILLING:
		put_kddm_obj_entry(set, obj_entry, objid);
		return;

	case INV_OWNER:
	case INV_COPY:
	case INV_NO_COPY:
		destroy_kddm_obj_entry(set, obj_entry, objid, 0);
		return;

	default:
		STATE_MACHINE_ERROR(set->id, objid, obj_entry);
		put_kddm_obj_entry(set, obj_entry, objid);
		return;
	}
}

void _kddm_flush_set(struct kddm_set *set,
		     int (*f)(struct kddm_set *, objid_t, struct kddm_obj *,
		              void *), void *data)
{
	struct kddm_flush_set_snapshot snapshot;
	struct kddm_flush_set_entry *entry, *tmp;

	INIT_LIST_HEAD(&snapshot.objects);
	snapshot.oom = 0;

	__for_each_kddm_object(set, collect_kddm_flush_objid, &snapshot);

	if (snapshot.oom) {
		list_for_each_entry_safe(entry, tmp, &snapshot.objects, list) {
			list_del(&entry->list);
			kfree(entry);
		}
		OOM;
		return;
	}

	list_for_each_entry_safe(entry, tmp, &snapshot.objects, list) {
		__kddm_flush_set_object(set, entry->objid, f, data);
		list_del(&entry->list);
		kfree(entry);
	}
}
EXPORT_SYMBOL(_kddm_flush_set);

void kddm_flush_set(struct kddm_ns *ns, kddm_set_id_t set_id,
		    int (*f)(struct kddm_set *, objid_t, struct kddm_obj *,
		             void *), void *data)
{
	struct kddm_set *set;

	set = _find_get_kddm_set(ns, set_id);
	if (!set)
		return;

	_kddm_flush_set(set, f, data);
	put_kddm_set(set);
}
EXPORT_SYMBOL(kddm_flush_set);
