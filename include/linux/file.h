/*
 * Wrapper functions for accessing the file_struct fd array.
 */

#ifndef __LINUX_FILE_H
#define __LINUX_FILE_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/posix_types.h>

struct file;
#ifdef CONFIG_KRG_FAF
struct files_struct;
struct task_struct;
#endif

extern void fput(struct file *);

struct file_operations;
struct vfsmount;
struct dentry;
struct path;
#ifdef CONFIG_KRG_FAF
extern struct file *get_empty_filp(void);
#endif
extern struct file *alloc_file(struct path *, fmode_t mode,
	const struct file_operations *fop);

static inline void fput_light(struct file *file, int fput_needed)
{
	if (fput_needed)
		fput(file);
}

struct fd {
	struct file *file;
	unsigned int flags;
};
#define FDPUT_FPUT       1
#define FDPUT_POS_UNLOCK 2

static inline void fdput(struct fd fd)
{
	if (fd.flags & FDPUT_FPUT)
		fput(fd.file);
}

extern struct file *fget(unsigned int fd);
extern struct file *fget_light(unsigned int fd, int *fput_needed);

static inline struct fd fdget(unsigned int fd)
{
	int b;
	struct file *f = fget_light(fd, &b);
	return (struct fd){f,b};
}

extern struct file *fget_raw(unsigned int fd);
extern struct file *fget_raw_light(unsigned int fd, int *fput_needed);

static inline struct fd fdget_raw(unsigned int fd)
{
	int b;
	struct file *f = fget_raw_light(fd, &b);
	return (struct fd){f,b};
}

extern int f_dupfd(unsigned int from, struct file *file, unsigned flags);
extern int replace_fd(unsigned fd, struct file *file, unsigned flags);
extern void set_close_on_exec(unsigned int fd, int flag);
extern bool get_close_on_exec(unsigned int fd);
extern void put_filp(struct file *);
extern int get_unused_fd_flags(unsigned flags);
#define get_unused_fd() get_unused_fd_flags(0)
#ifdef CONFIG_KRG_FAF
int __get_unused_fd(struct task_struct *task);
extern void __put_unused_fd(struct files_struct *files, unsigned int fd);
#endif
extern void put_unused_fd(unsigned int fd);
#ifdef CONFIG_KRG_FAF
extern void __fd_install(struct files_struct *files,
			 unsigned int fd, struct file *file);
#endif
extern void fd_install(unsigned int fd, struct file *file);

extern void flush_delayed_fput(void);
extern void __fput_sync(struct file *);

#ifdef CONFIG_KRG_DVFS
struct fdtable;
int count_open_files(struct fdtable *fdt);

struct fdtable *alloc_fdtable(unsigned int nr);
void free_fdtable(struct fdtable *fdt);
#endif

#endif /* __LINUX_FILE_H */
