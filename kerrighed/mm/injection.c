/** Memory injection code, ported to the Linux 3.10-era Kerrighed tree. */
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/page-flags.h>
#include <linux/module.h>
#include <linux/interrupt.h>

#include <net/krgrpc/rpc.h>
#include <net/krgrpc/rpcid.h>
#include <kddm/kddm.h>
#include <kddm/kddm_flush_object.h>
#include <kerrighed/hotplug.h>
#include <kerrighed/krginit.h>
#include <kerrighed/mm.h>

#include "injection.h"
#include "mm_struct.h"

static kerrighed_node_t last_chosen_node = KERRIGHED_NODE_ID_NONE;
int node_mem_usage[KERRIGHED_MAX_NODES];
EXPORT_SYMBOL(node_mem_usage);
static atomic_t mem_usage_notified = ATOMIC_INIT(FREE_MEM);
static struct tasklet_struct notify_tasklet;
static unsigned long low_mem_limit;
static unsigned long low_mem_limit_delta;

static kerrighed_node_t select_injection_node_rr(void)
{
    int pass;
    int online = num_online_krgnodes();
    kerrighed_node_t start;

    if (online <= 1)
        return KERRIGHED_NODE_ID_NONE;

    start = last_chosen_node == KERRIGHED_NODE_ID_NONE ?
        kerrighed_node_id : last_chosen_node;

    for (pass = 0; pass < 2; pass++) {
        kerrighed_node_t node = start;
        int i;

        for (i = 0; i < online; i++) {
            node = krgnode_next_online_in_ring(node);
            if (node == kerrighed_node_id)
                continue;

            if (node_mem_usage[node] == FREE_MEM ||
                (pass && node_mem_usage[node] == LOW_MEM)) {
                last_chosen_node = node;
                return node;
            }
        }
    }

    return KERRIGHED_NODE_ID_NONE;
}

static void handle_notify_low_mem(struct rpc_desc *desc, void *msg, size_t size)
{
    kerrighed_node_t nodeid = desc->client;
    int old_val;
    int new_val;

    if (size != sizeof(new_val))
        return;

    new_val = *(int *)msg;
    old_val = node_mem_usage[nodeid];
    node_mem_usage[nodeid] = new_val;

    if (new_val == OUT_OF_MEM)
        rpc_enable_lowmem_mode(nodeid);
    else if (old_val == OUT_OF_MEM)
        rpc_disable_lowmem_mode(nodeid);
}

static void do_notify_mem(unsigned long unused)
{
    krgnodemask_t nodes;
    int state = atomic_read(&mem_usage_notified);

    krgnodes_copy(nodes, krgnode_online_map);
    krgnode_clear(kerrighed_node_id, nodes);
    if (!krgnodes_empty(nodes))
        rpc_async_m(RPC_MM_NOTIFY_LOW_MEM, &nodes, &state, sizeof(state));
}

void krg_notify_mem(int mem_usage)
{
    long free_pages, cache_pages;
    int old_val;

    if (!mem_usage) {
        free_pages = nr_free_pages();
        if (free_pages < low_mem_limit) {
            cache_pages = global_page_state(NR_FILE_PAGES) -
                total_swapcache_pages();
            if (cache_pages < low_mem_limit)
                mem_usage = OUT_OF_MEM;
            else if (atomic_read(&mem_usage_notified) != OUT_OF_MEM)
                mem_usage = LOW_MEM;
        }

        if (free_pages > low_mem_limit + low_mem_limit_delta)
            mem_usage = FREE_MEM;
        if (!mem_usage)
            return;
    }

    old_val = atomic_xchg(&mem_usage_notified, mem_usage);
    if (old_val == mem_usage)
        return;

    if (mem_usage == OUT_OF_MEM)
        rpc_enable_local_lowmem_mode();
    else if (old_val == OUT_OF_MEM)
        rpc_disable_local_lowmem_mode();

    tasklet_hi_schedule(&notify_tasklet);
}
EXPORT_SYMBOL(krg_notify_mem);

struct flush_walk_arg {
    struct mm_struct *mm;
    unsigned long address;
};

/*
 * Preserve the original Kerrighed invariant for fork/shared anonymous pages:
 * unmap mappings one at a time until only one remains, then flush the KDDM
 * object through the mm owning that last mapping.
 */
