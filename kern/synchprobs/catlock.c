/*
 * catlock.c
 *
 * 20030131 gwa       Stub functions created for CS161 Asst1.
 * 20030215 dholland  Solution set.
 *
 * Cat/mouse problem solution using locks and condition variables.
 */


/*
 * Includes
 */

#include <types.h>
#include <lib.h>
#include <synch.h>
#include <test.h>
#include <thread.h>


/*
 * Constants
 */

/* Number of food bowls. */
#define NBOWLS 2

/* Number of cats. */
#define NCATS 6

/* Number of mice. */
#define NMICE 2

/* Number of iterations. */
#define NLOOPS 5

/* Names */
static const char *const mousenames[NMICE] = {
	"Mickey",
	"Minnie",
};

static const char *const catnames[NCATS] = {
	"Ken",
	"Midge",
	"Tick-Tock",
	"Lura",
	"Greebo",
	"Morris",
};

/*
 * Data
 */

/* Per-animal-type data */
static struct {
	volatile int hungry;		/* number of guys waiting */
	volatile int done;		/* number of guys finished */
	/*const*/ int total;		/* NCATS or NMICE */
	/*const*/ int guys_per_turn;	/* see notes below */
} info[2];

/* Indexes for info[] and values for turntype */
#define CATS 0
#define MICE 1
#define NOTYPE (-1)

static int dishbusy[NBOWLS];		/* bowl allocator */
static volatile int turntype;		/* type of animals in kitchen now */
static volatile int eaters_now;		/* number of guys currently eating */
static volatile int eaters_left_this_turn; /* change turns after this many */

static struct lock *mutex;		/* protects the above */
static struct cv *turncv;		/* wait here for the next turn */
static struct cv *donecv;		/* wait here for thread completion */


/*
 * Function Definitions
 */

/*
 * getname: return name of a cat or mouse.
 *
 * "mytype" should be either CATS or MICE, and the number should
 * be within range.
 */
static
const char *
getname(int mytype, unsigned long number)
{
	return mytype==CATS ? catnames[number] : mousenames[number];
}

/*
 * setup: initialize stuff for problem.
 */
static
void
setup(void)
{
	int i;

	/* No bowls are in use at the start. */
	for (i=0; i<NBOWLS; i++) {
		dishbusy[i]=0;
	}

	/* Nobody is either hungry or done. */
	info[CATS].hungry = info[MICE].hungry = 0;
	info[CATS].done = info[MICE].done = 0;

	/* Set the totals appropriately. */
	info[CATS].total = NCATS;
	info[MICE].total = NMICE;

	/*
	 * Choose the base number of guys allowed into the kitchen per
	 * "turn". For parallelism, this number should be at least the
	 * number of bowls available; for fairness, it probably
	 * shouldn't be greater. It should not, however, exceed the
	 * total number of animals of the type, or some/all
	 * individuals might need to eat more than once before anyone
	 * of the other type can go, which would be bad.
	 */
	info[CATS].guys_per_turn = NCATS > NBOWLS ? NBOWLS : NCATS;
	info[MICE].guys_per_turn = NCATS > NMICE ? NBOWLS : NMICE;

	/* At start, nobody is in the kitchen and no turn is established. */
	turntype = NOTYPE;
	eaters_now = 0;
	eaters_left_this_turn = 0;

	/* Create synch objects. */
	mutex = lock_create("catlock mutex");
	turncv = cv_create("catlock turn cv");
	donecv = cv_create("catlock completion cv");

	if (mutex==NULL || turncv==NULL || donecv==NULL) {
		panic("catlock: Out of memory.\n");
	}
}

/*
 * cleanup: tidy up when done.
 */
static
void
cleanup(void)
{
	int i;

	/* 
	 * First check everything came out right.
	 */

	/* No dishes should still be in use */
	for (i=0; i<NBOWLS; i++) {
		assert(dishbusy[i]==0);
	}

	/* The totals shouldn't have been clobbered */
	assert(info[CATS].total==NCATS);
	assert(info[MICE].total==NMICE);

	/* Nobody should be hungry and everyone should be done */
	for (i=0; i<2; i++) {
		assert(info[i].hungry==0);
		assert(info[i].done == info[i].total);
	}

	/* There should be nobody in the kitchen and no turn running */
	assert(turntype==NOTYPE);
	assert(eaters_now==0);
	assert(eaters_left_this_turn==0);

	/*
	 * Now clean up
	 */
	lock_destroy(mutex);
	mutex = NULL;

	cv_destroy(turncv);
	cv_destroy(donecv);
	turncv = donecv = NULL;
}

/*
 * start_turn: initialize a new turn.
 *
 * A "turn" is a period of time during which animals of one type are
 * allowed into the kitchen to eat. The number allowed in per turn may
 * depend on the animal type, for reasons described above.
 */
static
void
start_turn(void)
{
	assert(eaters_now==0);
	assert(eaters_left_this_turn==0);
	assert(turntype==CATS || turntype==MICE);
	eaters_left_this_turn = info[turntype].guys_per_turn;
}

/*
 * change_turn: pick a new turn, or none if nobody is waiting. 
 * The argument MYNAME is the name of the animal currently running.
 *
 * The turn system favors alternation of types to prevent starvation.
 */
static
void
change_turn(const char *myname)
{
	if (info[!turntype].hungry>0) {
		/* someone of the other type is hungry; let them go */
		turntype = !turntype;
		start_turn();
	}
	else if (info[turntype].hungry>0) {
		/* someone of our type is hungry; let _them_ go */
		start_turn();
	}
	else {
		/* nobody is hungry. */
		turntype = NOTYPE;
	}

	if (turntype==CATS) {
		kprintf("*** %s calls a cat turn\n", myname);
	}
	else if (turntype==MICE) {
		kprintf("*** %s calls a mouse turn\n", myname);
	}

	/* Wake up everyone waiting for the turn change */
	cv_broadcast(turncv, mutex);
}

