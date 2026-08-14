/** /proc manager
 *  @file procfs.c
 *
 *  Copyright (C) 2001-2006, INRIA, Universite de Rennes 1, EDF.
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 */

#include <linux/mmzone.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <asm/uaccess.h>
#include <linux/swap.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/hashtable.h>

#include <kerrighed/procfs.h>
#include <kddm/kddm.h>
#include "kddm_bench.h"
#include <linux/slab.h>

/*  /proc/kerrighed/kddm          */
static struct proc_dir_entry *procfs_kddm;

/*  /proc/kerrighed/kddm/meminfo  */
static struct proc_dir_entry *procfs_meminfo;

/*  /proc/kerrighed/kddm/kddmstat */
static struct proc_dir_entry *procfs_setstat;

/*  /proc/kerrighed/kddm/bench */
static struct proc_dir_entry *procfs_bench;



void check_pages(void)
{
        struct page *page;
        pg_data_t *pgdat;
        unsigned long i, pb;
	unsigned long flags;

        for_each_online_pgdat(pgdat) {
                pgdat_resize_lock(pgdat, &flags);
                for (i = 0; i < pgdat->node_spanned_pages; ++i) {
			pb = 0;
                        page = pgdat_page_nr(pgdat, i);
			if (page_count(page) < 0) {
				printk ("Negative count\n");
				pb = 1;
			}
			if (page_mapcount(page) < 0) {
				printk ("Negative mapcount\n");
				pb = 1;
			}
			if (page_mapcount(page) > page_count(page)) {
				printk ("Count problem\n");
				pb = 1;
			}
			if (page_count(page) == 0) {
				if (page_mapcount(page) != 0) {
					printk ("Non null map count\n");
					pb = 1;
				}
				if (page->mapping != NULL) {
					printk ("Non null mapping\n");
					pb = 1;
				}
			}
			else {
				if (page_mapcount(page)) {
					if (page->mapping == NULL) {
						printk ("Null mapping\n");
						pb = 1;
					}
				}
				if (page->mapping) {
					if ((page_mapcount(page) == 0) &&
					    (PageAnon(page))) {
						printk ("Null mapcount\n");
						pb = 1;
					}
				}
			}
			if (pb)
				printk ("Page %p - count %d - map count %d - "
					"mapping %p - flags 0x%08lx\n", page,
					page_count(page),
					page_mapcount(page),
					page->mapping,
					page->flags);
                }
		printk ("%ld pages checked\n", i);
                pgdat_resize_unlock(pgdat, &flags);
        }
}



/****************************************************************************/
/*                                                                          */
/*                         /proc/kddminfo Management                        */
/*                                                                          */
/****************************************************************************/



static void *s_start(struct seq_file *m, loff_t *pos)
{
	struct kddm_set *set;
	unsigned long found;

	if (*pos == 0)
		seq_printf (m, "       Set ID           nr entries       nr objects     obj size      Set size     \n");

	/* Assumption: KDDM set id 0 is never used. */
	set = hashtable_find_next (kddm_def_ns->kddm_set_table, *pos,
				   &found);

	*pos = found;
	return set;
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	return s_start(m, pos);
}

static void s_stop(struct seq_file *m, void *p)
{
}

static int s_show(struct seq_file *m, void *p)
{
	struct kddm_set *set = p;

	seq_printf (m, "%20ld %16d %16d %10d %16d\n", set->id,
		    atomic_read(&set->nr_entries),
		    atomic_read(&set->nr_objects), set->obj_size,
		    atomic_read(&set->nr_objects) * set->obj_size);

        return 0;
}



const struct seq_operations kddminfo_op = {
        .start = s_start,
        .next = s_next,
        .stop = s_stop,
        .show = s_show,
};



static int kddminfo_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &kddminfo_op);
}



static struct file_operations proc_kddminfo_operations = {
        .open           = kddminfo_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = seq_release,
};



