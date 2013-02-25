// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "arch_GOARCH.h"
#include "defs_GOOS_GOARCH.h"
#include "malloc.h"
#include "os_GOOS.h"
#include "stack.h"

bool	runtime路iscgo;

static void unwindstack(G*, byte*);
static void schedule(G*);

typedef struct Sched Sched;

M	runtime路m0;
G	runtime路g0;	// idle goroutine for m0

static	int32	debug	= 0;

int32	runtime路gcwaiting;

// Go scheduler
//
// The go scheduler's job is to match ready-to-run goroutines (`g's)
// with waiting-for-work schedulers (`m's).  If there are ready g's
// and no waiting m's, ready() will start a new m running in a new
// OS thread, so that all ready g's can run simultaneously, up to a limit.
// For now, m's never go away.
//
// By default, Go keeps only one kernel thread (m) running user code
// at a single time; other threads may be blocked in the operating system.
// Setting the environment variable $GOMAXPROCS or calling
// runtime.GOMAXPROCS() will change the number of user threads
// allowed to execute simultaneously.  $GOMAXPROCS is thus an
// approximation of the maximum number of cores to use.
//
// Even a program that can run without deadlock in a single process
// might use more m's if given the chance.  For example, the prime
// sieve will use as many m's as there are primes (up to runtime路sched.mmax),
// allowing different stages of the pipeline to execute in parallel.
// We could revisit this choice, only kicking off new m's for blocking
// system calls, but that would limit the amount of parallel computation
// that go would try to do.
//
// In general, one could imagine all sorts of refinements to the
// scheduler, but the goal now is just to get something working on
// Linux and OS X.

struct Sched {
	Lock;

	G *gfree;	// available g's (status == Gdead)
	int32 goidgen;

	G *ghead;	// g's waiting to run
	G *gtail;
	int32 gwait;	// number of g's waiting to run
	int32 gcount;	// number of g's that are alive
	int32 grunning;	// number of g's running on cpu or in syscall

	M *mhead;	// m's waiting for work
	int32 mwait;	// number of m's waiting for work
	int32 mcount;	// number of m's that have been created

	volatile uint32 atomic;	// atomic scheduling word (see below)

	int32 profilehz;	// cpu profiling rate

	bool init;  // running initialization
	bool lockmain;  // init called runtime.LockOSThread

	Note	stopped;	// one g can set waitstop and wait here for m's to stop
};

// The atomic word in sched is an atomic uint32 that
// holds these fields.
//
//	[15 bits] mcpu		number of m's executing on cpu
//	[15 bits] mcpumax	max number of m's allowed on cpu
//	[1 bit] waitstop	some g is waiting on stopped
//	[1 bit] gwaiting	gwait != 0
//
// These fields are the information needed by entersyscall
// and exitsyscall to decide whether to coordinate with the
// scheduler.  Packing them into a single machine word lets
// them use a fast path with a single atomic read/write and
// no lock/unlock.  This greatly reduces contention in
// syscall- or cgo-heavy multithreaded programs.
//
// Except for entersyscall and exitsyscall, the manipulations
// to these fields only happen while holding the schedlock,
// so the routines holding schedlock only need to worry about
// what entersyscall and exitsyscall do, not the other routines
// (which also use the schedlock).
//
// In particular, entersyscall and exitsyscall only read mcpumax,
// waitstop, and gwaiting.  They never write them.  Thus, writes to those
// fields can be done (holding schedlock) without fear of write conflicts.
// There may still be logic conflicts: for example, the set of waitstop must
// be conditioned on mcpu >= mcpumax or else the wait may be a
// spurious sleep.  The Promela model in proc.p verifies these accesses.
enum {
	mcpuWidth = 15,
	mcpuMask = (1<<mcpuWidth) - 1,
	mcpuShift = 0,
	mcpumaxShift = mcpuShift + mcpuWidth,
	waitstopShift = mcpumaxShift + mcpuWidth,
	gwaitingShift = waitstopShift+1,

	// The max value of GOMAXPROCS is constrained
	// by the max value we can store in the bit fields
	// of the atomic word.  Reserve a few high values
	// so that we can detect accidental decrement
	// beyond zero.
	maxgomaxprocs = mcpuMask - 10,
};

#define atomic_mcpu(v)		(((v)>>mcpuShift)&mcpuMask)
#define atomic_mcpumax(v)	(((v)>>mcpumaxShift)&mcpuMask)
#define atomic_waitstop(v)	(((v)>>waitstopShift)&1)
#define atomic_gwaiting(v)	(((v)>>gwaitingShift)&1)

Sched runtime路sched;
int32 runtime路gomaxprocs;
bool runtime路singleproc;

static bool canaddmcpu(void);

// An m that is waiting for notewakeup(&m->havenextg).  This may
// only be accessed while the scheduler lock is held.  This is used to
// minimize the number of times we call notewakeup while the scheduler
// lock is held, since the m will normally move quickly to lock the
// scheduler itself, producing lock contention.
static M* mwakeup;

// Scheduling helpers.  Sched must be locked.
static void gput(G*);	// put/get on ghead/gtail
static G* gget(void);
static void mput(M*);	// put/get on mhead
static M* mget(G*);
static void gfput(G*);	// put/get on gfree
static G* gfget(void);
static void matchmg(void);	// match m's to g's
static void readylocked(G*);	// ready, but sched is locked
static void mnextg(M*, G*);
static void mcommoninit(M*);

void
setmcpumax(uint32 n)
{
	uint32 v, w;

	for(;;) {
		v = runtime路sched.atomic;
		w = v;
		w &= ~(mcpuMask<<mcpumaxShift);
		w |= n<<mcpumaxShift;
		if(runtime路cas(&runtime路sched.atomic, v, w))
			break;
	}
}

// Keep trace of scavenger's goroutine for deadlock detection.
static G *scvg;

// The bootstrap sequence is:
//
//	call osinit
//	call schedinit
//	make & queue new G
//	call runtime路mstart
//
// The new G calls runtime路main.
void
runtime路schedinit(void)
{
	int32 n;
	byte *p;

	m->nomemprof++;
	runtime路mallocinit();
	mcommoninit(m);

	runtime路goargs();
	runtime路goenvs();

	// For debugging:
	// Allocate internal symbol table representation now,
	// so that we don't need to call malloc when we crash.
	// runtime路findfunc(0);

	runtime路gomaxprocs = 1;
	p = runtime路getenv("GOMAXPROCS");
	if(p != nil && (n = runtime路atoi(p)) != 0) {
		if(n > maxgomaxprocs)
			n = maxgomaxprocs;
		runtime路gomaxprocs = n;
	}
	// wait for the main goroutine to start before taking
	// GOMAXPROCS into account.
	setmcpumax(1);
	runtime路singleproc = runtime路gomaxprocs == 1;

	canaddmcpu();	// mcpu++ to account for bootstrap m
	m->helpgc = 1;	// flag to tell schedule() to mcpu--
	runtime路sched.grunning++;

	mstats.enablegc = 1;
	m->nomemprof--;
}

