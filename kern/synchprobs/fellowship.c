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

/**
 * Driver code for The Fellowship of the Ring synch problem.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#include "common.h"


///////////////////////////////////////////////////////////////////////////////
//
//  Name functions for the races of Middle-Earth.
//
//  Your solution should print NFOTRS full fellowships to stdout, each on a
//  separate line.  Each such fellowship should have the form:
//
//    n: wizard, man, man, elf, dwarf, hobbit, hobbit, hobbit, hobbit
//
//  where each member of each race is identified by name using these helper
//  routines (e.g., nameof_istari()), and `n' is some unique identifier for the
//  fellowship.  The threads can exit once the full fellowship is printed, and
//  should individually print out
//
//    name: n
//
//  where `name' is its name, and `n' is the identifier of the fellowship it
//  joined.
//

#define NAMEOF_FUNC(race)   \
  static const char *       \
  nameof_##race(int which)  \
  {                         \
    return race[which];     \
  }

NAMEOF_FUNC(istari);
NAMEOF_FUNC(menfolk);
NAMEOF_FUNC(eldar);
NAMEOF_FUNC(khazad);
NAMEOF_FUNC(hobbitses);

#undef NAMEOF_FUNC


///////////////////////////////////////////////////////////////////////////////
//
//  Driver code.
//
//  TODO: Implement all the thread entrypoints!
//

struct fotr {
  int n;
  int wizard;
  int men[MEN_PER_FOTR];
  int elf;
  int dwarf;
  int hobbits[HOBBITS_PER_FOTR];
};

static struct fotr *
fotr_new() {
  int i;
  struct fotr *fotr;

  fotr = kmalloc(sizeof(*fotr));
  if (fotr == NULL) {
    panic("fellowship: Out of memory\n");
  }

  fotr->n = 0;

  fotr->wizard = -1;
  fotr->elf = -1;
  fotr->dwarf = -1;
  for (i = 0; i < MEN_PER_FOTR; ++i) {
    fotr->men[i] = -1;
  }
  for (i = 0; i < HOBBITS_PER_FOTR; ++i) {
    fotr->hobbits[i] = -1;
  }

  return fotr;
}

struct semaphore *rivendell;
struct fotr *fotrs[NFOTRS];
struct lock *locks[NFOTRS];
struct cv *cvs[NFOTRS];

static void
fotr_print(int i)
{
  struct fotr *fotr;

  fotr = fotrs[i];

  kprintf("%d: %s, %s, %s, %s, %s, %s, %s, %s, %s\n", i,
      nameof_istari(fotr->wizard), nameof_menfolk(fotr->men[0]),
      nameof_menfolk(fotr->men[1]), nameof_eldar(fotr->elf),
      nameof_khazad(fotr->dwarf), nameof_hobbitses(fotr->hobbits[0]),
      nameof_hobbitses(fotr->hobbits[1]), nameof_hobbitses(fotr->hobbits[2]),
      nameof_hobbitses(fotr->hobbits[3]));
}

/**
 * fotr_join - Add ourselves to a FotR by incrementing the membership count.
 * If we complete the FotR, wake the other members; else wait.
 *
 * We should be holding locks[i] upon entering fotr_join().
 */
static void
fotr_join(int i)
{
  if (++(fotrs[i]->n) == 9) {
    cv_broadcast(cvs[i], locks[i]);
  } else {
    cv_wait(cvs[i], locks[i]);
  }

  // The Fellowship is complete.  Print the full roster if we're the first to
  // wake up.
  if (fotrs[i]->n == 9) {
    fotr_print(i);
  }

  // Remove our reference to the roster and free it if we're the last one;
  // then, depart from Rivendell!
  if (--(fotrs[i]->n) == 0) {
    kfree(fotrs[i]);
    lock_release(locks[i]);
    V(rivendell);
    return;
  }

  lock_release(locks[i]);
}