/****************************************************************************/
/*                                                                          */
/*                     /proc/kerrighed/kddm  Management                     */
/*                                                                          */
/****************************************************************************/



/** Read function for /proc/kerrighed/kddm/meminfo entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int read_meminfo (char *buffer,
                  char **start, off_t offset, int count, int *eof, void *data)
{
  static char mybuffer[80 * (5 + NB_OBJ_STATE)];
  static int len;
  int i;



  if (offset == 0)
    {
      len = sprintf (mybuffer,
                     "Free Pages:         %lu\n"
                     "Kddm objects:       %d\n"
                     "Master objetcs:     %d\n"
                     "Copy objects:       %d\n",
                     nr_free_pages (),
                     atomic_read(&nr_master_objects) +
		     atomic_read(&nr_copy_objects),
                     atomic_read(&nr_master_objects),
		     atomic_read(&nr_copy_objects));

      for (i = 0; i < NB_OBJ_STATE; i++)
        {
          len += sprintf (mybuffer + len,
                          "%s: \t %d\n", state_name[i],
			  atomic_read(&nr_OBJ_STATE[i]));
        }
      #ifdef CONFIG_KRG_KDDM_DEBUG
      check_pages();
      #endif
    }

  if (offset + count >= len)
    {
      count = len - offset;
      if (count < 0)
        count = 0;
      *eof = 1;
    }

  memcpy (buffer, &mybuffer[offset], count);

  return count;
}



/** Read function for /proc/kerrighed/kddm/setstat entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int read_setstat (char *buffer,
                   char **start, off_t offset, int count, int *eof, void *data)
{
  static char mybuffer[80 * 5];
  static int len;

  if (offset == 0)
    {
      len = 0;

      len += sprintf (mybuffer + len, "Get Object:          %ld\n",
                      total_get_object_counter);

      len += sprintf (mybuffer + len, "Grab Object:         %ld\n",
                      total_grab_object_counter);

      len += sprintf (mybuffer + len, "Remove Object:       %ld\n",
                      total_remove_object_counter);

      len += sprintf (mybuffer + len, "Flush Object:        %ld\n",
                      total_flush_object_counter);
    }

  if (offset + count >= len)
    {
      count = len - offset;
      if (count < 0)
        count = 0;
      *eof = 1;
    }

  memcpy (buffer, &mybuffer[offset], count);

  return count;
}



/** Read function for /proc/kerrighed/kddm/bench entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int read_bench (char *buffer,
		char **start, off_t offset, int count, int *eof, void *data)
{
	static char *mybuffer = NULL;
	static int len, size = 80*50;

	if (mybuffer == NULL)
		mybuffer = kmalloc (size, GFP_KERNEL);

	if (offset == 0)
		len = kddm_bench(mybuffer, size);

	if (offset + count >= len)
	{
		count = 1 + len - offset;
		if (count < 0)
			count = 0;
		*eof = 1;
	}

	snprintf (buffer, count, "%s", &mybuffer[offset]);

	*start = buffer;

	return count;
}




/*
 * Compatibility adapter for the pre-3.10 read_proc API.
 *
 * Keep the original KDDM proc read callbacks intact and expose them through
 * current struct file_operations.
 */
typedef int (*kddm_read_proc_t)(char *, char **, off_t, int, int *, void *);

static ssize_t kddm_legacy_proc_read(struct file *file,
                                     char __user *buf,
                                     size_t count,
                                     loff_t *ppos,
                                     kddm_read_proc_t read_proc)
{
        char *page;
        char *start;
        void *data;
        int eof = 0;
        int len;

        if (*ppos < 0)
                return -EINVAL;

        if (!count)
                return 0;

        count = min_t(size_t, count, PAGE_SIZE);

        page = kmalloc(count, GFP_KERNEL);
        if (!page)
                return -ENOMEM;

        start = page;
        data = PDE_DATA(file_inode(file));

        len = read_proc(page, &start, *ppos, (int)count, &eof, data);
        if (len <= 0) {
                kfree(page);
                return len;
        }

        if (!start)
                start = page;

        if (len > count)
                len = count;

        if (copy_to_user(buf, start, len)) {
                kfree(page);
                return -EFAULT;
        }

        *ppos += len;
        kfree(page);

        return len;
}

