/*
 * Copyright (c) 2015
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

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <clock.h>
#include <test.h>

/*
 * Unit tests for semaphores.
 *
 * We test 21 correctness criteria, each stated in a comment at the
 * top of each test.
 *
 * Note that these tests go inside the semaphore abstraction to
 * validate the internal state.
 *
 * All tests (apart from those that crash) attempt to clean up after
 * running, to avoid leaking memory and leaving extra threads lying
 * around. Tests with a cleanup phase call ok() before going to it in
 * case the cleanup crashes -- this should not happen of course but if
 * it does it should be distinguished from the main part of the test
 * itself dying.
 */

#define NAMESTRING "some-silly-name"

////////////////////////////////////////////////////////////
// support code

static unsigned waiters_running = 0;
static struct spinlock waiters_lock = SPINLOCK_INITIALIZER;

static
void
ok(void)
{
	kprintf("Test passed; now cleaning up.\n");
}

/*
 * Wrapper for sem_create when we aren't explicitly tweaking it.
 */
static
struct semaphore *
makesem(unsigned count)
{
	struct semaphore *sem;

	sem = sem_create(NAMESTRING, count);
	if (sem == NULL) {
		panic("semunit: whoops: sem_create failed\n");
	}
	return sem;
}

/*
 * A thread that just waits on a semaphore.
 */
static
void
waiter(void *vsem, unsigned long junk)
{
	struct semaphore *sem = vsem;
	(void)junk;

	P(sem);

	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running > 0);
	waiters_running--;
	spinlock_release(&waiters_lock);
}

/*
 * Set up a waiter.
 */
static
void
makewaiter(struct semaphore *sem)
{
	int result;

	spinlock_acquire(&waiters_lock);
	waiters_running++;
	spinlock_release(&waiters_lock);

	result = thread_fork("semunit waiter", NULL, waiter, sem, 0);
	if (result) {
		panic("semunit: thread_fork failed\n");
	}
	kprintf("Sleeping for waiter to run\n");
	clocksleep(1);
}

/*
 * Spinlocks don't natively provide this, because it only makes sense
 * under controlled conditions.
 *
 * Note that we should really read the holder atomically; but because
 * we're using this under controlled conditions, it doesn't actually
 * matter -- nobody is supposed to be able to touch the holder while
 * we're checking it, or the check wouldn't be reliable; and, provided
 * clocksleep works, nobody can.
 */
static
bool
spinlock_not_held(struct spinlock *splk)
{
	return splk->splk_holder == NULL;
}

////////////////////////////////////////////////////////////
// semaphore tests

/*
 * 1. After a successful sem_create:
 *     - sem_name compares equal to the passed-in name
 *     - sem_name is not the same pointer as the passed-in name
 *     - sem_wchan is not null
 *     - sem_lock is not held and has no owner
 *     - sem_count is the passed-in count
 */
int
semu1(int nargs, char **args)
{
	struct semaphore *sem;
	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	sem = sem_create(name, 56);
	if (sem == NULL) {
		panic("semu1: whoops: sem_create failed\n");
	}
	KASSERT(!strcmp(sem->sem_name, name));
	KASSERT(sem->sem_name != name);
	KASSERT(sem->sem_wchan != NULL);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 56);

	ok();
	/* clean up */
	sem_destroy(sem);
	return 0;
}

/*
 * 2. Passing a null name to sem_create asserts or crashes.
 */
int
semu2(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	kprintf("This should crash with a kernel null dereference\n");
	sem = sem_create(NULL, 44);
	(void)sem;
	panic("semu2: sem_create accepted a null name\n");
	return 0;
}

/*
 * 3. Passing a null semaphore to sem_destroy asserts or crashes.
 */
int
semu3(int nargs, char **args)
{
	(void)nargs; (void)args;

	kprintf("This should assert that sem != NULL\n");
	sem_destroy(NULL);
	panic("semu3: sem_destroy accepted a null semaphore\n");
}


/*
 * 4. sem_count is an unsigned type.
 */
