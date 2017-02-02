/*
 * Driver code for pizza problem
 */
#include <types.h>
#include <lib.h>
#include <test.h>
#include <thread.h>
#include <synch.h>

#define NSHELVES  10
#define NMAKERS   5
#define NSTUDENTS 20
#define NPIZZAS   10

struct cv *cv_full = NULL;
struct cv *cv_empty = NULL;
struct lock *lock_shelves = NULL;

int nPizzasAvailable = 0;
int nPizzasLeft  = NMAKERS * NPIZZAS;


static void
maker(void *p, unsigned long which)
{
  int i;

  (void)p;
  for(i = 0; i<NPIZZAS; i++)
    {
      kprintf("Pizza maker %d has produced its %d pizza\n", (int)which, (i+1));
      /* Try to deliver pizza */
      lock_acquire(lock_shelves);
      
      assert(nPizzasAvailable <= NSHELVES);

      while(nPizzasAvailable == NSHELVES)
	{
	  kprintf("Pizza maker %d is waiting for a shelve\n", (int)which);
	  cv_wait(cv_full, lock_shelves);
	}
      
      assert(nPizzasAvailable < NSHELVES);
      
      kprintf("Pizza maker %d has successfully delivered a pizza\n", 
	      (int)which);
      nPizzasAvailable++;
      if(nPizzasAvailable == 1)
	{
	  cv_broadcast(cv_empty, lock_shelves);
	}
      lock_release(lock_shelves);
    }
}

static void
student(void *p, unsigned long which)
{

  (void)p;
  while (1)
    {
      lock_acquire(lock_shelves);
      
      assert(nPizzasAvailable <= NSHELVES);
      
      while(nPizzasAvailable == 0)
	{
	  if(nPizzasLeft == 0)
	    {
	      kprintf("Student %d learned that there will be no more pizza\n", 
		      (int)which);
	      lock_release(lock_shelves);
	      return;
	    }
	  kprintf("Student %d is waiting for pizza\n", (int)which);
	  cv_wait(cv_empty, lock_shelves);
	}

      assert(nPizzasAvailable > 0);
      
      kprintf("Student %d has successfully gottent a pizza\n", (int)which);

      nPizzasLeft--;
      nPizzasAvailable--;
      if(nPizzasAvailable == NSHELVES - 1)
	{
	  cv_broadcast(cv_full, lock_shelves);
	}
      lock_release(lock_shelves);

      /* Eating pizza */
      clocksleep(1);
    }
}

static void
create_students(void)
{

  int i, err;

  for(i = 0; i<NSTUDENTS; i++)
    {
      err = thread_fork("Student thread", NULL, i, student, NULL);
      if(err)
	{
	  panic("create_makers(): Could not fork a thread\n");
	}
    }
}

static void
create_makers(void)
{

  int i, err;

  for(i = 0; i<NMAKERS; i++)
    {
      err = thread_fork("Pizza maker thread", NULL, i, maker, NULL);
      if(err)
	{
	  panic("create_makers(): Could not fork a thread\n");
	}
    }
}

int
pizza(int p, char **c)
{
  (void)p;
  (void)c;

  cv_full = cv_create("Full");
  assert(cv_full != NULL);

  cv_empty = cv_create("Empty");
  assert(cv_empty != NULL);

  lock_shelves = lock_create("Shelves");
  assert(lock_shelves != NULL);

  create_makers();
  create_students();
  
  return 0;
}

