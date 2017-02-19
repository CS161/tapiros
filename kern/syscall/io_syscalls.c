/*
 * IO-related system calls.
 *
 * Includes open(), read(), write(), close(), lseek(),
 * dup2(), chdir(), and __getcwd().
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h> 
#include <uio.h>
#include <vnode.h>
#include <addrspace.h>
#include <syscall.h>
#include <kern/fcntl.h>
#include <proc.h>
#include <current.h>
#include <vfs.h>
#include <kern/seek.h>
#include <stat.h>
#include <limits.h>
#include <copyinout.h>


int sys_open(const userptr_t pathname, int flags, int* retval) {
	if(pathname == NULL){ //Trivial Check
		return EFAULT;
	}

	int modeflags = (O_RDONLY * ((bool)(flags && O_RDONLY))) || (O_WRONLY * ((bool)(flags && O_WRONLY))) || ((O_RDWR * ((bool)(flags && O_RDWR))));
	int targetFD = -1; //Which entry in per process file table.
	int perms; //deriived from flags
	int result; //return values
	struct vnode* vn; //Opened vn
	unsigned size = vfilearray_num(k_file_table);
	struct vfile * thisFile; //Used when iterating through entries.
	char* path_copy = kstrdup((char*)pathname);


	if(flags && O_RDONLY){ //set the perms
		perms = 664;
	}else if(flags && O_WRONLY){
		perms = 662;
	}else if (flags && O_RDWR){
		perms = 666;
	}else{
		return EINVAL;
	}

	for(int i = 0; i < MAX_FDS; i++) //Find the appropriate per process entry
	{
		if(curproc->p_fds[i] == -1){
			targetFD = i;
			break;
		}
	}
	if(targetFD == -1){
		return EMFILE;
	}

	result = vfs_open(path_copy, flags, perms, &vn); //Actually open the file
	if(result){
		return result;
	}
	path_copy = kstrdup((char*)pathname);

	spinlock_acquire(&k_file_lock);

	if(! VOP_ISSEEKABLE(vn)){ //Handle stuff if the file is unseekable
		for(unsigned i = 0; i < size; i++){
			thisFile = vfilearray_get(k_file_table, i);
			if(thisFile != NULL && thisFile -> vf_offset == -1 && strcmp(path_copy, thisFile -> vf_name) == 0 && thisFile->open_mode == (mode_t) modeflags){
				lock_acquire(&(thisFile->vf_rlock));
				thisFile->refcount++;
				lock_release(&(thisFile->vf_rlock));
				curproc->p_fds[targetFD] = i;
				spinlock_release(&k_file_lock);
				vfs_close(vn);
				//return targetFD; 	
				*retval = targetFD;
				return 0;		
			}
		}
	}

	struct vfile * new_vfile = vfile_init(path_copy, vn, modeflags); //We need a new vfile entry!
	if(new_vfile == NULL){
		spinlock_release(&k_file_lock);
		vfs_close(vn);
		return ENFILE;
	}
	new_vfile->refcount = 1;


	for(unsigned i = 0; i < size; i++){ //Find if there is an existing entry in the table that has been nullified.
		thisFile = vfilearray_get(k_file_table, i);
		if(thisFile == NULL){
			vfilearray_set(k_file_table, i, new_vfile);
			curproc->p_fds[targetFD] = i;
			spinlock_release(&k_file_lock);
			//return targetFD; 	
			*retval = targetFD;
			return 0;
		}
	}
	
	int err = vfilearray_add(k_file_table, new_vfile, (unsigned int*)&(result));
	if(err){
		kfree(new_vfile);
		spinlock_release(&k_file_lock);
		vfs_close(vn);
		return ENFILE;
	}
	spinlock_release(&k_file_lock);
	curproc->p_fds[targetFD] = result;
	//return targetFD;
	*retval = targetFD;
	return 0;
}

ssize_t sys_read(int fd, userptr_t buf, size_t buflen, int *retval) {
	if(fd < 0|| curproc->p_fds[fd] == -1 || vfilearray_get(k_file_table, curproc->p_fds[fd]) == NULL){
		return EBADF;
	}
	struct vfile* file = vfilearray_get(k_file_table, curproc->p_fds[fd]);
	if(!(file->open_mode & O_RDONLY) || !(file->open_mode & O_RDWR)){
		return EFAULT;
	}
	struct uio newUIO;
	int result;
	if(file->vf_offset != -1){
		lock_acquire(&(file->vf_flock));
		uio_uinit(&newUIO, buf, buflen, file->vf_offset, UIO_READ);
		result = VOP_READ(file->vf_vnode, &newUIO);
		if(result){
			lock_release(&(file->vf_flock));
			return result;
		}
		size_t bytes_read = buflen - newUIO.uio_resid;
		file->vf_offset += bytes_read; 
		lock_release(&(file->vf_flock));
		//return bytes_read;
		*retval = bytes_read;
		return 0;
	}else{
		uio_uinit(&newUIO, buf, buflen, 0, UIO_READ);
		result = VOP_READ(file->vf_vnode, &newUIO);
		if(result){
			return result;
		}

		*retval =  buflen - newUIO.uio_resid;
		return 0;
	}
}


ssize_t sys_write(int fd, const userptr_t buf, size_t buflen, int* retval) {
	if(fd < 0 || curproc->p_fds[fd] == -1 || vfilearray_get(k_file_table, curproc->p_fds[fd]) == NULL){
		return EBADF;
	}
	struct vfile* file = vfilearray_get(k_file_table, curproc->p_fds[fd]);
	if(!(file->open_mode & O_WRONLY) || !(file->open_mode & O_RDWR)){
		return EFAULT;
	}
	struct uio newUIO; 
	int result;
	if(file->vf_offset != -1){
		lock_acquire(&(file->vf_flock));
		uio_uinit(&newUIO, buf, buflen, file->vf_offset, UIO_WRITE);
		result = VOP_WRITE(file->vf_vnode, &newUIO);
		if(result){
			lock_release(&(file->vf_flock));
			return result;
		}
		size_t bytes_written = buflen - newUIO.uio_resid;
		file->vf_offset += bytes_written; 
		lock_release(&(file->vf_flock));
		//return bytes_written;
		*retval = bytes_written;
		return 0;
	}else{
		uio_uinit(&newUIO, buf, buflen, 0, UIO_WRITE);
		result = VOP_WRITE(file->vf_vnode, &newUIO);
		if(result){
			return result;
		}
		//return buflen - newUIO.uio_resid;
		*retval = buflen - newUIO.uio_resid;
		return 0;
	}
}


off_t sys_lseek(int fd, off_t pos, int whence, int* retval) {
	if(fd < 0 || curproc->p_fds[fd] == -1 || vfilearray_get(k_file_table, curproc->p_fds[fd]) == NULL){
		return EBADF;
	}
	struct vfile* file = vfilearray_get(k_file_table, curproc->p_fds[fd]);
	if(file->vf_offset == -1){
		return ESPIPE;
	} 
	off_t output;
	lock_acquire(&(file->vf_flock));
	if(whence == SEEK_SET){
		if(pos < 0){
			lock_release(&(file->vf_flock));
			return EINVAL;
		}
		file->vf_offset = pos;
	}else if (whence == SEEK_CUR){
		if(pos + file->vf_offset < 0){
			lock_release(&(file->vf_flock));
			return EINVAL;
		}
		file->vf_offset += pos;
	}else if (whence == SEEK_END){
		struct stat* statbuf = (struct stat*) kmalloc(sizeof(struct stat));
		if(statbuf == NULL){
			lock_release(&(file->vf_flock));
			return ENOMEM;
		}
		VOP_STAT(file->vf_vnode, statbuf);
		int propOffset = statbuf->st_size + pos;
		kfree(statbuf);
		if(propOffset < 0){
			lock_release(&(file->vf_flock));
			return EINVAL;
		}
	}else{
		return EINVAL;
	}
	output = file->vf_offset;
	lock_release(&(file->vf_flock));
	//return output;
	*retval = output;
	return 0;
}

int sys_close(int fd) {
	if(curproc->p_fds[fd] == -1 || vfilearray_get(k_file_table, curproc->p_fds[fd]) == NULL){
		return EBADF;
	}
	struct vfile* file = vfilearray_get(k_file_table, curproc->p_fds[fd]);
	lock_acquire(&(file->vf_rlock));
KASSERT(file->refcount > 0);
	if(file->refcount == 1){ 
		spinlock_acquire(&k_file_lock);//Refcount should still be 1 at this point because refcount is protected.
		vfs_close(file->vf_vnode);
		kfree(file->vf_name);
		kfree(file->vf_vnode);
		lock_destroy(&(file->vf_flock));
		lock_destroy(&(file->vf_rlock)); 
		kfree(file);
		vfilearray_set(k_file_table, curproc->p_fds[fd], NULL);
		spinlock_release(&k_file_lock); //Check this for possible deadlock condition.
	}else{
		file->refcount--; 
		lock_release(&(file->vf_rlock));
	}
	curproc->p_fds[fd] = -1;
	return 0;
}

int sys_dup2(int oldfd, int newfd) {
	if(newfd < 0 || oldfd < 0 || curproc->p_fds[oldfd] == -1 || vfilearray_get(k_file_table, curproc->p_fds[oldfd]) == NULL){
		return EBADF;
	}
	if(newfd >= MAX_FDS){
		return EMFILE;
	}
	sys_close(newfd); //Has potential to mess up return values.
	KASSERT(curproc->p_fds[oldfd] == -1);
	curproc->p_fds[newfd] = curproc->p_fds[oldfd];  //No need to update refcount because the same process still has file open.
	return 0;

}

int sys_chdir(const userptr_t pathname) {
	int err = 0;
	size_t len = 0;
	char *kbuf = kmalloc( sizeof(char) * PATH_MAX);
	if(kbuf == NULL){
		return ENOMEM;
	}
	err = copyinstr(pathname, kbuf, PATH_MAX, &len);
	if(err == 0){
		err = vfs_chdir(kbuf);
	}
	kfree(kbuf);
	return err;
}

int sys___getcwd(userptr_t buf, size_t buflen, int *retval) {
	struct uio *uio = (struct uio*)kmalloc(sizeof(struct uio));
	if(uio == NULL){
		return ENOMEM;
	}
	uio_uinit(uio, buf, buflen, 0, UIO_READ);
	int err = vfs_getcwd(uio);
	if(err != 0){
		return err;
	}
	if(retval != NULL){
		*retval = uio->uio_offset;
	}
	return 0;
}