int
semu4(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	/* Create a semaphore with count 0. */
	sem = makesem(0);
	/* Decrement the count. */
	sem->sem_count--;
	/* This value should be positive. */
	KASSERT(sem->sem_count > 0);

	/* Clean up. */
	ok();
	sem_destroy(sem);
	return 0;
}

/*
 * 5. A semaphore can be successfully initialized with a count of at
 * least 0xf0000000.
 */
int
semu5(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	sem = sem_create(NAMESTRING, 0xf0000000U);
	if (sem == NULL) {
		/* This might not be an innocuous malloc shortage. */
		panic("semu5: sem_create failed\n");
	}
	KASSERT(sem->sem_count == 0xf0000000U);

	/* Clean up. */
	ok();
	sem_destroy(sem);
	return 0;
}

/*
 * 6. Passing a semaphore with a waiting thread to sem_destroy asserts
 * (in the wchan code).
 */
int
semu6(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	sem = makesem(0);
	makewaiter(sem);
	kprintf("This should assert that the wchan's threadlist is empty\n");
	sem_destroy(sem);
	panic("semu6: wchan_destroy with waiters succeeded\n");
	return 0;
}

/*
 * 7. Calling V on a semaphore does not block the caller, regardless
 * of the semaphore count.
 */
int
semu7(int nargs, char **args)
{
	struct semaphore *sem;
	struct spinlock lk;

	(void)nargs; (void)args;

	sem = makesem(0);

	/*
	 * Check for blocking by taking a spinlock; if we block while
	 * holding a spinlock, wchan_sleep will assert.
	 */
	spinlock_init(&lk);
	spinlock_acquire(&lk);

	/* try with count 0, count 1, and count 2, just for completeness */
	V(sem);
	V(sem);
	V(sem);

	/* Clean up. */
	ok();
	spinlock_release(&lk);
	spinlock_cleanup(&lk);
	sem_destroy(sem);
	return 0;
}

/*
 * 8/9. After calling V on a semaphore with no threads waiting:
 *    - sem_name is unchanged
 *    - sem_wchan is unchanged
 *    - sem_lock is (still) unheld and has no owner
 *    - sem_count is increased by one
 *
 * This is true even if we are in an interrupt handler.
 */
static
void
do_semu89(bool interrupthandler)
{
	struct semaphore *sem;
	struct wchan *wchan;
	const char *name;

	sem = makesem(0);

	/* check preconditions */
	name = sem->sem_name;
	wchan = sem->sem_wchan;
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(spinlock_not_held(&sem->sem_lock));

	/*
	 * The right way to this is to set up an actual interrupt,
	 * e.g. an interprocessor interrupt, and hook onto it to run
	 * the V() in the actual interrupt handler. However, that
	 * requires a good bit of infrastructure that we don't
	 * have. Instead we'll fake it by explicitly setting
	 * curthread->t_in_interrupt.
	 */
	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == false);
		curthread->t_in_interrupt = true;
	}

	V(sem);

	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == true);
		curthread->t_in_interrupt = false;
	}

	/* check postconditions */
	KASSERT(name == sem->sem_name);
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(wchan == sem->sem_wchan);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 1);

	/* clean up */
	ok();
	sem_destroy(sem);
}

int
semu8(int nargs, char **args)
{
	(void)nargs; (void)args;

	do_semu89(false /*interrupthandler*/);
	return 0;
}

int
semu9(int nargs, char **args)
{
	(void)nargs; (void)args;

	do_semu89(true /*interrupthandler*/);
	return 0;
}

/*
 * 10/11. After calling V on a semaphore with one thread waiting, and giving
 * it time to run:
 *    - sem_name is unchanged
 *    - sem_wchan is unchanged
 *    - sem_lock is (still) unheld and has no owner
 *    - sem_count is still 0
 *    - the other thread does in fact run
 *
 * This is true even if we are in an interrupt handler.
 */
