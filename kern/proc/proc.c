/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <wchan.h>
#include <vfs.h>


/*
 * Helper function for proc_create(). Iterates through procs, and sets
 * proc's pid to the first empty slot (or adds a new one if necessary).
 */
static int set_pid(struct proc *proc) {
	int err = 0;

	spinlock_acquire(&gp_lock);			// protect additions to global proc array

	int max = procarray_num(procs);
	pid_t pid = -1;
	for(int i = 0; i < max; i++) {					// find NULL slot
		if(PROCS(i) == NULL) {						// e.g. from exited processes with pids in the middle of the array
			pid = i;								// pid indexing means we can't remove them from anywhere but the end
			break;
		}
	}

	if(pid < 0) {
		err = procarray_add(procs, proc, NULL);		// if no NULL slot exists, add one instead
		if(err == 0)
			proc->pid = max;
	}
	else {
		procarray_set(procs, pid, proc);			// else fill NULL slot
		proc->pid = pid;
	}

	spinlock_release(&gp_lock);

	return err;
}

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL)
		goto err1;

	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL)
		goto err2;

	proc->p_children = procarray_create();
	if(proc->p_children == NULL)
		goto err3;

	proc->p_wchan = wchan_create(proc->p_name);
	if(proc->p_wchan == NULL)
		goto err4;

	if(set_pid(proc) != 0) 		// set proc to first free pid
		goto err5;

	spinlock_init(&proc->p_lock);
	proc->p_numthreads = 0;
	proc->p_addrspace = NULL;
	proc->p_cwd = NULL;
	proc->p_parent = NULL;
	proc->exit_code = -1;
	proc->tx = NULL;

	memset(proc->p_fds, -1, OPEN_MAX * sizeof(int));		// default value of per-process fd is -1

	return proc;

	// error cleanup

	err5:
		wchan_destroy(proc->p_wchan);
	err4:
		kfree(proc->p_children);
	err3:
		kfree(proc->p_name);
	err2:
		kfree(proc);
	err1:
		return NULL;
}

/*
 * Destroy a proc structure.
 *
 * You may not hold a spinlock while calling this.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	spinlock_acquire(&proc->p_lock);
	while(proc->p_numthreads != 0) {	// wait for thread_exit() to finish
		spinlock_release(&proc->p_lock);
		thread_yield();
		spinlock_acquire(&proc->p_lock);
	}
	spinlock_release(&proc->p_lock);

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	wchan_destroy(proc->p_wchan);

	spinlock_acquire(&gp_lock);
	unsigned pid = proc->pid;
	procarray_set(procs, proc->pid, NULL);		// remove entry from procs array
	while(pid == procarray_num(procs) - 1) {	// last element in array
		if(PROCS(pid) != NULL)					
			break;
		procarray_remove(procs, pid);			// purge NULL entries from end of array
		pid--;
	}
	spinlock_release(&gp_lock);

	for(int i = procarray_num(proc->p_children); i > 0; i--) {
		procarray_remove(proc->p_children, 0);
	}
	procarray_destroy(proc->p_children); 

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	procs = procarray_create();		// create global procs struct
	if(procs == NULL) {
		panic("procarray_create for procs failed\n");
	}

	kproc = proc_create("[kernel]");
	if(kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}

	spinlock_init(&gp_lock);
	spinlock_init(&coffin_lock);

	fork_exec_lock = lock_create("fork_exec_lock");
	if(fork_exec_lock == NULL) {
		panic("lock_create for fork_exec_lock failed\n");
	}

	// only ARG_MAX / 4 parameters are allowed because otherwise it uses so much memory
	nargvlens = kmalloc(ARG_MAX/4 * sizeof(size_t));
	if(nargvlens == NULL) {
		panic("kmalloc for nargvlens failed\n");
	}

	nargv = kmalloc(ARG_MAX/4 * sizeof(char *));
	if(nargv == NULL) {
		panic("kmalloc for nargv failed\n");
	}

	nbuf = kmalloc(ARG_MAX * sizeof(char));
	if(nbuf == NULL) {
		panic("kmalloc for nbuf failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	for(int i = 0; i < OPEN_MAX; i++) {		// duplicate all open file descriptors
		if(curproc->p_fds[i] >= 0) {		
			struct vfile *vf = vfilearray_get(vfiles, curproc->p_fds[i]);

			spinlock_acquire(&vf->vf_lock);		// duplicate some functionality from sys_close
												// because here we use an arbitrary proc, not curproc
			KASSERT(vf->vf_refcount > 0);
			vf->vf_refcount++;

			spinlock_release(&vf->vf_lock);

			newproc->p_fds[i] = curproc->p_fds[i];
		}
	}

	newproc->p_parent = curproc;

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