static ssize_t kddm_meminfo_read(struct file *file,
                                 char __user *buf,
                                 size_t count,
                                 loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos, read_meminfo);
}

static ssize_t kddm_setstat_read(struct file *file,
                                 char __user *buf,
                                 size_t count,
                                 loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos, read_setstat);
}

static ssize_t kddm_bench_read(struct file *file,
                               char __user *buf,
                               size_t count,
                               loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos, read_bench);
}

static const struct file_operations proc_kddm_meminfo_operations = {
        .read   = kddm_meminfo_read,
        .llseek = default_llseek,
};

static const struct file_operations proc_kddm_setstat_operations = {
        .read   = kddm_setstat_read,
        .llseek = default_llseek,
};

static const struct file_operations proc_kddm_bench_operations = {
        .read   = kddm_bench_read,
        .llseek = default_llseek,
};


/** Create the /proc/kerrighed/kddm directory and sub-directories.
 *  @author Renaud Lottiaux
 */
void create_kddm_proc_dir (void)
{
        BUG_ON(proc_kerrighed == NULL);

        /* Create /proc/kerrighed/kddm. */
        procfs_kddm = proc_mkdir_mode("kddm",
                                      S_IRUGO | S_IWUGO | S_IXUGO,
                                      proc_kerrighed);
        if (procfs_kddm == NULL) {
                printk("Cannot create /proc/kerrighed/kddm\n");
                return;
        }

        /* Create /proc/kerrighed/kddm/meminfo. */
        procfs_meminfo = proc_create("meminfo", S_IRUGO, procfs_kddm,
                                     &proc_kddm_meminfo_operations);
        if (procfs_meminfo == NULL) {
                printk("Cannot create /proc/kerrighed/kddm/meminfo\n");
                return;
        }

        /* Create /proc/kerrighed/kddm/setstat. */
        procfs_setstat = proc_create("setstat", S_IRUGO, procfs_kddm,
                                     &proc_kddm_setstat_operations);
        if (procfs_setstat == NULL) {
                printk("Cannot create /proc/kerrighed/kddm/setstat\n");
                return;
        }

        /* Create /proc/kerrighed/kddm/bench. */
        procfs_bench = proc_create("bench", S_IRUGO, procfs_kddm,
                                   &proc_kddm_bench_operations);
        if (procfs_bench == NULL) {
                printk("Cannot create /proc/kerrighed/kddm/bench\n");
                return;
        }

        /* Create /proc/kddminfo. */
        proc_create("kddminfo", S_IRUGO, NULL,
                    &proc_kddminfo_operations);
}



/** Delete the /proc/kerrighed/kddm directory and sub-directories.
 *  @author Renaud Lottiaux
 */
void remove_kddm_proc_dir (void)
{
  procfs_deltree (procfs_kddm);
}



/****************************************************************************/
/*                                                                          */
/*               /proc/kerrighed/kddm/<set_id>  Management                */
/*                                                                          */
/****************************************************************************/




