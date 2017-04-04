/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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
 * SYNCHRONIZATION PROBLEM 1: SINGING COWS
 *
 * A cow has many children. Each baby cow puts on a performance by singing
 * lyrics to "Call Me Maybe." Like a good parent, the daddy cow must
 * sit through each one of its baby cow's performances until the end, in order
 * to say "Congratulations Baby N!" where N corresponds to the N-th baby cow.
 *
 * At any given moment, there is a single parent cow and possibly multiple
 * baby cows singing. The parent cow is not allowed to congratulate a baby
 * cow until that baby cow has finished singing. Your solution CANNOT
 * wait for ALL the cows to finish before starting to congratulate the babies.
 *
 * Here is an example of correct looking output:
...
Baby   1 Cow: Hot night, wind was blowin'
Baby   2 Cow: Ripped jeans, skin was showin'
Baby   4 Cow: Don't ask me, I'll never tell
Baby   5 Cow: And this is crazy
Baby   8 Cow: Hot night, wind was blowin'
Parent   Cow: Congratulations Baby 7!
Baby   1 Cow: And now you're in my way
Baby   2 Cow: And now you're in my way
Baby   4 Cow: Hey, I just met you
Baby   5 Cow: Pennies and dimes for a kiss
Baby   8 Cow: But now you're in my way
Parent   Cow: Congratulations Baby 1!
Baby   2 Cow: Ripped jeans, skin was showin'
Baby   4 Cow: I'd trade my soul for a wish
Baby   8 Cow: Hey, I just met you
Parent   Cow: Congratulations Baby 5!
Baby   2 Cow: Your stare was holdin'
Baby   4 Cow: But now you're in my way
Baby   8 Cow: Don't ask me, I'll never tell
Baby   2 Cow: Your stare was holdin'
Baby   4 Cow: Hot night, wind was blowin'
Baby   8 Cow: But now you're in my way
Baby   2 Cow: Your stare was holdin'
Baby   4 Cow: I'd trade my soul for a wish
Baby   8 Cow: But here's my number
Baby   2 Cow: Ripped jeans, skin was showin'
Baby   4 Cow: But now you're in my way
Baby   8 Cow: But now you're in my way
Parent   Cow: Congratulations Baby 2!
Baby   4 Cow: Your stare was holdin'
Baby   8 Cow: Hey, I just met you
Baby   4 Cow: And this is crazy
Baby   8 Cow: I wasn't looking for this
...
 */

#include <types.h>
#include <lib.h>
#include <wchan.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/errno.h>
#include <array.h>
#include "common.h"

#define NUM_LYRICS 16

const char *LYRICS[NUM_LYRICS] = {
    "I threw a wish in the well",
    "Don't ask me, I'll never tell",
    "I looked to you as it fell",
    "And now you're in my way",
    "I'd trade my soul for a wish",
    "Pennies and dimes for a kiss",
    "I wasn't looking for this",
    "But now you're in my way",
    "Your stare was holdin'",
    "Ripped jeans, skin was showin'",
    "Hot night, wind was blowin'",
    "Where do you think you're going, baby?",
    "Hey, I just met you",
    "And this is crazy",
    "But here's my number",
    "So call me, maybe!",
};

/*
 * Do not modify this!
 */
static
void sing(unsigned cow_num) {
    int r = random() % NUM_LYRICS;
    while (r != 0) {
        kprintf("Baby %3u Cow: %s\n", cow_num, LYRICS[r]);
        r = random() % NUM_LYRICS;
        thread_yield(); // cause some interleaving!
    }
}

DECLARRAY(baby_cow_args);
DEFARRAY(baby_cow_args, /*noinline*/);

// One of these structs should be passed from the main driver thread
// to the parent cow thread.
struct parent_cow_args {
    unsigned pc_num_babies;
    // Add stuff as necessary
    struct semaphore *pc_done_sem;
};