extern void main路init(void);
extern void main路main(void);

// The main goroutine.
void
runtime路main(void)
{
	// Lock the main goroutine onto this, the main OS thread,
	// during initialization.  Most programs won't care, but a few
	// do require certain calls to be made by the main thread.
	// Those can arrange for main.main to run in the main thread
	// by calling runtime.LockOSThread during initialization
	// to preserve the lock.
	runtime路LockOSThread();
	// From now on, newgoroutines may use non-main threads.
	setmcpumax(runtime路gomaxprocs);
	runtime路sched.init = true;
	scvg = runtime路newproc1((byte*)runtime路MHeap_Scavenger, nil, 0, 0, runtime路main);
	main路init();
	runtime路sched.init = false;
	if(!runtime路sched.lockmain)
		runtime路UnlockOSThread();

	// The deadlock detection has false negatives.
	// Let scvg start up, to eliminate the false negative
	// for the trivial program func main() { select{} }.
	runtime路gosched();

	main路main();
	runtime路exit(0);
	for(;;)
		*(int32*)runtime路main = 0;
}

// Lock the scheduler.
static void
schedlock(void)
{
	runtime路lock(&runtime路sched);
}

// Unlock the scheduler.
static void
schedunlock(void)
{
	M *m;

	m = mwakeup;
	mwakeup = nil;
	runtime路unlock(&runtime路sched);
	if(m != nil)
		runtime路notewakeup(&m->havenextg);
}

void
runtime路goexit(void)
{
	g->status = Gmoribund;
	runtime路gosched();
}

void
runtime路goroutineheader(G *g)
{
	int8 *status;

	switch(g->status) {
	case Gidle:
		status = "idle";
		break;
	case Grunnable:
		status = "runnable";
		break;
	case Grunning:
		status = "running";
		break;
	case Gsyscall:
		status = "syscall";
		break;
	case Gwaiting:
		if(g->waitreason)
			status = g->waitreason;
		else
			status = "waiting";
		break;
	case Gmoribund:
		status = "moribund";
		break;
	default:
		status = "???";
		break;
	}
	runtime路printf("goroutine %d [%s]:\n", g->goid, status);
}

void
runtime路tracebackothers(G *me)
{
	G *g;

	for(g = runtime路allg; g != nil; g = g->alllink) {
		if(g == me || g->status == Gdead)
			continue;
		runtime路printf("\n");
		runtime路goroutineheader(g);
		runtime路traceback(g->sched.pc, g->sched.sp, 0, g);
	}
}

// Mark this g as m's idle goroutine.
// This functionality might be used in environments where programs
// are limited to a single thread, to simulate a select-driven
// network server.  It is not exposed via the standard runtime API.
void
runtime路idlegoroutine(void)
{
	if(g->idlem != nil)
		runtime路throw("g is already an idle goroutine");
	g->idlem = m;
}

static void
mcommoninit(M *m)
{
	m->id = runtime路sched.mcount++;
	m->fastrand = 0x49f6428aUL + m->id + runtime路cputicks();
	m->stackalloc = runtime路malloc(sizeof(*m->stackalloc));
	runtime路FixAlloc_Init(m->stackalloc, FixedStack, runtime路SysAlloc, nil, nil);

	if(m->mcache == nil)
		m->mcache = runtime路allocmcache();

	runtime路callers(1, m->createstack, nelem(m->createstack));

	// Add to runtime路allm so garbage collector doesn't free m
	// when it is just in a register or thread-local storage.
	m->alllink = runtime路allm;
	// runtime路NumCgoCall() iterates over allm w/o schedlock,
	// so we need to publish it safely.
	runtime路atomicstorep(&runtime路allm, m);
}

// Try to increment mcpu.  Report whether succeeded.
static bool
canaddmcpu(void)
{
	uint32 v;

	for(;;) {
		v = runtime路sched.atomic;
		if(atomic_mcpu(v) >= atomic_mcpumax(v))
			return 0;
		if(runtime路cas(&runtime路sched.atomic, v, v+(1<<mcpuShift)))
			return 1;
	}
}

// Put on `g' queue.  Sched must be locked.
static void
gput(G *g)
{
	M *m;

	// If g is wired, hand it off directly.
	if((m = g->lockedm) != nil && canaddmcpu()) {
		mnextg(m, g);
		return;
	}

	// If g is the idle goroutine for an m, hand it off.
	if(g->idlem != nil) {
		if(g->idlem->idleg != nil) {
			runtime路printf("m%d idle out of sync: g%d g%d\n",
				g->idlem->id,
				g->idlem->idleg->goid, g->goid);
			runtime路throw("runtime: double idle");
		}
		g->idlem->idleg = g;
		return;
	}

	g->schedlink = nil;
	if(runtime路sched.ghead == nil)
		runtime路sched.ghead = g;
	else
		runtime路sched.gtail->schedlink = g;
	runtime路sched.gtail = g;

	// increment gwait.
	// if it transitions to nonzero, set atomic gwaiting bit.
	if(runtime路sched.gwait++ == 0)
		runtime路xadd(&runtime路sched.atomic, 1<<gwaitingShift);
}

// Report whether gget would return something.
static bool
haveg(void)
{
	return runtime路sched.ghead != nil || m->idleg != nil;
}

// Get from `g' queue.  Sched must be locked.
static G*
gget(void)
{
	G *g;

	g = runtime路sched.ghead;
	if(g){
		runtime路sched.ghead = g->schedlink;
		if(runtime路sched.ghead == nil)
			runtime路sched.gtail = nil;
		// decrement gwait.
		// if it transitions to zero, clear atomic gwaiting bit.
		if(--runtime路sched.gwait == 0)
			runtime路xadd(&runtime路sched.atomic, -1<<gwaitingShift);
	} else if(m->idleg != nil) {
		g = m->idleg;
		m->idleg = nil;
	}
	return g;
}

// Put on `m' list.  Sched must be locked.
static void
mput(M *m)
{
	m->schedlink = runtime路sched.mhead;
	runtime路sched.mhead = m;
	runtime路sched.mwait++;
}

// Get an `m' to run `g'.  Sched must be locked.
static M*
mget(G *g)
{
	M *m;

	// if g has its own m, use it.
	if(g && (m = g->lockedm) != nil)
		return m;

	// otherwise use general m pool.
	if((m = runtime路sched.mhead) != nil){
		runtime路sched.mhead = m->schedlink;
		runtime路sched.mwait--;
	}
	return m;
}

// Mark g ready to run.
void
runtime路ready(G *g)
{
	schedlock();
	readylocked(g);
	schedunlock();
}

