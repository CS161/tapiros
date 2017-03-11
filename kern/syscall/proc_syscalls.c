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
	*retval = curproc->pid;		// return pid
	return 0;
}

int sys_fork(struct trapframe *tf, int *retval) {
	int err = 0;

	lock_acquire(fork_exec_lock);	// fork and exec are memory intensive, so we don't want multiple
									// running simultaneously

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
		
	newp->p_parent = curproc;

	err = thread_fork(curthread->t_name, newp, enter_forked_process, (void *)newtf, 0);
	if(err != 0) {	// release our baby into the dangerous world that is the cpu runqueue
		err = ENOMEM;
		goto err4;
	}

	if(retval != NULL)
		*retval = newp->pid;	// return the child's pid

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
	int err = 0;

	lock_acquire(fork_exec_lock);
	
	char *kprogram = kmalloc(sizeof(char) * PATH_MAX);
	if(kprogram == NULL)
		goto err1;

	size_t klen = 0;
	err = copyinstr(program, kprogram, PATH_MAX, &klen);	// move program into kernel space
	
	if(err != 0) {
		goto err2;
	}

	// only ARG_MAX / 4 parameters are allowed because otherwise memory runs out way too quickly.
	// once kfree() actually does something, we can change this back to ARG_MAX

	char **nargv = kmalloc(ARG_MAX/4 * sizeof(char *));	// for the strings argv points to
	if(nargv == NULL) {
		err = ENOMEM;
		goto err2;
	}

	size_t *nargvlens = kmalloc(ARG_MAX/4 * sizeof(size_t));	// length of each string
	if(nargvlens == NULL) {
		err = ENOMEM;
		goto err3;
	}

	char *kbuf = kmalloc(ARG_MAX * sizeof(char));	// intermediate buffer of maximum length
	if(kbuf == NULL) {								// before transferring into one of the right size
		err = ENOMEM;
		goto err4;
	}

	int i = 0;
	size_t argvlen = 0;
	while(1)	{	// extract parameter strings and lengths
		userptr_t uptr = NULL;
		err = copyin(argv, &uptr, sizeof(userptr_t));	// get argv[i] basically (userptr_t)
		if(err != 0)
			goto err5;

		if(uptr == NULL)
			break;

		klen = 0;
		err = copyinstr(uptr, kbuf, ARG_MAX, &klen);	// get *argv[i] basically (in kbuf)
		if(err != 0)
			goto err5;
		
		char *nargvi = kmalloc(klen * sizeof(char));
		if(nargvi == NULL) {
			err = ENOMEM;
			goto err5;
		}

		argvlen += klen;								// keep track of total string length
		if(argvlen > ARG_MAX || i > ARG_MAX/4) {		// total parameter length is too long
			err = E2BIG;
			goto err5;
		}

		memcpy(nargvi, kbuf, klen * sizeof(char));	// copy kbuf's contents into a region without extra space
		nargv[i] = nargvi;
		nargvlens[i] = klen;

		i++;
		argv += sizeof(userptr_t);	// go to argv[i+1] (but you can't use that syntax with userptr_t)
	}

	int argc = i;

	struct addrspace *naddr = as_create();				// make a new address space
	struct addrspace *oaddr = curproc->p_addrspace;		// but keep the old one in case execv fails and we need to abort
	if(naddr == NULL) {
		err = ENOMEM;
		goto err5;
	}

	proc_setas(naddr);
	as_activate();

	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	err = vfs_open(kprogram, O_RDONLY, 0, &v);
	if(err != 0) {
		goto err6;
	}

	err = load_elf(v, &entrypoint);		// load executable
	if(err != 0) {
		vfs_close(v);
		goto err6;
	}

	vfs_close(v);

	err = as_define_stack(naddr, &stackptr);	// create stack
	if(err != 0)
		goto err6;

	userptr_t *uptrs = kmalloc(argc * sizeof(userptr_t));	// keep track of where on the new stack params go
	if(uptrs == NULL) {
		err = ENOMEM;
		goto err6;
	}

	char zeros[sizeof(userptr_t)];
	memset(zeros, 0, sizeof(userptr_t));
	while(i > 0) {										// fill new stack with parameter strings
		i--;
		int nzeros = sizeof(userptr_t) - (nargvlens[i] % sizeof(userptr_t));	// pad the end with 0s to be 4-aligned (on 32-bit)
		if(nzeros > 0 && nzeros < 4) {
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

		KASSERT(stackptr % sizeof(userptr_t) == 0);					// make sure alignment logic works
	}
	stackptr -= sizeof(userptr_t);									// null-terminate argv
	err = copyout(zeros, (userptr_t) stackptr, sizeof(userptr_t));
	if(err != 0)
		goto err7;

	i = argc;
	while(i > 0) {							// populate argv pointers on the new stack
		i--;
		stackptr -= sizeof(userptr_t);
		userptr_t uptr = uptrs[i];

		err = copyout(&uptr, (userptr_t) stackptr, sizeof(userptr_t));
		if(err != 0)
			goto err7;

		KASSERT(stackptr % sizeof(userptr_t) == 0);
	}

	as_destroy(oaddr);		// clean up all the kmalloced vars
	kfree(uptrs);
	kfree(kbuf);
	kfree(nargvlens);
	kfree(nargv);
	kfree(kprogram);


	lock_release(fork_exec_lock);

	enter_new_process(argc, (userptr_t) stackptr, NULL, stackptr, entrypoint);

	panic("enter_new_process in execv failed (even though it can't fail) :(\n");
	return EINVAL;

	// error cleanup

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

int sys_waitpid(pid_t pid, int *status, int *retval) {
	int max = procarray_num(procs);			// more naughty user mistakes
	if(pid < 0 || pid >= max)
		return ESRCH;

	struct proc *child = PROCS(pid);
	if(child == NULL)
		return ESRCH;

	if(child->p_parent != curproc)			// doesn't need to be synchronized because p_parent
		return ECHILD;						// could only be changed if the parent process is already dead

	spinlock_acquire(&child->p_lock);
	if(child->exit_code == -1) {			// the "wait" part of waitpid
		wchan_sleep(child->p_wchan, &child->p_lock);
	}
	spinlock_release(&child->p_lock);

	if(status != NULL)
		*status = child->exit_code;

	if(retval != NULL)
		*retval = child->pid;

	int index = -1;
	max = procarray_num(curproc->p_children);
	for(int i = 0; i < max; i++) {
		if(procarray_get(curproc->p_children, i) == child) {	// find this child's index in curproc's list
			index = i;
			break;
		}
	}
	if(index >= 0)	// kproc doesn't keep track of children because it always blocks
		procarray_remove(curproc->p_children, index);
	proc_destroy(child);

	return 0;
}

void sys__exit(int exitcode, int codetype) {
	int max = procarray_num(curproc->p_children);
	int pids[max];
	int j = 0;

	for(int i = 0; i < OPEN_MAX; i++) {		// close all open file descriptors
		sys_close(i);						// sys_close handles invalid fds
	}

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
		sys_waitpid(pids[k], NULL, NULL);	// reap children who have already exited
	}

	spinlock_acquire(&curproc->p_lock);

	if(codetype == 0)
		curproc->exit_code = _MKWAIT_EXIT(exitcode);	// process exit code
	if(codetype == 1)
		curproc->exit_code = _MKWAIT_SIG(exitcode);		// process signal

	spinlock_release(&curproc->p_lock);

	struct proc *corpse = NULL;						// use coffin method to handle orphaned processes
	spinlock_acquire(&coffin_lock);
	if(coffin != NULL) {
		corpse = coffin;
		coffin = NULL;
	}
	if(curproc->p_parent == NULL) {					// this proc is an orphan :(
		coffin = curproc;
	}

	spinlock_release(&coffin_lock);					// this proc might be destroyed through the coffin any point after this
	if(corpse != NULL)
		proc_destroy(corpse);						// destroy old coffin process
													// can't be called while holding coffin_lock

	if(curproc->p_parent != NULL) {					// if in coffin, this code won't execute anyway
		spinlock_acquire(&curproc->p_lock);
		wchan_wakeone(curproc->p_wchan, &curproc->p_lock);	// signal waitpid
		spinlock_release(&curproc->p_lock);
	}

	thread_exit();
}