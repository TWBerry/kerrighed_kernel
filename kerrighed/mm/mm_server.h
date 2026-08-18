/** Kerrighed MM Server.
 *  @file mm_server.h
 *
 *  @author Renaud Lottiaux
 */

#ifndef __MM_SERVER__
#define __MM_SERVER__



/*--------------------------------------------------------------------------*
 *                                                                          *
 *                                  TYPES                                   *
 *                                                                          *
 *--------------------------------------------------------------------------*/



typedef struct mm_vma_msg {
	unique_id_t mm_id;
	unsigned long start;
	size_t len;
	unsigned long flags;
	unsigned long vm_flags;
	unsigned long pgoff;
	unsigned long old_len;
	unsigned long new_len;
	unsigned long new_addr;
	unsigned long result;
	unsigned long brk;
	unsigned long prot;
} mm_vma_msg_t;

typedef struct mm_munmap_msg {
	unique_id_t mm_id;
	unsigned long start;
	size_t len;
} mm_munmap_msg_t;



/*--------------------------------------------------------------------------*
 *                                                                          *
 *                              EXTERN FUNCTIONS                            *
 *                                                                          *
 *--------------------------------------------------------------------------*/



void mm_server_init (void);
void mm_server_finalize (void);


#endif // __MM_SERVER__
