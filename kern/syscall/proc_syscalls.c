/*
 * Process-related system calls.
 *
 * Includes getpid(), fork(), execv(), waitpid(), and _exit().
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <syscall.h>
#include <copyinout.h>
#include <kern/wait.h>
#include <thread.h>
#include <wchan.h>
#include <machine/trapframe.h>
#include <vfs.h>
#include <limits.h>
#include <kern/fcntl.h>


int sys_getpid(int *retval) {
	KASSERT(retval != NULL);
	*retval = curproc->pid;
	return 0;
}

int sys_fork(struct trapframe *tf, int *retval) {
	int err = 0;

	lock_acquire(fork_exec_lock);

	struct trapframe *newtf = kmalloc(sizeof(struct trapframe));	// prevent race condition where tf
	memcpy(newtf, tf, sizeof(struct trapframe));					// (and the stack) go away before 
																	// enter_forked_process() is reached
	if(newtf == NULL) {
		err = ENOMEM;
		goto err1;
	}

	struct proc *newp = proc_create_runprogram(curproc->p_name);
	if(newp == NULL) {
		err = ENPROC;
		goto err2;
	}

	err = as_copy(curproc->p_addrspace, &newp->p_addrspace);
	if(err != 0) {
		goto err3;
	}

	unsigned index = 0;
	err = procarray_add(curproc->p_children, newp, &index);
	if(err != 0) {
		err = ENOMEM;
		goto err3;	// proc_destroy destroys address space if assigned
	}

	if(curproc != kproc)			// since we use a coffin for orphans, we
		newp->p_parent = curproc;	// don't want to mark kproc as a parent


	for(int i = 0; i < MAX_FDS; i++) {		// duplicate all open file descriptors
		if(curproc->p_fds[i] >= 0) {		
			struct vfile *vf = vfilearray_get(vfiles, curproc->p_fds[i]);

			spinlock_acquire(&vf->vf_lock);		// duplicate some functionality from sys_close
												// because here we use an arbitrary proc, not curproc
			KASSERT(vf->vf_refcount > 0);
			vf->vf_refcount++;

			spinlock_release(&vf->vf_lock);

			newp->p_fds[i] = curproc->p_fds[i];
		}
	}

	err = thread_fork(curthread->t_name, newp, enter_forked_process, (void *)newtf, 0);
	if(err != 0) {
		err = ENOMEM;
		goto err4;
	}

	if(retval != NULL)
		*retval = newp->pid;

	lock_release(fork_exec_lock);

	return 0;

	// error cleanup

	err4:
		procarray_remove(curproc->p_children, index);
	err3:
		proc_destroy(newp);
	err2:
		kfree(newtf);
	err1:
		lock_release(fork_exec_lock);
		return err;
}

int sys_execv(const userptr_t program, userptr_t argv) {
	if(program == NULL || argv == NULL)
		return EFAULT;

	int err = 0;

	lock_acquire(fork_exec_lock);
	
	char *kprogram = kmalloc(sizeof(char) * PATH_MAX);
	if(kprogram == NULL)
		goto err1;

	size_t klen = 0;
	err = copyinstr(program, kprogram, PATH_MAX, &klen);
	if(err != 0) {
		goto err2;
	}
	if(klen <= 1) {
		err = ENOENT;
		goto err2;
	}

	char **nargv = kmalloc(ARG_MAX * sizeof(char *));
	if(nargv == NULL) {
		err = ENOMEM;
		goto err2;
	}

	size_t *nargvlens = kmalloc(ARG_MAX * sizeof(size_t));
	if(nargvlens == NULL) {
		err = ENOMEM;
		goto err3;
	}

	char *kbuf = kmalloc(ARG_MAX * sizeof(char));
	if(kbuf == NULL) {
		err = ENOMEM;
		goto err4;
	}

	int i = 0;
	size_t argvlen = 0;
	while(argv != NULL)	{	// extract parameter strings and lengths
		userptr_t uptr = NULL;
		err = copyin(argv, &uptr, sizeof(userptr_t));
		if(err != 0)
			goto err5;

		copyinstr(uptr, kbuf, ARG_MAX, &klen);
		char *nargvi = kmalloc(klen * sizeof(char));
		if(nargvi == NULL) {
			err = ENOMEM;
			goto err5;
		}

		argvlen += klen;
		if(argvlen > ARG_MAX) {
			err = E2BIG;
			goto err5;
		}

		memcpy(nargvi, kbuf, klen * sizeof(char));
		nargv[i] = nargvi;
		nargvlens[i] = klen;

		i++;
		argv += sizeof(userptr_t);
	}

	int argc = i;

	struct addrspace *naddr = as_create();
	struct addrspace *oaddr = curproc->p_addrspace;
	if(naddr == NULL) {
		err = ENOMEM;
		goto err5;
	}

	proc_setas(naddr);
	as_activate();

	vaddr_t stackptr;
	err = as_define_stack(naddr, &stackptr);
	if(err != 0)
		goto err6;

	userptr_t *uptrs = kmalloc(argc * sizeof(userptr_t));
	if(uptrs == NULL) {
		err = ENOMEM;
		goto err6;
	}

	static char zeros[sizeof(userptr_t)];	// static, so initialized as 0
	while(i > 0) {							// fill new stack with parameter strings
		i--;
		int nzeros = nargvlens[i] % sizeof(userptr_t);	// pad the end with 0s to be 4-aligned (on 32-bit)
		if(nzeros > 0) {
			stackptr -= nzeros;
			err = copyout(zeros, (userptr_t) stackptr, nzeros);
			if(err != 0)
				goto err7;
		}
		stackptr -= nargvlens[i];
		err = copyout(nargv[i], (userptr_t) stackptr, nargvlens[i]);	// copy the actual string
		if(err != 0)
			goto err7;

		uptrs[i] = (userptr_t) stackptr;

		KASSERT(stackptr % sizeof(userptr_t) == 0);	// make sure alignment logic works
	}
	stackptr -= sizeof(userptr_t);									// null-terminate argv
	err = copyout(zeros, (userptr_t) stackptr, sizeof(userptr_t));
	if(err != 0)
		goto err7;

	i = argc;
	while(i > 0) {
		i--;

		stackptr -= sizeof(userptr_t);
		userptr_t uptr = uptrs[i];
		copyout(uptr, (userptr_t) stackptr, sizeof(userptr_t));

		KASSERT(stackptr % sizeof(userptr_t) == 0);
	}

	vaddr_t entrypoint;
	struct vnode *v;
	err = vfs_open(kprogram, O_RDONLY, 0, &v);
	if(err != 0) {
		goto err7;
	}

	err = load_elf(v, &entrypoint);
	if(err != 0) {
		vfs_close(v);
		goto err7;
	}

	vfs_close(v);

	as_destroy(oaddr);
	kfree(uptrs);
	kfree(kbuf);
	kfree(nargvlens);
	kfree(nargv);
	kfree(kprogram);

	lock_release(fork_exec_lock);

	enter_new_process(argc, (userptr_t) stackptr, NULL, stackptr, entrypoint);

	panic("enter_new_process in execv failed (even though it can't fail) :(\n");
	return EINVAL;

	err7:
		kfree(uptrs);
	err6:
		proc_setas(oaddr);
		as_activate();
		as_destroy(naddr);
	err5:
		for(int j = 0; j < i; j++)
			kfree(nargv[j]);
		kfree(kbuf);
	err4:
		kfree(nargvlens);
	err3:
		kfree(nargv);
	err2:
		kfree(kprogram);
	err1:
		lock_release(fork_exec_lock);
		return err;
}

int sys_waitpid(pid_t pid, userptr_t status, int *retval) {
	if(status == NULL)
		return EFAULT;

	int max = procarray_num(procs);
	if(pid < 0 || pid > max)
		return ESRCH;

	struct proc *child = PROCS(pid);
	if(child == NULL)
		return ESRCH;

	if(child->p_parent != curproc)	// doesn't need to be synchronized because p_parent
		return ECHILD;			// could only be changed if the parent process is already dead

	spinlock_acquire(&child->p_lock);
	if(child->exit_code == -1) {
		while(child->exit_code == -1) {
			wchan_sleep(child->p_wchan, &child->p_lock);
		}
	}
	spinlock_release(&child->p_lock);

	int err = copyout(&child->exit_code, status, sizeof(int));
	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = child->exit_code;

	int index = -1;
	max = procarray_num(curproc->p_children);
	for(int i = 0; i < max; i++) {
		if(procarray_get(curproc->p_children, i) == child) {
			index = i;
			break;
		}
	}
	KASSERT(index >= 0);
	procarray_remove(curproc->p_children, index);
	proc_destroy(child);

	return 0;
}

void sys__exit(int exitcode) {
	int max = procarray_num(curproc->p_children);
	int pids[max];
	int j = 0;

	for(int i = 0; i < max; i++) {
		struct proc *p = procarray_get(curproc->p_children, i);
		spinlock_acquire(&p->p_lock);	// protect against simultaneous parent/child exits
										// leaving unaware orphans
		if(p->exit_code != -1) {
			pids[j] = p->pid;			// waitpid the processes after the loop, otherwise
			j++;						// indexing gets messed up
		}
		else {
			p->p_parent = NULL;
		}

		spinlock_release(&p->p_lock);
	}

	for(int k = 0; k < j; k++) {
		sys_waitpid(pids[k], NULL, NULL);
	}

	spinlock_acquire(&curproc->p_lock);
	curproc->exit_code = _MKWAIT_EXIT(exitcode);
	spinlock_release(&curproc->p_lock);

	struct proc *corpse = NULL;
	spinlock_acquire(&coffin_lock);
	if(coffin != NULL) {
		corpse = coffin;
		coffin = NULL;
	}
	if(curproc->p_parent == NULL) {		// this proc is an orphan :(
		coffin = curproc;
	}
	spinlock_release(&coffin_lock);		// might be destroyed through coffin any point after this
	if(corpse != NULL)
		proc_destroy(corpse);	// can't be called while holding coffin_lock

	if(curproc->p_parent != NULL) {		// if in coffin, this code won't execute anyway
		spinlock_acquire(&curproc->p_lock);
		wchan_wakeone(curproc->p_wchan, &curproc->p_lock);	// signal waitpid
		spinlock_release(&curproc->p_lock);
	}

	thread_exit();	// Another thread might try to destroy this one before this line,
					// so I added a busy wait while loop to thread_destroy().
					// This also protects processes because we always destroy threads first.
}