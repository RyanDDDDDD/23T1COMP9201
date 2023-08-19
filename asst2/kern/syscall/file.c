#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

/*
 * Add your file-related functions here ...
 */
int open_file_table_init(void) {
    if (open_file_table == NULL) {
        return ENOMEM;
    }

    for (int i = 0; i < OPEN_MAX; i++) {
        open_file_table[i].flags = -1;
        open_file_table[i].offset = -1;
        open_file_table[i].refcount = 0;
        open_file_table[i].vn = NULL;
    }

    return 0;
}

// assign fd 1/2 to stdout/stderr for each process
int fd_table_init(void){
    char stdout[] = "con:";
	char stderr[] = "con:";

    int *fd_table = curproc->fd_table;

    // assign fd of index 1 to stdout
    struct vnode *vn_1;
    int err;

    err = vfs_open(stdout, O_WRONLY, 0, &vn_1);
    if (err){
        return err;
    }

    // find an valid open table index 
    int op_index = -1;
    for (int i = 0; i < OPEN_MAX; i++){
        if(open_file_table[i].vn == NULL){
            op_index = i;
        }
    }
    // open file table is full
    if (op_index == -1){
        return ENFILE;
    }

    open_file_table[op_index].flags = O_WRONLY;
    open_file_table[op_index].offset = 0;
    open_file_table[op_index].refcount++;
    open_file_table[op_index].vn = vn_1;
    
    // connect fd 1 to stdout
    fd_table[1] = op_index;
 
    struct vnode *vn_2;

    err = vfs_open(stderr, O_WRONLY, 0, &vn_2);
    if (err){
        return err;
    }

     // find an valid open table index 
    op_index = -1;
    for (int i = 0; i < OPEN_MAX; i++){
        if(open_file_table[i].vn == NULL){
            op_index = i;
        }
    }
    // open file table is full
    if (op_index == -1){
        return ENFILE;
    }

    open_file_table[op_index].flags = O_WRONLY;
    open_file_table[op_index].offset = 0;
    open_file_table[op_index].refcount++;
    open_file_table[op_index].vn = vn_2;

    // connect fd 2 to stderr
    fd_table[2] = op_index;
    return 0;
};

int sys_open(userptr_t filename, int flags, mode_t mode, int *retval) {
    int error;
    char src[PATH_MAX];
    size_t actual;

    // check if flag is valid
    int all_flags = O_RDONLY | O_WRONLY | O_RDWR| O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NOCTTY;
    if ((flags & all_flags) != flags) {
		return EINVAL;
	}

    error = copyinstr(filename, src, PATH_MAX, &actual);

    if (error) {
        return error;
    }

    struct vnode *vn;
    error = vfs_open(src, flags, mode, &vn);

    if (error) {
        return error;
    }

    // Find the first available file descriptor
    int fd = 0;
    while (fd < OPEN_MAX && curproc->fd_table[fd] != -1) {
        fd++;
    }
    if (fd == OPEN_MAX) {
        return EMFILE;
    }

    // Find the first available file pointer
    int file_index = 0;
    while (file_index < OPEN_MAX && open_file_table[file_index].flags != -1) {
        file_index++;
    }
    if (file_index == OPEN_MAX) {
        return ENFILE;
    }

    // Update the file table
    open_file_table[file_index].vn = vn;
    open_file_table[file_index].offset = 0;
    open_file_table[file_index].flags = flags;
    open_file_table[file_index].refcount = 1;

    // Update the file descriptor table
    curproc->fd_table[fd] = file_index;
    *retval = fd;

    return 0;
}

int sys_lseek (int fd,off_t pos, int whence,off_t *retval){
    // get file descriptor table
    int *fd_table = curproc->fd_table;
    
    int op_index = fd_table[fd];

    // check if fd is valid (out of range)
    if (fd < 0 || fd >= OPEN_MAX){
        return EBADF;
    }

    // check if fd is valid (not used)
    if (fd_table[fd] == -1){
        return EBADF;
    }

    // check whether the file is seekable
    if (!VOP_ISSEEKABLE(open_file_table[op_index].vn)){
        return ESPIPE;
    }

    off_t new_pos = -1;
    
    // process new posistion 
    switch(whence){
        case SEEK_SET: {
            new_pos = pos;
            break;
        }
        case SEEK_CUR: {
            new_pos = open_file_table[op_index].offset + pos;
            break;
        }
        case SEEK_END: {
            struct stat st;
            int err = VOP_STAT(open_file_table[op_index].vn, &st);

            if (err){
                return err;
            }
            new_pos = st.st_size + pos;
            break;
        }
        default:
            // lseek fails, whence is invalid
            return EINVAL;
    }

    // the resulting seeking position is negative, which is not seekable
    if (new_pos < 0){
        return EINVAL;
    }

    // successully setup new offset
    open_file_table[op_index].offset = new_pos;

    *retval = new_pos;
    return 0;

};