static void
wizard(void *p, unsigned long which)
{
  (void)p;

  int i;

  for (i = 0; i < NFOTRS; ++i) {
    lock_acquire(locks[i]);

    if (fotrs[i] == NULL) {
      fotrs[i] = fotr_new();
    }

    if (fotrs[i]->wizard == -1) {
      fotrs[i]->wizard = which;
      fotr_join(i);

      kprintf("%s: %d\n", nameof_istari(which), i);
      return;
    }

    lock_release(locks[i]);
  }
}

static void
man(void *p, unsigned long which)
{
  (void)p;

  int i, j;

  for (i = 0; i < NFOTRS; ++i) {
    lock_acquire(locks[i]);

    if (fotrs[i] == NULL) {
      fotrs[i] = fotr_new();
    }

    for (j = 0; j < MEN_PER_FOTR; ++j) {
      if (fotrs[i]->men[j] == -1) {
        fotrs[i]->men[j] = which;
        fotr_join(i);

        kprintf("%s: %d\n", nameof_menfolk(which), i);
        return;
      }
    }

    lock_release(locks[i]);
  }
}

static void
elf(void *p, unsigned long which)
{
  (void)p;

  int i;

  for (i = 0; i < NFOTRS; ++i) {
    lock_acquire(locks[i]);

    if (fotrs[i] == NULL) {
      fotrs[i] = fotr_new();
    }

    if (fotrs[i]->elf == -1) {
      fotrs[i]->elf = which;
      fotr_join(i);

      kprintf("%s: %d\n", nameof_eldar(which), i);
      return;
    }

    lock_release(locks[i]);
  }
}

static void
dwarf(void *p, unsigned long which)
{
  (void)p;

  int i;

  for (i = 0; i < NFOTRS; ++i) {
    lock_acquire(locks[i]);

    if (fotrs[i] == NULL) {
      fotrs[i] = fotr_new();
    }

    if (fotrs[i]->dwarf == -1) {
      fotrs[i]->dwarf = which;
      fotr_join(i);

      kprintf("%s: %d\n", nameof_khazad(which), i);
      return;
    }

    lock_release(locks[i]);
  }
}

static void
hobbit(void *p, unsigned long which)
{
  (void)p;

  int i, j;

  for (i = 0; i < NFOTRS; ++i) {
    lock_acquire(locks[i]);

    if (fotrs[i] == NULL) {
      fotrs[i] = fotr_new();
    }

    for (j = 0; j < HOBBITS_PER_FOTR; ++j) {
      if (fotrs[i]->hobbits[j] == -1) {
        fotrs[i]->hobbits[j] = which;
        fotr_join(i);

        kprintf("%s: %d\n", nameof_hobbitses(which), i);
        return;
      }
    }

    lock_release(locks[i]);
  }
}

/**
 * fellowship - Fellowship synch problem driver routine.
 *
 * You may modify this function to initialize any synchronization primitives
 * you need; however, any other data structures you need to solve the problem
 * must be handled entirely by the forked threads (except for some freeing at
 * the end).  Feel free to change the thread forking loops if you wish to use
 * the same entrypoint routine to implement multiple Middle-Earth races.
 *
 * Make sure you don't leak any kernel memory!  Also, try to return the test to
 * its original state so it can be run again.
 */
int
fellowship(int nargs, char **args)
{
  int i;

  (void)nargs;
  (void)args;

  for (i = 0; i < NFOTRS; ++i) {
    locks[i] = lock_create("fotr");
    cvs[i] = cv_create("fotr");
  }
  rivendell = sem_create("fotr", 0);

  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("wizard", wizard, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("elf", elf, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("dwarf", dwarf, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS * MEN_PER_FOTR; ++i) {
    thread_fork_or_panic("man", man, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS * HOBBITS_PER_FOTR; ++i) {
    thread_fork_or_panic("hobbit", hobbit, NULL, i, NULL);
  }

  for (i = 0; i < NFOTRS; ++i) {
    P(rivendell);
  }

  for (i = 0; i < NFOTRS; ++i) {
    fotrs[i] = NULL;
    lock_destroy(locks[i]);
    cv_destroy(cvs[i]);
  }
  sem_destroy(rivendell);

  return 0;
}