static
int
do_semu1011(bool interrupthandler)
{
	struct semaphore *sem;
	struct wchan *wchan;
	const char *name;

	sem = makesem(0);
	makewaiter(sem);

	/* check preconditions */
	name = sem->sem_name;
	wchan = sem->sem_wchan;
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(spinlock_not_held(&sem->sem_lock));
	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running == 1);
	spinlock_release(&waiters_lock);

	/* see above */
	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == false);
		curthread->t_in_interrupt = true;
	}

	V(sem);

	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == true);
		curthread->t_in_interrupt = false;
	}

	/* give the waiter time to exit */
	clocksleep(1);

	/* check postconditions */
	KASSERT(name == sem->sem_name);
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(wchan == sem->sem_wchan);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 0);
	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running == 0);
	spinlock_release(&waiters_lock);

	/* clean up */
	ok();
	sem_destroy(sem);
	return 0;

}

int
semu10(int nargs, char **args)
{
	(void)nargs; (void)args;

	do_semu1011(false /*interrupthandler*/);
	return 0;
}

int
semu11(int nargs, char **args)
{
	(void)nargs; (void)args;

	do_semu1011(true /*interrupthandler*/);
	return 0;
}


/*
 * 12/13. After calling V on a semaphore with two threads waiting, and
 * giving it time to run:
 *    - sem_name is unchanged
 *    - sem_wchan is unchanged
 *    - sem_lock is (still) unheld and has no owner
 *    - sem_count is still 0
 *    - one of the other threads does in fact run
 *    - the other one does not
 */
static
void
semu1213(bool interrupthandler)
{
	struct semaphore *sem;
	struct wchan *wchan;
	const char *name;

	sem = makesem(0);
	makewaiter(sem);
	makewaiter(sem);

	/* check preconditions */
	name = sem->sem_name;
	wchan = sem->sem_wchan;
	KASSERT(!strcmp(name, NAMESTRING));
	wchan = sem->sem_wchan;
	KASSERT(spinlock_not_held(&sem->sem_lock));
	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running == 2);
	spinlock_release(&waiters_lock);

	/* see above */
	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == false);
		curthread->t_in_interrupt = true;
	}

	V(sem);

	if (interrupthandler) {
		KASSERT(curthread->t_in_interrupt == true);
		curthread->t_in_interrupt = false;
	}

	/* give the waiter time to exit */
	clocksleep(1);

	/* check postconditions */
	KASSERT(name == sem->sem_name);
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(wchan == sem->sem_wchan);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 0);
	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running == 1);
	spinlock_release(&waiters_lock);

	/* clean up */
	ok();
	V(sem);
	clocksleep(1);
	spinlock_acquire(&waiters_lock);
	KASSERT(waiters_running == 0);
	spinlock_release(&waiters_lock);
	sem_destroy(sem);
}

int
semu12(int nargs, char **args)
{
	(void)nargs; (void)args;

	semu1213(false /*interrupthandler*/);
	return 0;
}

int
semu13(int nargs, char **args)
{
	(void)nargs; (void)args;

	semu1213(true /*interrupthandler*/);
	return 0;
}

/*
 * 14. Calling V on a semaphore whose count is the maximum allowed value
 * asserts.
 */
int
semu14(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	kprintf("This should assert that sem_count is > 0.\n");
	sem = makesem(0);

	/*
	 * The maximum value is (unsigned)-1. Get this by decrementing
	 * from 0.
	 */
	sem->sem_count--;
	V(sem);
	KASSERT(sem->sem_count == 0);
	panic("semu14: V tolerated count wraparound\n");
	return 0;
}

/*
 * 15. Calling V on a null semaphore asserts.
 */
int
semu15(int nargs, char **args)
{
	(void)nargs; (void)args;

	kprintf("This should assert that the semaphore isn't null.\n");
	V(NULL);
	panic("semu15: V tolerated null semaphore\n");
	return 0;
}

/*
 * 16. Calling P on a semaphore with count > 0 does not block the caller.
 */
