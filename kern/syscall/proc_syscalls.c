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


int sys_getpid(int *retval) {
	KASSERT(retval != NULL);
	*retval = curproc->pid;
	return 0;
}

int sys_fork(struct trapframe *tf, int *retval) {
	int err = 0;

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

	memcpy(newp->p_fds, curproc->p_fds, MAX_FDS * sizeof(int));

	err = thread_fork(curthread->t_name, newp, enter_forked_process, (void *)newtf, 0);
	if(err != 0) {
		err = ENOMEM;
		goto err4;
	}

	if(retval != NULL)
		*retval = newp->pid;

	return 0;

	// error cleanup

	err4:
		procarray_remove(curproc->p_children, index);
	err3:
		proc_destroy(newp);
	err2:
		kfree(newtf);
	err1:
		return err;
}

int sys_execv(const userptr_t program, userptr_t argv) {
	if(program == NULL || argv == NULL)
		return EFAULT;
	(void)program;
	(void)argv;
	// do stuff
	return 0;
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
	if(coffin != NULL) {
		proc_destroy(coffin);
		coffin = NULL;
	}
	if(curproc->p_parent == NULL) {		// this proc is an orphan :(
		coffin = curproc;
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