// Mark g ready to run.  Sched is already locked.
// G might be running already and about to stop.
// The sched lock protects g->status from changing underfoot.
/* 从函数名就可以看出,它干的事情是,把g变成Grunnable的,挂在就绪队列
   要求调用函数前调度器是锁着的,所以叫readylocked
 */
static void
readylocked(G *g)
{
	if(g->m){
		// Running on another machine.
		// Ready it when it stops.
		g->readyonstop = 1;
		return;
	}

	// Mark runnable.
	if(g->status == Grunnable || g->status == Grunning) {
		runtime路printf("goroutine %d has status %d\n", g->goid, g->status);
		runtime路throw("bad g->status in ready");
	}
	g->status = Grunnable;

	gput(g);
	matchmg();
}

static void
nop(void)
{
}

// Same as readylocked but a different symbol so that
// debuggers can set a breakpoint here and catch all
// new goroutines.
static void
newprocreadylocked(G *g)
{
	nop();	// avoid inlining in 6l
	readylocked(g);
}

// Pass g to m for running.
// Caller has already incremented mcpu.
static void
mnextg(M *m, G *g)
{
	runtime路sched.grunning++;
	m->nextg = g;
	if(m->waitnextg) {
		m->waitnextg = 0;
		if(mwakeup != nil)
			runtime路notewakeup(&mwakeup->havenextg);
		mwakeup = m;
	}
}

// Get the next goroutine that m should run.
// Sched must be locked on entry, is unlocked on exit.
// Makes sure that at most $GOMAXPROCS g's are
// running on cpus (not in system calls) at any given time.
/* 最多同时只有GOMAXPROCS个g正在运行,处于系统调用的不算
 */
static G*
nextgandunlock(void)
{
	G *gp;
	uint32 v;

top:
	if(atomic_mcpu(runtime路sched.atomic) >= maxgomaxprocs)
		runtime路throw("negative mcpu");

	// If there is a g waiting as m->nextg, the mcpu++
	// happened before it was passed to mnextg.
	if(m->nextg != nil) {
		gp = m->nextg;
		m->nextg = nil;
		schedunlock();
		return gp;
	}

	if(m->lockedg != nil) {
		// We can only run one g, and it's not available.
		// Make sure some other cpu is running to handle
		// the ordinary run queue.
		if(runtime路sched.gwait != 0) {
			matchmg();
			// m->lockedg might have been on the queue.
			if(m->nextg != nil) {
				gp = m->nextg;
				m->nextg = nil;
				schedunlock();
				return gp;
			}
		}
	} else {
		// Look for work on global queue.
		while(haveg() && canaddmcpu()) {
			gp = gget();
			if(gp == nil)
				runtime路throw("gget inconsistency");

			if(gp->lockedm) {
				mnextg(gp->lockedm, gp);
				continue;
			}
			runtime路sched.grunning++;
			schedunlock();
			return gp;
		}

		// The while loop ended either because the g queue is empty
		// or because we have maxed out our m procs running go
		// code (mcpu >= mcpumax).  We need to check that
		// concurrent actions by entersyscall/exitsyscall cannot
		// invalidate the decision to end the loop.
		//
		// We hold the sched lock, so no one else is manipulating the
		// g queue or changing mcpumax.  Entersyscall can decrement
		// mcpu, but if does so when there is something on the g queue,
		// the gwait bit will be set, so entersyscall will take the slow path
		// and use the sched lock.  So it cannot invalidate our decision.
		//
		// Wait on global m queue.
		mput(m);
	}

	// Look for deadlock situation.
	// There is a race with the scavenger that causes false negatives:
	// if the scavenger is just starting, then we have
	//	scvg != nil && grunning == 0 && gwait == 0
	// and we do not detect a deadlock.  It is possible that we should
	// add that case to the if statement here, but it is too close to Go 1
	// to make such a subtle change.  Instead, we work around the
	// false negative in trivial programs by calling runtime.gosched
	// from the main goroutine just before main.main.
	// See runtime路main above.
	//
	// On a related note, it is also possible that the scvg == nil case is
	// wrong and should include gwait, but that does not happen in
	// standard Go programs, which all start the scavenger.
	//
	if((scvg == nil && runtime路sched.grunning == 0) ||
	   (scvg != nil && runtime路sched.grunning == 1 && runtime路sched.gwait == 0 &&
	    (scvg->status == Grunning || scvg->status == Gsyscall))) {
		runtime路throw("all goroutines are asleep - deadlock!");
	}

	m->nextg = nil;
	m->waitnextg = 1;
	runtime路noteclear(&m->havenextg);

	// Stoptheworld is waiting for all but its cpu to go to stop.
	// Entersyscall might have decremented mcpu too, but if so
	// it will see the waitstop and take the slow path.
	// Exitsyscall never increments mcpu beyond mcpumax.
	v = runtime路atomicload(&runtime路sched.atomic);
	if(atomic_waitstop(v) && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		// set waitstop = 0 (known to be 1)
		runtime路xadd(&runtime路sched.atomic, -1<<waitstopShift);
		runtime路notewakeup(&runtime路sched.stopped);
	}
	schedunlock();

	runtime路notesleep(&m->havenextg);
	if(m->helpgc) {
		runtime路gchelper();
		m->helpgc = 0;
		runtime路lock(&runtime路sched);
		goto top;
	}
	if((gp = m->nextg) == nil)
		runtime路throw("bad m->nextg in nextgoroutine");
	m->nextg = nil;
	return gp;
}

int32
runtime路helpgc(bool *extra)
{
	M *mp;
	int32 n, max;

	// Figure out how many CPUs to use.
	// Limited by gomaxprocs, number of actual CPUs, and MaxGcproc.
	max = runtime路gomaxprocs;
	if(max > runtime路ncpu)
		max = runtime路ncpu;
	if(max > MaxGcproc)
		max = MaxGcproc;

	// We're going to use one CPU no matter what.
	// Figure out the max number of additional CPUs.
	max--;

	runtime路lock(&runtime路sched);
	n = 0;
	while(n < max && (mp = mget(nil)) != nil) {
		n++;
		mp->helpgc = 1;
		mp->waitnextg = 0;
		runtime路notewakeup(&mp->havenextg);
	}
	runtime路unlock(&runtime路sched);
	if(extra)
		*extra = n != max;
	return n;
}

void
runtime路stoptheworld(void)
{
	uint32 v;

	schedlock();
	runtime路gcwaiting = 1;

	setmcpumax(1);

	// while mcpu > 1
	for(;;) {
		v = runtime路sched.atomic;
		if(atomic_mcpu(v) <= 1)
			break;

		// It would be unsafe for multiple threads to be using
		// the stopped note at once, but there is only
		// ever one thread doing garbage collection.
		runtime路noteclear(&runtime路sched.stopped);
		if(atomic_waitstop(v))
			runtime路throw("invalid waitstop");

		// atomic { waitstop = 1 }, predicated on mcpu <= 1 check above
		// still being true.
		if(!runtime路cas(&runtime路sched.atomic, v, v+(1<<waitstopShift)))
			continue;

		schedunlock();
		runtime路notesleep(&runtime路sched.stopped);
		schedlock();
	}
	runtime路singleproc = runtime路gomaxprocs == 1;
	schedunlock();
}