/** Read function for /proc/kerrighed/kddm/<set_id>/setstat entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int read_set_id_setstat (char *buffer,
                          char **start,
                          off_t offset, int count, int *eof, void *data)
{
	struct kddm_set *set = NULL;
	static char mybuffer[80 * 5];
	static int len;

	if (offset == 0) {
		set = _find_get_kddm_set (kddm_def_ns, (kddm_set_id_t) data);
		BUG_ON (!set);

		len = 0;

		len += sprintf (mybuffer + len, "Get Object:          %ld\n",
				set->get_object_counter);

		len += sprintf (mybuffer + len, "Grab Object:         %ld\n",
				set->grab_object_counter);

		len += sprintf (mybuffer + len, "Remove Object:       %ld\n",
				set->remove_object_counter);

		len += sprintf (mybuffer + len, "Flush Object:        %ld\n",
				set->flush_object_counter);

		put_kddm_set(set);
	}

  if (offset + count >= len)
    {
      count = len - offset;
      if (count < 0)
        count = 0;
      *eof = 1;
    }

  memcpy (buffer, &mybuffer[offset], count);

  return count;
}



/** Read function for /proc/kerrighed/kddm/<set_id>/setinfo entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int read_set_id_setinfo (char *buffer,
                          char **start,
                          off_t offset, int count, int *eof, void *data)
{
	struct kddm_set *set = NULL;
	static char mybuffer[80 * 20];

	static int len;

	if (offset == 0) {
		len = 0;

		set = _find_get_kddm_set (kddm_def_ns, (kddm_set_id_t) data);
		BUG_ON (!set);

		if (set->iolinker == NULL)
			len += sprintf (mybuffer + len,
					"Type:         No IO Linker\n");
		else
			len += sprintf (mybuffer + len, "Type:         %s\n",
					set->iolinker->linker_name);

		len += sprintf (mybuffer + len, "Manager            : %d\n",
				KDDM_SET_MGR (set));

		len += sprintf (mybuffer + len, "Nr Objects         : %d\n",
				atomic_read(&set->nr_objects));

		len += sprintf (mybuffer + len, "Nr Entries         : %d\n",
				atomic_read(&set->nr_entries));

		len += sprintf (mybuffer + len, "Flags              : "
				"0x%08lx\n", set->flags);

		len += sprintf (mybuffer + len, "State              : %d\n",
				set->state);

		len += sprintf (mybuffer + len, "- Master entries   : %d\n",
				atomic_read(&set->nr_masters));

		len += sprintf (mybuffer + len, "- Copy entries     : %d\n",
				atomic_read(&set->nr_copies));

		len += sprintf (mybuffer + len, "Usage count  : %d\n",
				atomic_read(&set->count) - 1);

		switch (set->def_owner) {
		  case KDDM_RR_DEF_OWNER:
			  len += sprintf (mybuffer + len,
					  "Default owner    : Round Robin\n");
			  break;

		  case KDDM_CUSTOM_DEF_OWNER:
			  len += sprintf (mybuffer + len,
					  "Default owner    : Custom\n");
			  break;

		  default:
			  len += sprintf (mybuffer + len, "Default owner    : %d\n",
					  set->def_owner);
		}

		put_kddm_set(set);
	}

	if (offset + count >= len) {
		count = len - offset;
		if (count < 0)
			count = 0;
		*eof = 1;
	}

	memcpy (buffer, &mybuffer[offset], count);

	return count;
}



/** Read function for /proc/kerrighed/kddm/<set_id>/objectstates entry.
 *  @author Gael Utard
 */
int read_set_id_objectstates (char *buffer,
                              char **start,
                              off_t offset, int count, int *eof, void *data)
{
	int i, size = 0;
	struct kddm_set *set;

	if (offset >= size) {
		*eof = 1;
		return 0;
	}

	for (i = 0; offset + i < size && i < count; i++) {
		struct kddm_obj *obj_entry;

		obj_entry = _get_kddm_obj_entry(kddm_def_ns,
						(kddm_set_id_t) data,
						offset + i, &set);

		if (obj_entry != NULL) {
			switch (OBJ_STATE(obj_entry)) {
			case READ_COPY:
				buffer[i] = 'R';
				break;

			case WAIT_CHG_OWN_ACK:
			case WAIT_ACK_WRITE:
			case WAIT_ACK_INV:
			case READ_OWNER:
				buffer[i] = 'O';
				break;

			case WRITE_GHOST:
			case WRITE_OWNER:
				buffer[i] = 'W';
				break;

			case INV_COPY:
			case INV_OWNER:
			case INV_FILLING:
			case WAIT_OBJ_READ:
			case WAIT_OBJ_WRITE:
				buffer[i] = 'I';
				break;

			default:
				buffer[i] = '?';
			}
			put_kddm_obj_entry(set, obj_entry, offset + i);
		}
		else
			buffer[i] = 'I';
	}

	return i;
}




