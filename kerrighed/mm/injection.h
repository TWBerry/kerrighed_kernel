/** Memory injection. */
#ifndef __KERRIGHED_MEMORY_INJECTION_H__
#define __KERRIGHED_MEMORY_INJECTION_H__

#include <linux/mm_types.h>
#include <kerrighed/sys/types.h>

#define FREE_MEM 1
#define LOW_MEM 2
#define OUT_OF_MEM 3

extern int node_mem_usage[KERRIGHED_MAX_NODES];

void mm_injection_init(void);
void mm_injection_finalize(void);
void krg_notify_mem(int mem_usage);
int try_to_flush_page(struct page *page);

#endif