void
runtime路starttheworld(bool extra)
{
	M *m;

	schedlock();
	runtime路gcwaiting = 0;
	setmcpumax(runtime路gomaxprocs);
	matchmg();
	if(extra && canaddmcpu()) {
		// Start a new m that will (we hope) be idle
		// and so available to help when the next
		// garbage collection happens.
		// canaddmcpu above did mcpu++
		// (necessary, because m will be doing various
		// initialization work so is definitely running),
		// but m is not running a specific goroutine,
		// so set the helpgc flag as a signal to m's
		// first schedule(nil) to mcpu-- and grunning--.
		m = runtime路newm();
		m->helpgc = 1;
		runtime路sched.grunning++;
	}
	schedunlock();
}

// Called to start an M.
void
runtime路mstart(void)
{
	/* mstart是runtime.newosproc新建的线程的入口地址
	   新线程执行时会从这里开始运行
	   注意在runtime.newosproc中传过来时,参数就是g0和g0的栈
	   并不是新建goroutine的g!
	 */
	if(g != m->g0)
		runtime路throw("bad runtime路mstart");

	// Record top of stack for use by mcall.
	// Once we call schedule we're never coming back,
	// so other calls can reuse this stack space.
	/* 记录下栈顶供mcall使用.
	   一旦调用了schedule函数,schedule不会继续使用这个栈(schedule不是普通的栈调用方式,被调者继续使用调用者栈的下面的地址空间)
	   因此其它调用可以重用这个栈地址空间
	 */
	runtime路gosave(&m->g0->sched);
	m->g0->sched.pc = (void*)-1;  // make sure it is never used
	runtime路asminit();
	runtime路minit(); /* 为信号处理建立了专门的G结构体 */

	// Install signal handlers; after minit so that minit can
	// prepare the thread to be able to handle the signals.
	if(m == &runtime路m0)
		runtime路initsig();

	schedule(nil); /* 不会返回 */
}

// When running with cgo, we call libcgo_thread_start
// to start threads for us so that we can play nicely with
// foreign code.
void (*libcgo_thread_start)(void*);

typedef struct CgoThreadStart CgoThreadStart;
struct CgoThreadStart
{
	M *m;
	G *g;
	void (*fn)(void);
};

// Kick off new m's as needed (up to mcpumax).
// Sched is locked.
/* 这个函数就是做个匹配,只要m没有突破上限,就绪队列中还有g,就用一个m运行一个g */
static void
matchmg(void)
{
	G *gp;
	M *mp;

	if(m->mallocing || m->gcing)
		return;
	/* 只要就绪队列中有g,并且可运行的m数没有到上限 */
	while(haveg() && canaddmcpu()) {
		gp = gget();
		if(gp == nil)
			runtime路throw("gget inconsistency");

		// Find the m that will run gp.
		if((mp = mget(gp)) == nil)  //m的waiting队列中有就直接拿,没有就新建
			mp = runtime路newm();
		mnextg(mp, gp);
	}
}

// Create a new m.  It will start off with a call to runtime路mstart.
/* 其实就是新建一个操作系统线程,线程的入口点是mstart
   调度器会将这个machine和某个goroutine绑定(matchmg)
   mstart会回调对应的goroutine的上下文
 */
M*
runtime路newm(void)
{
	M *m;

	m = runtime路malloc(sizeof(M));
	mcommoninit(m);  //做一些初始化工作

        /* cgo是不能在分段栈运行,要切换到系统栈中,所以代码处理有些不同
	   这部分先跳过不看
	 */
	if(runtime路iscgo) {  
		CgoThreadStart ts;

		if(libcgo_thread_start == nil)
			runtime路throw("libcgo_thread_start missing");
		// pthread_create will make us a stack.
		m->g0 = runtime路malg(-1);
		ts.m = m;
		ts.g = m->g0;
		ts.fn = runtime路mstart;
		runtime路asmcgocall(libcgo_thread_start, &ts);
	} else {
		if(Windows)
			// windows will layout sched stack on os stack
			m->g0 = runtime路malg(-1);
		else
			m->g0 = runtime路malg(8192);
		runtime路newosproc(m, m->g0, m->g0->stackbase, runtime路mstart);
	}

	return m;
}

// One round of scheduler: find a goroutine and run it.
// The argument is the goroutine that was running before
// schedule was called, or nil if this is the first call.
// Never returns.
static void
schedule(G *gp)
{
	int32 hz;
	uint32 v;

	schedlock();
	if(gp != nil) {
		// Just finished running gp.
		gp->m = nil;
		runtime路sched.grunning--;

		// atomic { mcpu-- }
		v = runtime路xadd(&runtime路sched.atomic, -1<<mcpuShift);
		if(atomic_mcpu(v) > maxgomaxprocs)
			runtime路throw("negative mcpu in scheduler");

		switch(gp->status){
		case Grunnable:
		case Gdead:
			// Shouldn't have been running!
			runtime路throw("bad gp->status in sched");
		case Grunning:
			gp->status = Grunnable;
			gput(gp);
			break;
		case Gmoribund:
			gp->status = Gdead;
			if(gp->lockedm) {
				gp->lockedm = nil;
				m->lockedg = nil;
			}
			gp->idlem = nil;
			unwindstack(gp, nil);
			gfput(gp);
			if(--runtime路sched.gcount == 0)
				runtime路exit(0);
			break;
		}
		if(gp->readyonstop){
			gp->readyonstop = 0;
			readylocked(gp);
		}
	} else if(m->helpgc) {
		// Bootstrap m or new m started by starttheworld.
		// atomic { mcpu-- }
		v = runtime路xadd(&runtime路sched.atomic, -1<<mcpuShift);
		if(atomic_mcpu(v) > maxgomaxprocs)
			runtime路throw("negative mcpu in scheduler");
		// Compensate for increment in starttheworld().
		runtime路sched.grunning--;
		m->helpgc = 0;
	} else if(m->nextg != nil) {
		// New m started by matchmg.
	} else {
		runtime路throw("invalid m state in scheduler");
	}

	// Find (or wait for) g to run.  Unlocks runtime路sched.
	gp = nextgandunlock();
	gp->readyonstop = 0;
	gp->status = Grunning;
	m->curg = gp;
	gp->m = m;

	// Check whether the profiler needs to be turned on or off.
	hz = runtime路sched.profilehz;
	if(m->profilehz != hz)
		runtime路resetcpuprofiler(hz);

	if(gp->sched.pc == (byte*)runtime路goexit) {	// kickoff
		runtime路gogocall(&gp->sched, (void(*)(void))gp->entry);
	}
	runtime路gogo(&gp->sched, 0);
}