int
semu16(int nargs, char **args)
{
	struct semaphore *sem;
	struct spinlock lk;

	(void)nargs; (void)args;

	sem = makesem(1);

	/* As above, check for improper blocking by taking a spinlock. */
	spinlock_init(&lk);
	spinlock_acquire(&lk);

	P(sem);

	ok();
	spinlock_release(&lk);
	spinlock_cleanup(&lk);
	sem_destroy(sem);
	return 0;
}

/*
 * 17. Calling P on a semaphore with count == 0 does block the caller.
 */

static struct thread *semu17_thread;

static
void
semu17_sub(void *semv, unsigned long junk)
{
	struct semaphore *sem = semv;

	(void)junk;

	semu17_thread = curthread;

	/* precondition */
	KASSERT(sem->sem_count == 0);

	P(sem);
}

int
semu17(int nargs, char **args)
{
	struct semaphore *sem;
	int result;

	(void)nargs; (void)args;

	semu17_thread = NULL;

	sem = makesem(0);
	result = thread_fork("semu17_sub", NULL, semu17_sub, sem, 0);
	if (result) {
		panic("semu17: whoops: thread_fork failed\n");
	}
	kprintf("Waiting for subthread...\n");
	clocksleep(1);

	/* The subthread should be blocked. */
	KASSERT(semu17_thread != NULL);
	KASSERT(semu17_thread->t_state == S_SLEEP);

	/* Clean up. */
	ok();
	V(sem);
	clocksleep(1);
	sem_destroy(sem);
	semu17_thread = NULL;
	return 0;
}

/*
 * 18. After calling P on a semaphore with count > 0:
 *    - sem_name is unchanged
 *    - sem_wchan is unchanged
 *    - sem_lock is unheld and has no owner
 *    - sem_count is one less
 */
int
semu18(int nargs, char **args)
{
	struct semaphore *sem;
	struct wchan *wchan;
	const char *name;

	(void)nargs; (void)args;

	sem = makesem(1);

	/* preconditions */
	name = sem->sem_name;
	KASSERT(!strcmp(name, NAMESTRING));
	wchan = sem->sem_wchan;
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 1);

	P(sem);
	
	/* postconditions */
	KASSERT(name == sem->sem_name);
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(wchan == sem->sem_wchan);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 0);

	return 0;
}

/*
 * 19. After calling P on a semaphore with count == 0 and another
 * thread uses V exactly once to cause a wakeup:
 *    - sem_name is unchanged
 *    - sem_wchan is unchanged
 *    - sem_lock is unheld and has no owner
 *    - sem_count is still 0
 */

static
void
semu19_sub(void *semv,  unsigned long junk)
{
	struct semaphore *sem = semv;

	(void)junk;

	kprintf("semu19: waiting for parent to sleep\n");
	clocksleep(1);
	/*
	 * We could assert here that the parent *is* sleeping; but for
	 * that we'd need its thread pointer and it's not worth the
	 * trouble.
	 */
	V(sem);
}

int
semu19(int nargs, char **args)
{
	struct semaphore *sem;
	struct wchan *wchan;
	const char *name;
	int result;

	(void)nargs; (void)args;

	sem = makesem(0);
	result = thread_fork("semu19_sub", NULL, semu19_sub, sem, 0);
	if (result) {
		panic("semu19: whoops: thread_fork failed\n");
	}

	/* preconditions */
	name = sem->sem_name;
	KASSERT(!strcmp(name, NAMESTRING));
	wchan = sem->sem_wchan;
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 0);

	P(sem);

	/* postconditions */
	KASSERT(name == sem->sem_name);
	KASSERT(!strcmp(name, NAMESTRING));
	KASSERT(wchan == sem->sem_wchan);
	KASSERT(spinlock_not_held(&sem->sem_lock));
	KASSERT(sem->sem_count == 0);

	return 0;
}

/*
 * 20/21. Calling P in an interrupt handler asserts, regardless of the
 * count.
 */
int
semu20(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	kprintf("This should assert that we aren't in an interrupt\n");

	sem = makesem(0);
	/* as above */
	curthread->t_in_interrupt = true;
	P(sem);
	panic("semu20: P tolerated being in an interrupt handler\n");
	return 0;
}