static int prepare_last_mapping(struct page *page, struct vm_area_struct *vma,
                                unsigned long address, void *arg)
{
    struct flush_walk_arg *fw = arg;
    int ret;

    if (page_mapcount(page) > 1) {
        ret = krg_try_to_unmap_one(page, vma, address);
        if (ret != SWAP_AGAIN)
            return ret;
        return SWAP_AGAIN;
    }

    if (page_mapcount(page) != 1)
        return SWAP_AGAIN;

    fw->mm = vma->vm_mm;
    fw->address = address;
    mmget(fw->mm);

    /* Stop rmap_walk(): the remaining VMA is revalidated after unlocking. */
    return SWAP_SUCCESS;
}

static int flush_last_mapping(struct page *page, struct mm_struct *mm,
                              unsigned long address)
{
    struct vm_area_struct *vma;
    struct kddm_set *set;
    kerrighed_node_t dest_node;
    objid_t objid;
    pte_t *pte;
    spinlock_t *ptl;
#ifdef CONFIG_KRG_EPM
    struct task_struct *krg_cur;
#endif
    int ret;

    down_read(&mm->mmap_sem);
    vma = find_vma(mm, address);
    if (!vma || address < vma->vm_start || address >= vma->vm_end) {
        up_read(&mm->mmap_sem);
        return SWAP_AGAIN;
    }

    set = mm->anon_vma_kddm_set;
    if (!set || krgnodes_empty(mm->copyset)) {
        up_read(&mm->mmap_sem);
        return SWAP_FAIL;
    }

    pte = page_check_address(page, mm, address, &ptl, 1);
    if (!pte) {
        up_read(&mm->mmap_sem);
        return SWAP_AGAIN;
    }

    if (vma->vm_flags & VM_LOCKED) {
        pte_unmap_unlock(pte, ptl);
        up_read(&mm->mmap_sem);
        return SWAP_MLOCK;
    }

    if (ptep_clear_flush_young(vma, address, pte)) {
        pte_unmap_unlock(pte, ptl);
        up_read(&mm->mmap_sem);
        return SWAP_FAIL;
    }

    pte_unmap_unlock(pte, ptl);
    up_read(&mm->mmap_sem);

    /* KerMM uses the virtual page number as the anonymous KDDM objid. */
    objid = address >> PAGE_SHIFT;
    dest_node = PageMigratable(page) ? select_injection_node_rr() :
        KERRIGHED_NODE_ID_NONE;

    SetPageSwapCache(page);
#ifdef CONFIG_KRG_EPM
    krg_current_save(krg_cur);
#endif
    ret = _kddm_flush_object(set, objid, dest_node);
#ifdef CONFIG_KRG_EPM
    krg_current_restore(krg_cur);
#endif
    ClearPageSwapCache(page);

    if (ret) {
        if (ret == -ENOSPC && dest_node == KERRIGHED_NODE_ID_NONE)
            return SWAP_FLUSH_FAIL;
        return SWAP_FAIL;
    }

    ClearPageMigratable(page);
    return SWAP_SUCCESS;
}

int try_to_flush_page(struct page *page)
{
    struct flush_walk_arg fw = { };
    struct rmap_walk_control rwc = {
        .arg = &fw,
        .rmap_one = prepare_last_mapping,
        .anon_lock = page_lock_anon_vma_read,
    };
    int ret;

    krg_notify_mem(OUT_OF_MEM);

    if (!PageAnon(page) || PageKsm(page))
        return SWAP_FAIL;

    ret = rmap_walk(page, &rwc);
    if (!fw.mm)
        return ret == SWAP_SUCCESS ? SWAP_AGAIN : ret;

    ret = flush_last_mapping(page, fw.mm, fw.address);
    mmput(fw.mm);
    return ret;
}
EXPORT_SYMBOL(try_to_flush_page);

static void init_low_mem_limit(void)
{
    struct zone *zone;

    low_mem_limit = 0;
    for_each_zone(zone)
        low_mem_limit += low_wmark_pages(zone);
    low_mem_limit *= 2;
    low_mem_limit_delta = low_mem_limit;
}

void mm_injection_init(void)
{
    int i;

    tasklet_init(&notify_tasklet, do_notify_mem, 0);
    init_low_mem_limit();
    rpc_register_void(RPC_MM_NOTIFY_LOW_MEM, handle_notify_low_mem, 0);
    for (i = 0; i < KERRIGHED_MAX_NODES; i++)
        node_mem_usage[i] = FREE_MEM;
}

void mm_injection_finalize(void)
{
    tasklet_kill(&notify_tasklet);
}