// Enter scheduler.  If g->status is Grunning,
// re-queues g and runs everyone else who is waiting
// before running g again.  If g->status is Gmoribund,
// kills off g.
// Cannot split stack because it is called from exitsyscall.
// See comment below.
#pragma textflag 7
void
runtime路gosched(void)
{
	if(m->locks != 0)
		runtime路throw("gosched holding locks");
	if(g == m->g0)
		runtime路throw("gosched of g0");
	runtime路mcall(schedule);
}

// The goroutine g is about to enter a system call.
// Record that it's not using the cpu anymore.
// This is called only from the go syscall library and cgocall,
// not from the low-level system calls used by the runtime.
//
// Entersyscall cannot split the stack: the runtime路gosave must
// make g->sched refer to the caller's stack segment, because
// entersyscall is going to return immediately after.
// It's okay to call matchmg and notewakeup even after
// decrementing mcpu, because we haven't released the
// sched lock yet, so the garbage collector cannot be running.
#pragma textflag 7
void
runtime路entersyscall(void)
{
	uint32 v;

	if(m->profilehz > 0)
		runtime路setprof(false);

	// Leave SP around for gc and traceback.
	runtime路gosave(&g->sched);
	g->gcsp = g->sched.sp;
	g->gcstack = g->stackbase;
	g->gcguard = g->stackguard;
	g->status = Gsyscall;
	if(g->gcsp < g->gcguard-StackGuard || g->gcstack < g->gcsp) {
		// runtime路printf("entersyscall inconsistent %p [%p,%p]\n",
		//	g->gcsp, g->gcguard-StackGuard, g->gcstack);
		runtime路throw("entersyscall");
	}

	// Fast path.
	// The slow path inside the schedlock/schedunlock will get
	// through without stopping if it does:
	//	mcpu--
	//	gwait not true
	//	waitstop && mcpu <= mcpumax not true
	// If we can do the same with a single atomic add,
	// then we can skip the locks.
	v = runtime路xadd(&runtime路sched.atomic, -1<<mcpuShift);
	if(!atomic_gwaiting(v) && (!atomic_waitstop(v) || atomic_mcpu(v) > atomic_mcpumax(v)))
		return;

	schedlock();
	v = runtime路atomicload(&runtime路sched.atomic);
	if(atomic_gwaiting(v)) {
		matchmg();
		v = runtime路atomicload(&runtime路sched.atomic);
	}
	if(atomic_waitstop(v) && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		runtime路xadd(&runtime路sched.atomic, -1<<waitstopShift);
		runtime路notewakeup(&runtime路sched.stopped);
	}

	// Re-save sched in case one of the calls
	// (notewakeup, matchmg) triggered something using it.
	runtime路gosave(&g->sched);

	schedunlock();
}

// The goroutine g exited its system call.
// Arrange for it to run on a cpu again.
// This is called only from the go syscall library, not
// from the low-level system calls used by the runtime.
void
runtime路exitsyscall(void)
{
	uint32 v;

	// Fast path.
	// If we can do the mcpu++ bookkeeping and
	// find that we still have mcpu <= mcpumax, then we can
	// start executing Go code immediately, without having to
	// schedlock/schedunlock.
	v = runtime路xadd(&runtime路sched.atomic, (1<<mcpuShift));
	if(m->profilehz == runtime路sched.profilehz && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		// There's a cpu for us, so we can run.
		g->status = Grunning;
		// Garbage collector isn't running (since we are),
		// so okay to clear gcstack.
		g->gcstack = nil;

		if(m->profilehz > 0)
			runtime路setprof(true);
		return;
	}

	// Tell scheduler to put g back on the run queue:
	// mostly equivalent to g->status = Grunning,
	// but keeps the garbage collector from thinking
	// that g is running right now, which it's not.
	g->readyonstop = 1;

	// All the cpus are taken.
	// The scheduler will ready g and put this m to sleep.
	// When the scheduler takes g away from m,
	// it will undo the runtime路sched.mcpu++ above.
	runtime路gosched();

	// Gosched returned, so we're allowed to run now.
	// Delete the gcstack information that we left for
	// the garbage collector during the system call.
	// Must wait until now because until gosched returns
	// we don't know for sure that the garbage collector
	// is not running.
	g->gcstack = nil;
}

// Called from runtime路lessstack when returning from a function which
// allocated a new stack segment.  The function's return value is in
// m->cret.
void
runtime路oldstack(void)
{
	Stktop *top, old;
	uint32 argsize;
	uintptr cret;
	byte *sp;
	G *g1;
	int32 goid;

//printf("oldstack m->cret=%p\n", m->cret);

	g1 = m->curg;
	top = (Stktop*)g1->stackbase;
	sp = (byte*)top;
	old = *top;
	argsize = old.argsize;
	if(argsize > 0) {
		sp -= argsize;
		runtime路memmove(top->argp, sp, argsize);
	}
	goid = old.gobuf.g->goid;	// fault if g is bad, before gogo
	USED(goid);

	if(old.free != 0)
		runtime路stackfree(g1->stackguard - StackGuard, old.free);
	g1->stackbase = old.stackbase;
	g1->stackguard = old.stackguard;

	cret = m->cret;
	m->cret = 0;  // drop reference
	runtime路gogo(&old.gobuf, cret);
}