static ssize_t kddm_objectstates_read(struct file *file,
                                      char __user *buf,
                                      size_t count,
                                      loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos,
                                     read_set_id_objectstates);
}

static ssize_t kddm_set_id_setstat_read(struct file *file,
                                        char __user *buf,
                                        size_t count,
                                        loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos,
                                     read_set_id_setstat);
}

static ssize_t kddm_setinfo_read(struct file *file,
                                 char __user *buf,
                                 size_t count,
                                 loff_t *ppos)
{
        return kddm_legacy_proc_read(file, buf, count, ppos,
                                     read_set_id_setinfo);
}

static const struct file_operations proc_kddm_objectstates_operations = {
        .read   = kddm_objectstates_read,
        .llseek = default_llseek,
};

static const struct file_operations proc_kddm_set_id_setstat_operations = {
        .read   = kddm_set_id_setstat_read,
        .llseek = default_llseek,
};

static const struct file_operations proc_kddm_setinfo_operations = {
        .read   = kddm_setinfo_read,
        .llseek = default_llseek,
};


/* Create a /proc/kerrighed/kddm/<set_id> directory and sub-directories. */

struct proc_dir_entry *create_kddm_proc (kddm_set_id_t set_id)
{
        struct proc_dir_entry *entry;
        struct proc_dir_entry *objectstates;
        struct proc_dir_entry *stat;
        struct proc_dir_entry *info;
        char buffer[24];
        void *data = (void *)(unsigned long)set_id;

        BUG_ON(procfs_kddm == NULL);

        /* Create /proc/kerrighed/kddm/<set_id>. */
        snprintf(buffer, sizeof(buffer), "%ld", set_id);

        entry = proc_mkdir_mode(buffer,
                                S_IRUGO | S_IWUGO | S_IXUGO,
                                procfs_kddm);
        if (entry == NULL)
                return NULL;

        /* Create <set_id>/objectstates. */
        objectstates =
                proc_create_data("objectstates", S_IRUGO, entry,
                                 &proc_kddm_objectstates_operations, data);
        if (objectstates == NULL)
                return NULL;

        /* Create <set_id>/setstat. */
        stat = proc_create_data("setstat", S_IRUGO, entry,
                                &proc_kddm_set_id_setstat_operations, data);
        if (stat == NULL) {
                printk("Cannot create proc entry for %ld/setstat\n",
                       set_id);
                return NULL;
        }

        /* Create <set_id>/setinfo. */
        info = proc_create_data("setinfo", S_IRUGO, entry,
                                &proc_kddm_setinfo_operations, data);
        if (info == NULL)
                return NULL;

        return entry;
}



/* Remove a /proc/kerrighed/kddm/<set_id> directory and sub-directories. */


void remove_kddm_proc (struct proc_dir_entry *proc_entry)
{
  if (proc_entry != NULL)
    procfs_deltree (proc_entry);
}



/***********************************************************************/
/*                                                                     */
/*         Define Kddm services in the /proc/kerrighed/services        */
/*                                                                     */
/***********************************************************************/



/** Init Kddm proc stuffs.
 *  @author Renaud Lottiaux
 */
int procfs_kddm_init (void)
{
	create_kddm_proc_dir ();

	return 0;
};



/** Finalize Kddm proc stuffs.
 *  @author Renaud Lottiaux
 */
int procfs_kddm_finalize (void)
{
	remove_kddm_proc_dir ();

	return 0;
};