int
semu21(int nargs, char **args)
{
	struct semaphore *sem;

	(void)nargs; (void)args;

	kprintf("This should assert that we aren't in an interrupt\n");

	sem = makesem(1);
	/* as above */
	curthread->t_in_interrupt = true;
	P(sem);
	panic("semu21: P tolerated being in an interrupt handler\n");
	return 0;
}

/*
 * 22. Calling P on a null semaphore asserts.
 */
int
semu22(int nargs, char **args)
{
	(void)nargs; (void)args;

	kprintf("This should assert that the semaphore isn't null.\n");
	P(NULL);
	panic("semu22: P tolerated null semaphore\n");
	return 0;
}

////////////////////////////////////////////////////////////
// lock and cv tests

/*
 * 1. A thread will error if it tries to acquire a lock it already holds.
 */
int
ut1(int nargs, char **args)
{
	struct lock *lock;
	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	lock = lock_create(name);
	if (lock == NULL) {
		panic("ut1: whoops: lock_create failed\n");
	}
	lock_acquire(lock);
	kprintf("Should panic: lock_acquire: You already hold lock some-silly-name\n");
	lock_acquire(lock);

	panic("ut1: lock didn't error when it was double acquired\n");
	return 0;
}

/*
 * 2. Passing a null lock to a lock_destroy will error.
 */
int
ut2(int nargs, char **args)
{
	(void)nargs; (void)args;
	
	struct lock *naughty = NULL;	
	kprintf("Should fail assertion: lock != NULL\n");
	lock_destroy(naughty);

	panic("ut2: lock didn't error when it destroyed NULL\n");
	return 0;
}

/*
 * 3. A thread will error if it releases a lock it doesn’t hold.
 */
int
ut3(int nargs, char **args)
{
	struct lock *lock;
	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	lock = lock_create(name);
	if (lock == NULL) {
		panic("ut3: whoops: lock_create failed\n");
	}
	kprintf("Should panic: lock_release: You don't hold lock some-silly-name\n");
	lock_release(lock);

	panic("ut3: lock didn't error when it was released without ownership\n");
	return 0;
}

/*
 * Helper function for ut4.
 */
static
void
ut4helper(void* vcv, unsigned long ullock)
{
	struct cv *cv = vcv;
	//struct lock *lock = (struct lock *) ullock;
	lock_acquire((struct lock*)ullock);

	kprintf("Should fail assertion: threadlist_isempty(tl)\n");
	cv_destroy(cv);

	panic("thread: no error when cv with non-empty wchan was destroyed\n");
}

/*
 * 4. A CV will error if destroyed while its wait channel isn’t empty.
 */
int
ut4(int nargs, char **args)
{
	struct cv *cv;
	struct lock *lock;
	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	cv = cv_create(name);
	if (cv == NULL) {
		panic("ut4: whoops: cv_create failed\n");
	}

	lock = lock_create(name);
	if (lock == NULL) {
		panic("ut4: whoops: lock_create failed\n");
	}
	lock_acquire(lock);

	int result;
	result = thread_fork("ut4helper", NULL, ut4helper, (void*)cv, (unsigned long)lock);
	if (result) {
		panic("ut4: thread_fork failed\n");
	}
	cv_wait(cv, lock);

	panic("ut4: thread was unslept when cv was destroyed\n");
	return 0;
}

/*
 * 5. A CV will error if a thread tries to signal using a lock it doesn’t
 * own.
 */
int
ut5(int nargs, char **args)
{
	struct cv *cv;	
	struct lock *lock;
	const char *name = NAMESTRING;

	(void)nargs; (void)args;

	cv = cv_create(name);
	if (cv == NULL) {
		panic("ut5: whoops: cv_create failed\n");
	}

	lock = lock_create(name);
	if (lock == NULL) {
		panic("ut5: whoops: lock_create failed\n");
	}
	kprintf("Should panic: cv_signal: You don't hold lock some-silly-name\n");
	cv_signal(cv, lock);

	panic("ut5: CV didn't error when it signaled without owning the lock\n");
	return 0;
}