// Called from reflect路call or from runtime路morestack when a new
// stack segment is needed.  Allocate a new stack big enough for
// m->moreframesize bytes, copy m->moreargsize bytes to the new frame,
// and then act as though runtime路lessstack called the function at
// m->morepc.
void
runtime路newstack(void)
{
	int32 framesize, argsize;
	Stktop *top;
	byte *stk, *sp;
	G *g1;
	Gobuf label;
	bool reflectcall;
	uintptr free;

	framesize = m->moreframesize;
	argsize = m->moreargsize;
	g1 = m->curg;

	if(m->morebuf.sp < g1->stackguard - StackGuard) {
		runtime路printf("runtime: split stack overflow: %p < %p\n", m->morebuf.sp, g1->stackguard - StackGuard);
		runtime路throw("runtime: split stack overflow");
	}
	if(argsize % sizeof(uintptr) != 0) {
		runtime路printf("runtime: stack split with misaligned argsize %d\n", argsize);
		runtime路throw("runtime: stack split argsize");
	}

	reflectcall = framesize==1;
	if(reflectcall)
		framesize = 0;

	if(reflectcall && m->morebuf.sp - sizeof(Stktop) - argsize - 32 > g1->stackguard) {
		// special case: called from reflect.call (framesize==1)
		// to call code with an arbitrary argument size,
		// and we have enough space on the current stack.
		// the new Stktop* is necessary to unwind, but
		// we don't need to create a new segment.
		top = (Stktop*)(m->morebuf.sp - sizeof(*top));
		stk = g1->stackguard - StackGuard;
		free = 0;
	} else {
		// allocate new segment.
		framesize += argsize;
		framesize += StackExtra;	// room for more functions, Stktop.
		if(framesize < StackMin)
			framesize = StackMin;
		framesize += StackSystem;
		stk = runtime路stackalloc(framesize);
		top = (Stktop*)(stk+framesize-sizeof(*top));
		free = framesize;
	}

//runtime路printf("newstack framesize=%d argsize=%d morepc=%p moreargp=%p gobuf=%p, %p top=%p old=%p\n",
//framesize, argsize, m->morepc, m->moreargp, m->morebuf.pc, m->morebuf.sp, top, g1->stackbase);

	top->stackbase = g1->stackbase;
	top->stackguard = g1->stackguard;
	top->gobuf = m->morebuf;
	top->argp = m->moreargp;
	top->argsize = argsize;
	top->free = free;
	m->moreargp = nil;
	m->morebuf.pc = nil;
	m->morebuf.sp = nil;

	// copy flag from panic
	top->panic = g1->ispanic;
	g1->ispanic = false;

	g1->stackbase = (byte*)top;
	g1->stackguard = stk + StackGuard;

	sp = (byte*)top;
	if(argsize > 0) {
		sp -= argsize;
		runtime路memmove(sp, top->argp, argsize);
	}
	if(thechar == '5') {
		// caller would have saved its LR below args.
		sp -= sizeof(void*);
		*(void**)sp = nil;
	}

	// Continue as if lessstack had just called m->morepc
	// (the PC that decided to grow the stack).
	label.sp = sp;
	label.pc = (byte*)runtime路lessstack;
	label.g = m->curg;
	runtime路gogocall(&label, m->morepc);

	*(int32*)345 = 123;	// never return
}

// Hook used by runtime路malg to call runtime路stackalloc on the
// scheduler stack.  This exists because runtime路stackalloc insists
// on being called on the scheduler stack, to avoid trying to grow
// the stack while allocating a new stack segment.
static void
mstackalloc(G *gp)
{
	gp->param = runtime路stackalloc((uintptr)gp->param);
	runtime路gogo(&gp->sched, 0);
}

// Allocate a new g, with a stack big enough for stacksize bytes.
G*
runtime路malg(int32 stacksize)
{
	G *newg;
	byte *stk;

	if(StackTop < sizeof(Stktop)) {
		runtime路printf("runtime: SizeofStktop=%d, should be >=%d\n", (int32)StackTop, (int32)sizeof(Stktop));
		runtime路throw("runtime: bad stack.h");
	}

	newg = runtime路malloc(sizeof(G));
	if(stacksize >= 0) {
		if(g == m->g0) {
			// running on scheduler stack already.
			stk = runtime路stackalloc(StackSystem + stacksize);
		} else {
			// have to call stackalloc on scheduler stack.
			g->param = (void*)(StackSystem + stacksize);
			runtime路mcall(mstackalloc);
			stk = g->param;
			g->param = nil;
		}
		newg->stack0 = stk;
		newg->stackguard = stk + StackGuard;
		newg->stackbase = stk + StackSystem + stacksize - sizeof(Stktop);
		runtime路memclr(newg->stackbase, sizeof(Stktop));
	}
	return newg;
}

// Create a new g running fn with siz bytes of arguments.
// Put it on the queue of g's waiting to run.
// The compiler turns a go statement into a call to this.
// Cannot split the stack because it assumes that the arguments
// are available sequentially after &fn; they would not be
// copied if a stack split occurred.  It's OK for this to call
// functions that split the stack.
/* 函数功能:创建一个新的g
   这个函数不能用分段栈,真正的工作是调用newproc1完成的
   newproc1的动作包括
   分配一个g的结构体
   初始化这个结构体的一些域
   将它挂在就绪队列
   引发一次调度matchmg
 */
#pragma textflag 7
void
runtime路newproc(int32 siz, byte* fn, ...)
{
	byte *argp;

	if(thechar == '5')
		argp = (byte*)(&fn+2);  // skip caller's saved LR
	else
		argp = (byte*)(&fn+1);
	runtime路newproc1(fn, argp, siz, 0, runtime路getcallerpc(&siz));
}

// Create a new g running fn with narg bytes of arguments starting
// at argp and returning nret bytes of results.  callerpc is the
// address of the go statement that created this.  The new g is put
// on the queue of g's waiting to run.
G*
runtime路newproc1(byte *fn, byte *argp, int32 narg, int32 nret, void *callerpc)
{
	byte *sp;
	G *newg;
	int32 siz;

//printf("newproc1 %p %p narg=%d nret=%d\n", fn, argp, narg, nret);
	siz = narg + nret;
	siz = (siz+7) & ~7; //8字节对齐

	// We could instead create a secondary stack frame
	// and make it look like goexit was on the original but
	// the call to the actual goroutine function was split.
	// Not worth it: this is almost always an error.
	if(siz > StackMin - 1024)//跳过不看
		runtime路throw("runtime.newproc: function arguments too large for new goroutine");

	schedlock();

	/* 下面这一段做的事情是分配一个g的空间
	   如果gfree队列中有就直接取,没有
	   就再调用malg函数分配一个新的g,并挂在allg队列尾
	   然后设置g的状态为Gwaiting
	 */
	if((newg = gfget()) != nil){
		if(newg->stackguard - StackGuard != newg->stack0)
			runtime路throw("invalid stack in newg");
	} else {
		newg = runtime路malg(StackMin);
		if(runtime路lastg == nil)
			runtime路allg = newg;
		else
			runtime路lastg->alllink = newg;
		runtime路lastg = newg;
	}
	newg->status = Gwaiting;
	newg->waitreason = "new goroutine";

	/* 下面这一段做的事情是做一些初始化g的工作
	   包括把调用参数移到g的栈中
	   把g的sp,pc等进程上下文保存在g的sched中
	   设置g的gid域,增加g个数的的全局计数
	 */
	sp = newg->stackbase;
	sp -= siz;
	runtime路memmove(sp, argp, narg);
	if(thechar == '5') {
		// caller's LR
		sp -= sizeof(void*);
		*(void**)sp = nil;
	}

	newg->sched.sp = sp;
	newg->sched.pc = (byte*)runtime路goexit;
	newg->sched.g = newg;
	newg->entry = fn;
	newg->gopc = (uintptr)callerpc;

	runtime路sched.gcount++;
	runtime路sched.goidgen++;
	newg->goid = runtime路sched.goidgen;

	/* 这个函数将把newg状态设置成Grunnable,并引起一将调度matchmg */
	newprocreadylocked(newg);
	schedunlock();

	return newg;
//printf(" goid=%d\n", newg->goid);
}