int sys_dup2(int oldfd,int newfd,int *retval){
    // get file descriptor table 
    int *fd_table = curproc->fd_table;

    // check valid fd
    // one of the fd is out of range, which is not valid
    if (oldfd < 0 || newfd < 0 || oldfd >= OPEN_MAX || newfd >= OPEN_MAX) {
        return EBADF;
    }

    // if oldfd is not used
    if (fd_table[oldfd] == -1){
        return EBADF;
    }

    // no effect on oldfd == newfd, we simply return newfd
    if (oldfd == newfd){
        *retval = newfd;
        return 0;
    }

    // check whether file descriptor table is full
    int fd_index = -1;
    for (int i = 0; i < OPEN_MAX; i++){
        if(fd_table[i] != -1){
            fd_index = i;
        }
    }
    // file descriptor table is full
    if (fd_index == -1){
        return EMFILE;
    }

    // check whether open file table is full 
    int op_index = -1;
    for (int i = 0; i < OPEN_MAX; i++){
        if(open_file_table[i].vn != NULL){
            op_index = i;
        }
    }
    // open file table is full
    if (op_index == -1){
        return ENFILE;
    }

    // check whether newfd points to an opened file, if so, close the file
    op_index = fd_table[newfd];
    if (open_file_table[op_index].vn != NULL){
        sys_close(newfd);
    }

    // we can now safely redirect newfd to point to oldfd

    // update reference counter on file pointer at open file table
    op_index = fd_table[oldfd];
    open_file_table[op_index].refcount++;
    
    // newfd now points to the same file pointer on open file table
    fd_table[newfd] = fd_table[oldfd];
    *retval = newfd;
    return 0;
};

int sys_close(int fd) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    if (curproc->fd_table[fd] == -1) {
        return EBADF;
    }

    int file_index = curproc->fd_table[fd];
    open_file_table[file_index].refcount--;
    curproc->fd_table[fd] = -1;

    if (open_file_table[file_index].refcount == 0) {
        vfs_close(open_file_table[file_index].vn);
        open_file_table[file_index].flags = -1;
        open_file_table[file_index].offset = -1;
        open_file_table[file_index].refcount = 0;
        open_file_table[file_index].vn = NULL;
    }
    
    return 0;
}

ssize_t sys_read(int fd, void *buf, size_t buflen, int *retval) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    if (curproc->fd_table[fd] == -1) {
        return EBADF;
    }

    int file_index = curproc->fd_table[fd];

    if (open_file_table[file_index].flags & O_WRONLY) {
        return EBADF;
    }

    struct uio u;
    struct iovec iov;

    uio_uinit(&iov, &u, buf, buflen, open_file_table[file_index].offset, UIO_READ);

    int error = VOP_READ(open_file_table[file_index].vn, &u);

    if (error) {
        return error;
    }

    *retval = buflen - u.uio_resid;
    open_file_table[file_index].offset = u.uio_offset;

    return 0;
}

ssize_t sys_write(int fd, const void *buf, size_t nbytes, int *retval) {
    if (fd < 0 || fd >= OPEN_MAX) {
        return EBADF;
    }

    if (curproc->fd_table[fd] == -1) {
        return EBADF;
    }

    int file_index = curproc->fd_table[fd];

    if (open_file_table[file_index].flags & O_RDONLY) {
        return EBADF;
    }

    // check if entry on file table is used
    if (open_file_table[file_index].vn == NULL){
        return EBADF;
    }


    struct uio u;
    struct iovec iov;

    uio_uinit(&iov, &u, (userptr_t)buf, nbytes, open_file_table[file_index].offset, UIO_WRITE);
    
    int error = VOP_WRITE(open_file_table[file_index].vn, &u);

    if (error) {
        return error;
    }

    *retval = nbytes - u.uio_resid;
    open_file_table[file_index].offset = u.uio_offset;

    return 0;
}