/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>


/*
 * Put your function declarations and data types here ...
 */

struct file {
    struct vnode *vn; // Vnode pointer
    off_t offset; // offset for the file
    int flags; // flags for the file (read or write)
    int refcount; // reference count for the file
};

// initialized at main.c, so that all processes share same open file table
struct file open_file_table[OPEN_MAX];
int open_file_table_init(void);

// initialized at runprogram.c, so that all threads share same file descriptor table
// if we need to implement advanced parts
int fd_table_init(void);

int sys_open(userptr_t filename, int flags, mode_t mode, int *retval);
int sys_close(int fd);
ssize_t sys_read(int fd, void *buf, size_t buflen, int *retval);
ssize_t sys_write(int fd, const void *buf, size_t nbytes, int *retval);

int sys_lseek (int fd,off_t pos, int whence,off_t *retval);
int sys_dup2(int oldfd,int newfd,int *retval);

#endif /* _FILE_H_ */