// Create a new deferred function fn with siz bytes of arguments.
// The compiler turns a defer statement into a call to this.
// Cannot split the stack because it assumes that the arguments
// are available sequentially after &fn; they would not be
// copied if a stack split occurred.  It's OK for this to call
// functions that split the stack.
#pragma textflag 7
uintptr
runtime路deferproc(int32 siz, byte* fn, ...)
{
	Defer *d;

	d = runtime路malloc(sizeof(*d) + siz - sizeof(d->args));
	d->fn = fn;
	d->siz = siz;
	d->pc = runtime路getcallerpc(&siz);
	if(thechar == '5')
		d->argp = (byte*)(&fn+2);  // skip caller's saved link register
	else
		d->argp = (byte*)(&fn+1);
	runtime路memmove(d->args, d->argp, d->siz);

	d->link = g->defer;
	g->defer = d;

	// deferproc returns 0 normally.
	// a deferred func that stops a panic
	// makes the deferproc return 1.
	// the code the compiler generates always
	// checks the return value and jumps to the
	// end of the function if deferproc returns != 0.
	return 0;
}

// Run a deferred function if there is one.
// The compiler inserts a call to this at the end of any
// function which calls defer.
// If there is a deferred function, this will call runtime路jmpdefer,
// which will jump to the deferred function such that it appears
// to have been called by the caller of deferreturn at the point
// just before deferreturn was called.  The effect is that deferreturn
// is called again and again until there are no more deferred functions.
// Cannot split the stack because we reuse the caller's frame to
// call the deferred function.
#pragma textflag 7
void
runtime路deferreturn(uintptr arg0)
{
	Defer *d;
	byte *argp, *fn;

	d = g->defer;
	if(d == nil)
		return;
	argp = (byte*)&arg0;
	if(d->argp != argp)
		return;
	runtime路memmove(argp, d->args, d->siz);
	g->defer = d->link;
	fn = d->fn;
	if(!d->nofree)
		runtime路free(d);
	runtime路jmpdefer(fn, argp);
}

// Run all deferred functions for the current goroutine.
static void
rundefer(void)
{
	Defer *d;

	while((d = g->defer) != nil) {
		g->defer = d->link;
		reflect路call(d->fn, d->args, d->siz);
		if(!d->nofree)
			runtime路free(d);
	}
}

// Free stack frames until we hit the last one
// or until we find the one that contains the argp.
static void
unwindstack(G *gp, byte *sp)
{
	Stktop *top;
	byte *stk;

	// Must be called from a different goroutine, usually m->g0.
	if(g == gp)
		runtime路throw("unwindstack on self");

	while((top = (Stktop*)gp->stackbase) != nil && top->stackbase != nil) {
		stk = gp->stackguard - StackGuard;
		if(stk <= sp && sp < gp->stackbase)
			break;
		gp->stackbase = top->stackbase;
		gp->stackguard = top->stackguard;
		if(top->free != 0)
			runtime路stackfree(stk, top->free);
	}

	if(sp != nil && (sp < gp->stackguard - StackGuard || gp->stackbase < sp)) {
		runtime路printf("recover: %p not in [%p, %p]\n", sp, gp->stackguard - StackGuard, gp->stackbase);
		runtime路throw("bad unwindstack");
	}
}

// Print all currently active panics.  Used when crashing.
static void
printpanics(Panic *p)
{
	if(p->link) {
		printpanics(p->link);
		runtime路printf("\t");
	}
	runtime路printf("panic: ");
	runtime路printany(p->arg);
	if(p->recovered)
		runtime路printf(" [recovered]");
	runtime路printf("\n");
}

static void recovery(G*);

// The implementation of the predeclared function panic.
void
runtime路panic(Eface e)
{
	Defer *d;
	Panic *p;

	p = runtime路mal(sizeof *p);
	p->arg = e;
	p->link = g->panic;
	p->stackbase = g->stackbase;
	g->panic = p;

	for(;;) {
		d = g->defer;
		if(d == nil)
			break;
		// take defer off list in case of recursive panic
		g->defer = d->link;
		g->ispanic = true;	// rock for newstack, where reflect.call ends up
		reflect路call(d->fn, d->args, d->siz);
		if(p->recovered) {
			g->panic = p->link;
			if(g->panic == nil)	// must be done with signal
				g->sig = 0;
			runtime路free(p);
			// put recovering defer back on list
			// for scheduler to find.
			d->link = g->defer;
			g->defer = d;
			runtime路mcall(recovery);
			runtime路throw("recovery failed"); // mcall should not return
		}
		if(!d->nofree)
			runtime路free(d);
	}

	// ran out of deferred calls - old-school panic now
	runtime路startpanic();
	printpanics(g->panic);
	runtime路dopanic(0);
}

// Unwind the stack after a deferred function calls recover
// after a panic.  Then arrange to continue running as though
// the caller of the deferred function returned normally.
static void
recovery(G *gp)
{
	Defer *d;

	// Rewind gp's stack; we're running on m->g0's stack.
	d = gp->defer;
	gp->defer = d->link;

	// Unwind to the stack frame with d's arguments in it.
	unwindstack(gp, d->argp);

	// Make the deferproc for this d return again,
	// this time returning 1.  The calling function will
	// jump to the standard return epilogue.
	// The -2*sizeof(uintptr) makes up for the
	// two extra words that are on the stack at
	// each call to deferproc.
	// (The pc we're returning to does pop pop
	// before it tests the return value.)
	// On the arm there are 2 saved LRs mixed in too.
	if(thechar == '5')
		gp->sched.sp = (byte*)d->argp - 4*sizeof(uintptr);
	else
		gp->sched.sp = (byte*)d->argp - 2*sizeof(uintptr);
	gp->sched.pc = d->pc;
	if(!d->nofree)
		runtime路free(d);
	runtime路gogo(&gp->sched, 1);
}