/*
 * onepass: one animal eats once.
 * The type and number identify who it is.
 */
static
void
onepass(int mytype, unsigned long number)
{
	int i;
	int mydish;
	const char *myname = getname(mytype, number);

	lock_acquire(mutex);

	/* Report that we're hungry. */
	info[mytype].hungry++;
	kprintf("    %s feels hungry\n", myname);

	/* If there's no turn in progress, start one of our own type. */
	if (turntype==NOTYPE) {
		turntype = mytype;
		start_turn();
	}

	/* Wait until a slot opens for us. */
	while (turntype != mytype || eaters_left_this_turn==0) {
		cv_wait(turncv, mutex);
	}

	/* paranoia */
	assert(turntype==mytype);
	assert(eaters_left_this_turn>0);

	/* consume the slot */
	eaters_left_this_turn--;

	/* crosscheck - not supposed to let too many people in. */
	assert(eaters_now < NBOWLS);

	/* mark that we're eating */
	eaters_now++;

	kprintf(">>> %s enters kitchen\n", myname);

	/*
	 * Look for a bowl. There must be one unless we've screwed up.
	 */
	mydish = -1;
	for (i=0; i<NBOWLS; i++) {
		if (dishbusy[i]==0) {
			dishbusy[i] = 1;
			mydish = i;
			break;
		}
	}
	assert(mydish>=0);

	kprintf("*** %s starts eating at dish %d\n", myname, mydish);
	lock_release(mutex);

	/* Eating is a slow operation. */
	clocksleep(1);

	lock_acquire(mutex);
	kprintf("*** %s done eating at dish %d\n", myname, mydish);

	/* Release our dish. */
	assert(dishbusy[mydish]==1);
	dishbusy[mydish] = 0;

	/* Note that we're no longer eating. */
	assert(eaters_now>0);
	eaters_now--;

	/* And no longer hungry. */
	assert(info[mytype].hungry>0);
	info[mytype].hungry--;

	if (info[!mytype].hungry==0 && info[mytype].hungry>0) {
		/* 
		 * Optimization. Nobody of the other type is hungry
		 * yet, but someone of our own type is. Increase the
		 * turn length and let them eat now, taking over our
		 * slot. This obviously can't cause guys of the other
		 * type to starve.
		 */
		eaters_left_this_turn++;
		cv_signal(turncv, mutex);
	}

	if (eaters_now==0 && eaters_left_this_turn==0) {
		/*
		 * We were the last eater in the current turn, and no
		 * more are to be admitted. Time for a turn change.
		 */
		change_turn(myname);
	}

	kprintf("<<< %s leaves kitchen\n", myname);
	lock_release(mutex);
}

/*
 * commonlock: common overall code for both animal types.
 * The type and number identify the individual.
 */
static
void
commonlock(int mytype, unsigned long number)
{
	int i;
	const char *myname = getname(mytype, number);

	kprintf("... %s starting\n", myname);

	for (i=0; i<NLOOPS; i++) {
		clocksleep(random()%3+1); /* 1-3 seconds */
		onepass(mytype, number);
	}

	kprintf("... %s exiting\n", myname);

	lock_acquire(mutex);
	/* mark us done */
	info[mytype].done++;
	if (info[mytype].done == info[mytype].total) {
		/*
		 * We're last. The last turn for our type may not have
		 * had all its slots used. So end the turn forcibly if
		 * necessary.
		 */
		if (eaters_left_this_turn > 0) {
			eaters_left_this_turn = 0;
			change_turn(myname);
		}
	}
	/* Wake up the menu thread. */
	cv_signal(donecv, mutex);
	lock_release(mutex);
}

/*
 * catlock: cat code. (Just calls the common code.)
 */
static
void
catlock(void *unusedpointer, 
        unsigned long catnumber)
{
        (void) unusedpointer;
	commonlock(CATS, catnumber);
}
	

/*
 * mouselock: mouse code. (Just calls the common code.)
 */

static
void
mouselock(void *unusedpointer,
          unsigned long mousenumber)
{
        (void) unusedpointer;
	commonlock(MICE, mousenumber);
}


/*
 * catmouselock: driver code. 
 *
 * The arguments are from the menu system and are ignored.
 *
 * Creates NCATS cats and NMICE mice in random order, then waits for
 * everything to finish.
 */
int
catmouselock(int nargs, char **args)
{
        int result;
	int ncats, nmice;
   
        /* Avoid unused variable warnings. */
        (void) nargs;
        (void) args;

	setup();
   
        /*
	 * Randomized thread creation.
	 */

	ncats = nmice = 0;

	while (ncats < NCATS || nmice < NMICE) {
		int catsleft = NCATS-ncats;
		int miceleft = NMICE-nmice;
		int rand = random()%(catsleft + miceleft);

		if (rand < catsleft) {
			/* start a cat */
			result = thread_fork("catlock_thread", NULL, ncats,
					     catlock, NULL);
			ncats++;
		}
		else {
			/* start a mouse */
			result = thread_fork("mouselock thread", NULL, nmice,
					     mouselock, NULL);
			nmice++;
		}

		if (result) {
			panic("catmouselock: thread_fork failed: %s\n",
			      strerror(result));
		}
	}

	/* wait for everything to finish */
	lock_acquire(mutex);
	while (info[CATS].done < NCATS || info[MICE].done < NMICE) {
		cv_wait(donecv, mutex);
	}
	lock_release(mutex);

	cleanup();

        return 0;
}

/*
 * End of catlock.c
 */
