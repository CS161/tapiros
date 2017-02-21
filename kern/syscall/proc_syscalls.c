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


int sys_getpid(int *retval) {
	KASSERT(retval != NULL);
	*retval = curproc->pid;
	return 0;
}

int sys_fork(int *retval) {
	(void)retval;
	// do stuff
	return 0;
}

int sys_execv(const userptr_t program, userptr_t argv) {
	(void)program;
	(void)argv;
	// do stuff
	return 0;
}

int sys_waitpid(pid_t pid, userptr_t status, int *retval) {
	(void)pid;
	(void)status;
	(void)retval;
	// do stuff
	return 0;
}

void sys__exit(int exitcode) {
	int max = procarray_num(curproc->p_children);
	for(int i = 0; i < max; i++) {
		struct proc *p = procarray_get(curproc->p_children, i);
		spinlock_acquire(&p->p_lock);	// protect against simultaneous parent/child exits
										// leaving unaware orphans
		if(p->exit_code != -1) {
			sys_waitpid(p->pid, NULL, NULL);
		}
		else {
			p->p_parent = NULL;
		}
		spinlock_release(&p->p_lock);
	}

	spinlock_acquire(&curproc->p_lock);
	curproc->exit_code = _MKWAIT_EXIT(exitcode);
	spinlock_release(&curproc->p_lock);

	spinlock_acquire(&coffin_lock);
	if(t_coffin != NULL) {
		thread_destroy(t_coffin);
		t_coffin = NULL;
	}
	if(p_coffin != NULL) {
		proc_destroy(p_coffin);
		p_coffin = NULL;
	}
	if(curproc->p_parent == NULL) {		// this thread is an orphan :(
		t_coffin = curthread;
		KASSERT(curproc->p_numthreads == 1);	// no multithreading, so should always be true
		if(curproc->p_numthreads == 1)
			p_coffin = curproc;
	}
	spinlock_release(&coffin_lock);		// might be destroyed through coffin any point after this

	if(curproc->p_parent != NULL) {		// if in coffin, this code won't execute anyway
		spinlock_acquire(&curproc->p_lock);
		wchan_wakeone(curproc->p_wchan, &curproc->p_lock);	// signal waitpid
		spinlock_release(&curproc->p_lock);
	}

	thread_exit();	// Another thread might try to destroy this one before this line,
					// so I added a busy wait while loop to thread_destroy().
					// This also protects processes because we always destroy threads first.
}