// The implementation of the predeclared function recover.
// Cannot split the stack because it needs to reliably
// find the stack segment of its caller.
#pragma textflag 7
void
runtime路recover(byte *argp, Eface ret)
{
	Stktop *top, *oldtop;
	Panic *p;

	// Must be a panic going on.
	if((p = g->panic) == nil || p->recovered)
		goto nomatch;

	// Frame must be at the top of the stack segment,
	// because each deferred call starts a new stack
	// segment as a side effect of using reflect.call.
	// (There has to be some way to remember the
	// variable argument frame size, and the segment
	// code already takes care of that for us, so we
	// reuse it.)
	//
	// As usual closures complicate things: the fp that
	// the closure implementation function claims to have
	// is where the explicit arguments start, after the
	// implicit pointer arguments and PC slot.
	// If we're on the first new segment for a closure,
	// then fp == top - top->args is correct, but if
	// the closure has its own big argument frame and
	// allocated a second segment (see below),
	// the fp is slightly above top - top->args.
	// That condition can't happen normally though
	// (stack pointers go down, not up), so we can accept
	// any fp between top and top - top->args as
	// indicating the top of the segment.
	top = (Stktop*)g->stackbase;
	if(argp < (byte*)top - top->argsize || (byte*)top < argp)
		goto nomatch;

	// The deferred call makes a new segment big enough
	// for the argument frame but not necessarily big
	// enough for the function's local frame (size unknown
	// at the time of the call), so the function might have
	// made its own segment immediately.  If that's the
	// case, back top up to the older one, the one that
	// reflect.call would have made for the panic.
	//
	// The fp comparison here checks that the argument
	// frame that was copied during the split (the top->args
	// bytes above top->fp) abuts the old top of stack.
	// This is a correct test for both closure and non-closure code.
	oldtop = (Stktop*)top->stackbase;
	if(oldtop != nil && top->argp == (byte*)oldtop - top->argsize)
		top = oldtop;

	// Now we have the segment that was created to
	// run this call.  It must have been marked as a panic segment.
	if(!top->panic)
		goto nomatch;

	// Okay, this is the top frame of a deferred call
	// in response to a panic.  It can see the panic argument.
	p->recovered = 1;
	ret = p->arg;
	FLUSH(&ret);
	return;

nomatch:
	ret.type = nil;
	ret.data = nil;
	FLUSH(&ret);
}


// Put on gfree list.  Sched must be locked.
static void
gfput(G *g)
{
	if(g->stackguard - StackGuard != g->stack0)
		runtime路throw("invalid stack in gfput");
	g->schedlink = runtime路sched.gfree;
	runtime路sched.gfree = g;
}

// Get from gfree list.  Sched must be locked.
static G*
gfget(void)
{
	G *g;

	g = runtime路sched.gfree;
	if(g)
		runtime路sched.gfree = g->schedlink;
	return g;
}

void
runtime路Breakpoint(void)
{
	runtime路breakpoint();
}

void
runtime路Goexit(void)
{
	rundefer();
	runtime路goexit();
}

void
runtime路Gosched(void)
{
	runtime路gosched();
}

// Implementation of runtime.GOMAXPROCS.
// delete when scheduler is stronger
int32
runtime路gomaxprocsfunc(int32 n)
{
	int32 ret;
	uint32 v;

	schedlock();
	ret = runtime路gomaxprocs;
	if(n <= 0)
		n = ret;
	if(n > maxgomaxprocs)
		n = maxgomaxprocs;
	runtime路gomaxprocs = n;
	if(runtime路gomaxprocs > 1)
		runtime路singleproc = false;
 	if(runtime路gcwaiting != 0) {
 		if(atomic_mcpumax(runtime路sched.atomic) != 1)
 			runtime路throw("invalid mcpumax during gc");
		schedunlock();
		return ret;
	}

	setmcpumax(n);

	// If there are now fewer allowed procs
	// than procs running, stop.
	v = runtime路atomicload(&runtime路sched.atomic);
	if(atomic_mcpu(v) > n) {
		schedunlock();
		runtime路gosched();
		return ret;
	}
	// handle more procs
	matchmg();
	schedunlock();
	return ret;
}

void
runtime路LockOSThread(void)
{
	if(m == &runtime路m0 && runtime路sched.init) {
		runtime路sched.lockmain = true;
		return;
	}
	m->lockedg = g;
	g->lockedm = m;
}

void
runtime路UnlockOSThread(void)
{
	if(m == &runtime路m0 && runtime路sched.init) {
		runtime路sched.lockmain = false;
		return;
	}
	m->lockedg = nil;
	g->lockedm = nil;
}

bool
runtime路lockedOSThread(void)
{
	return g->lockedm != nil && m->lockedg != nil;
}

// for testing of callbacks
void
runtime路golockedOSThread(bool ret)
{
	ret = runtime路lockedOSThread();
	FLUSH(&ret);
}

// for testing of wire, unwire
void
runtime路mid(uint32 ret)
{
	ret = m->id;
	FLUSH(&ret);
}

void
runtime路NumGoroutine(int32 ret)
{
	ret = runtime路sched.gcount;
	FLUSH(&ret);
}

int32
runtime路gcount(void)
{
	return runtime路sched.gcount;
}

int32
runtime路mcount(void)
{
	return runtime路sched.mcount;
}

void
runtime路badmcall(void)  // called from assembly
{
	runtime路throw("runtime: mcall called on m->g0 stack");
}

void
runtime路badmcall2(void)  // called from assembly
{
	runtime路throw("runtime: mcall function returned");
}

static struct {
	Lock;
	void (*fn)(uintptr*, int32);
	int32 hz;
	uintptr pcbuf[100];
} prof;

// Called if we receive a SIGPROF signal.
void
runtime路sigprof(uint8 *pc, uint8 *sp, uint8 *lr, G *gp)
{
	int32 n;

	if(prof.fn == nil || prof.hz == 0)
		return;

	runtime路lock(&prof);
	if(prof.fn == nil) {
		runtime路unlock(&prof);
		return;
	}
	n = runtime路gentraceback(pc, sp, lr, gp, 0, prof.pcbuf, nelem(prof.pcbuf));
	if(n > 0)
		prof.fn(prof.pcbuf, n);
	runtime路unlock(&prof);
}

// Arrange to call fn with a traceback hz times a second.
void
runtime路setcpuprofilerate(void (*fn)(uintptr*, int32), int32 hz)
{
	// Force sane arguments.
	if(hz < 0)
		hz = 0;
	if(hz == 0)
		fn = nil;
	if(fn == nil)
		hz = 0;

	// Stop profiler on this cpu so that it is safe to lock prof.
	// if a profiling signal came in while we had prof locked,
	// it would deadlock.
	runtime路resetcpuprofiler(0);

	runtime路lock(&prof);
	prof.fn = fn;
	prof.hz = hz;
	runtime路unlock(&prof);
	runtime路lock(&runtime路sched);
	runtime路sched.profilehz = hz;
	runtime路unlock(&runtime路sched);

	if(hz != 0)
		runtime路resetcpuprofiler(hz);
}

void (*libcgo_setenv)(byte**);

// Update the C environment if cgo is loaded.
// Called from syscall.Setenv.
void
syscall路setenv_c(String k, String v)
{
	byte *arg[2];

	if(libcgo_setenv == nil)
		return;

	arg[0] = runtime路malloc(k.len + 1);
	runtime路memmove(arg[0], k.str, k.len);
	arg[0][k.len] = 0;

	arg[1] = runtime路malloc(v.len + 1);
	runtime路memmove(arg[1], v.str, v.len);
	arg[1][v.len] = 0;

	runtime路asmcgocall((void*)libcgo_setenv, arg);
	runtime路free(arg[0]);
	runtime路free(arg[1]);
}
