#include <types.h>
#include <lib.h>
#include <test.h>
#include <thread.h>
#include <synch.h>

/* Number of shelves in MD */
#define NSHELVES	10

/* Number of pizza-makers */
#define NPIZZAMAKERS	 5  

/* Number of pizzas the union limits pizza makers to */
#define NPIZZAS		50

/* Number of students */
#define NSTUDENTS	45


/* Pizza types */
#define PIZZA_NONE	(-1)
#define PIZZA_PEPPERONI	0
#define PIZZA_SAUSAGE	1
#define PIZZA_MEATBALL	2
#define PIZZA_VEGGIE	3
#define PIZZA_NTYPES	4

static const char *const pizzatypenames[] = {
	"pepperoni",
	"sausage",
	"meatball",
	"veggie",
};

static int shelves[NSHELVES];	/* pizza buffer */
static int curshelf;		/* current position in pizza buffer */
static int neating;		/* number of students currently eating */
static int nworking;		/* number of pizzamakers currently working */
static int nproduced;		/* total number of pizzas made */

static struct lock *mainlock;	/* lock for data */
static struct cv *emptywait;	/* for waiting when shelves are empty */
static struct cv *fullwait;	/* for waiting when shelves are full */
static struct cv *donecv;	/* to wait for everyone to finish */


static
void
setup(void)
{
	curshelf = 0;
	neating = 0;
	nworking = 0;
	nproduced = 0;
	mainlock = lock_create("mainlock");
	emptywait = cv_create("emptywait");
	fullwait = cv_create("fullwait");
	donecv = cv_create("donecv");
}

static
void
cleanup(void)
{
	kprintf("+++ %d pizzas total\n", nproduced);
	assert(nworking==0 && neating==0);

	lock_destroy(mainlock);
	cv_destroy(emptywait);
	cv_destroy(fullwait);
	cv_destroy(donecv);
}

static
void
pizzamaker(void *p, unsigned long which)
{
	int i, type, imade;

	(void)p;

	kprintf("*** Pizza-maker %ld arriving\n", which);

	lock_acquire(mainlock);
	nworking++;
	lock_release(mainlock);

	imade = 0;

	for (i=0; i<NPIZZAS; i++) {
		type = random()%PIZZA_NTYPES;
		kprintf("*** Pizza-maker %ld making %s pizza\n",
			which, pizzatypenames[type]);

		clocksleep(1);

		lock_acquire(mainlock);

		assert(curshelf>=0 && curshelf <= NSHELVES);
		while (curshelf==NSHELVES) {
			cv_wait(fullwait, mainlock);
		}
		assert(curshelf>=0 && curshelf <= NSHELVES);

		kprintf("*** Pizza-maker %ld puts %s pizza on shelf %d\n",
			which, pizzatypenames[type], curshelf);

		shelves[curshelf] = type;
		curshelf++;
		imade++;

		cv_broadcast(emptywait, mainlock);
		lock_release(mainlock);
	}

	lock_acquire(mainlock);
	nproduced += imade;
	nworking--;
	/* wake up any waiting students, in case we're the last maker */
	cv_broadcast(emptywait, mainlock);
	lock_release(mainlock);

	kprintf("*** Pizza-maker %ld done (made %d pizzas)\n", which, imade);
}

static
void
student(void *p, unsigned long which)
{
	int done, avail, type, mypizzas;

	(void) p;

	kprintf("--- Student %ld arriving\n", which);

	lock_acquire(mainlock);
	neating++;
	lock_release(mainlock);

	mypizzas = 0;

	
	while (1) {
		kprintf("--- Student %ld looking for a pizza\n", which);

		lock_acquire(mainlock);

		assert(curshelf>=0 && curshelf<=NSHELVES);
		while (1) {
			done = (nproduced > 0 && nworking==0);
			avail = (curshelf > 0);
			if (done || avail) {
				break;
			}
			cv_wait(emptywait, mainlock);
		}
		assert(curshelf>=0 && curshelf<=NSHELVES);

		if (done && !avail) {
			lock_release(mainlock);
			kprintf("--- Student %ld: No more pizzas today\n",
				which);
			break;
		}

		/*
		 * These students are not polite and always take the
		 * freshest pizza.
		 */
			
		curshelf--;
		type = shelves[curshelf];
		kprintf("--- Student %ld gets %s pizza from shelf %d\n",
			which, pizzatypenames[type], curshelf);
		lock_release(mainlock);

		clocksleep(1);
		mypizzas++;

		lock_acquire(mainlock);
		cv_broadcast(fullwait, mainlock);
		lock_release(mainlock);
	}

	kprintf("--- Student %ld done (ate %d pizzas)\n", which, mypizzas);

	lock_acquire(mainlock);
	neating--;
	cv_broadcast(donecv, mainlock);
	lock_release(mainlock);
}

/*
 * Driver code to start up pizzamaker and student threads.  
 *
 * Change this function as necessary for your solution.  */
int
pizza(int nargs, char **args)
{
	int i, err;

	(void)nargs;
	(void)args;

	setup();

	for (i = 0; i < NPIZZAMAKERS; i++) {
		err = thread_fork("Pizza-maker Thread",
		    NULL, i, pizzamaker, NULL);
		if (err) {
			panic("pizza: thread_fork failed: %s)\n",
			      strerror(err));
		}
	}

	for (i = 0; i < NSTUDENTS; i++) {
		err = thread_fork("Student Thread", NULL, i, student, NULL);
		if (err) {
			panic("pizza: thread_fork failed: %s\n",
			      strerror(err));
		}
	}

	lock_acquire(mainlock);
	while (nproduced==0 || nworking>0 || neating>0) {
		cv_wait(donecv, mainlock);
	}
	lock_release(mainlock);

	cleanup();

	return 0;
}