// One of these structs should be passed from the parent cow thread
// to each of the baby cow threads.
struct baby_cow_args {
    unsigned bc_num;
    // Add stuff as necessary
    struct baby_cow_argsarray *bc_donearray;
    struct lock *bc_donelock;
    struct cv *bc_donecv;
};

static
void
baby_cow(void *args, unsigned long junk) {
    (void) junk; // suppress unused warnings
    struct baby_cow_args *bcargs = (struct baby_cow_args *) args;

    sing(bcargs->bc_num);
    // Add this struct to the done array and signal the parent that a child
    // has finished.
    lock_acquire(bcargs->bc_donelock);
    if (baby_cow_argsarray_add(bcargs->bc_donearray, bcargs, NULL)) {
        panic("could not add baby cow args to done array");
    }
    cv_signal(bcargs->bc_donecv, bcargs->bc_donelock);
    lock_release(bcargs->bc_donelock);
}

static
void
parent_cow(void *args, unsigned long junk) {
    (void) junk; // suppress unused warnings
    struct parent_cow_args *pcargs = (struct parent_cow_args *) args;

    // Keep track of finished babies in this array
    struct baby_cow_argsarray *donearray = baby_cow_argsarray_create();
    if (donearray == NULL) {
        goto done;
    }
    // Lock to protect concurrent access to array
    struct lock *donelock = lock_create("done lock");
    if (donelock == NULL) {
        goto cleanup_array;
    }
    // Parent will wait on this CV for babies to finish
    struct cv *donecv = cv_create("done cv");
    if (donecv == NULL) {
        goto cleanup_lock;
    }

    // Spawn all the babies
    for (unsigned i = 0; i < pcargs->pc_num_babies; i++) {
        struct baby_cow_args *bcargs = kmalloc(sizeof(struct baby_cow_args));
        if (bcargs == NULL) {
            panic("could not spawn baby");
        }
        bcargs->bc_donearray = donearray;
        bcargs->bc_donelock = donelock;
        bcargs->bc_donecv = donecv;
        bcargs->bc_num = i;
        thread_fork_or_panic("baby", NULL, baby_cow, bcargs, 0);
    }

    // Wait to be signaled by a baby. Once the parent has been signaled,
    // the parent will remove the done babies from the array, congratulate
    // them, and wait for the next babies to finish.
    unsigned finished = 0;
    while (finished < pcargs->pc_num_babies) {
        lock_acquire(donelock);
        while (baby_cow_argsarray_num(donearray) == 0) {
            cv_wait(donecv, donelock);
        }
        while (baby_cow_argsarray_num(donearray) > 0) {
            struct baby_cow_args *done = baby_cow_argsarray_get(donearray, 0);
            kprintf("Parent   Cow: Congratulations Baby %u!\n", done->bc_num);
            baby_cow_argsarray_remove(donearray, 0);
            kfree(done);
            finished++;
        }
        lock_release(donelock);
    }

    // cleanup
    cv_destroy(donecv);
  cleanup_lock:
    lock_destroy(donelock);
  cleanup_array:
    baby_cow_argsarray_destroy(donearray);
  done:
    V(pcargs->pc_done_sem);
}

int
cows(int nargs, char **args) {
    // if an argument is passed, use that as the number of baby cows
    unsigned num_babies = 10;
    if (nargs == 2) {
        num_babies = atoi(args[1]);
    }

    int result;
    struct parent_cow_args *pcargs = kmalloc(sizeof(struct parent_cow_args));
    if (pcargs == NULL) {
        result = ENOMEM;
        goto done;
    }
    pcargs->pc_done_sem = sem_create("done sem", 0);
    if (pcargs->pc_done_sem == NULL) {
        result = ENOMEM;
        goto cleanup_kmalloc;
    }
    pcargs->pc_num_babies = num_babies;
    thread_fork_or_panic("parent", NULL, parent_cow, pcargs, 0);

    // wait for the parent cow thread to finish
    P(pcargs->pc_done_sem);
    result = 0; // success, fall through

    sem_destroy(pcargs->pc_done_sem);
  cleanup_kmalloc:
    kfree(pcargs);
  done:
    return result;
}
