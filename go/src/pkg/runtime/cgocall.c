// Copyright 2009 The Go Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "arch_GOARCH.h"
#include "stack.h"
#include "cgocall.h"
#include "race.h"

// Cgo call and callback support.
// cgo调用以及回调支持
//
// 从Go中调用C的函数f，cgo生成代码调用runtime.cgocall(_cgo_Cfunc_f, frame)。
// 其中_cgo_Cfunc_f是一个由cgo生成并由gcc编译的函数。
// To call into the C function f from Go, the cgo-generated code calls
// runtime.cgocall(_cgo_Cfunc_f, frame), where _cgo_Cfunc_f is a
// gcc-compiled function written by cgo.
//
// runtime.cgocall将g锁定到m，调用entersyscall，这样不会阻塞其它的goroutine或者垃圾回收，
// 然后调用runtime.asmcgocall(_cgo_Cfunc_f, frame)
// runtime.cgocall (below) locks g to m, calls entersyscall
// so as not to block other goroutines or the garbage collector,
// and then calls runtime.asmcgocall(_cgo_Cfunc_f, frame). 
//
// runtime.asmcgocall切换到m->g0栈(这个栈是操作系统分配的栈)，因此可以安全地运行gcc编译的代码以及执行_cgo_Cfunc_f(frame)
// runtime.asmcgocall (in asm_$GOARCH.s) switches to the m->g0 stack
// (assumed to be an operating system-allocated stack, so safe to run
// gcc-compiled code on) and calls _cgo_Cfunc_f(frame).
// (我有个疑问，m->g0栈也仅仅是有一个的，如果有多次cgo调用同时发生，怎么处理？？)
//
// _cgo_Cfunc_f使用从frame结构体中取得的参数调用实际的C函数f，将结果记录在frame中，然后返回到runtime.asmcgocall
// _cgo_Cfunc_f invokes the actual C function f with arguments
// taken from the frame structure, records the results in the frame,
// and returns to runtime.asmcgocall.
//
// 当Go重获控制权之后，runtime.asmcgocall切回之前的g(m->curg)的栈，并且返回到runtime.cgocall
// After it regains control, runtime.asmcgocall switches back to the
// original g (m->curg)'s stack and returns to runtime.cgocall.
//
// 当它重获控制权之后，runtime.cgocall调用exitsyscall，然后将g从m中解锁。
// exitsyscall后m会阻塞直到它可以运行Go代码而不违反$GOMAXPROCS限制。
// After it regains control, runtime.cgocall calls exitsyscall, which blocks
// until this m can run Go code without violating the $GOMAXPROCS limit,
// and then unlocks g from m.
//
// 上述描述中略过了可能在gcc编译的函数f中会回调到Go代码，如果发生了这种情况，我们继续在f执行时挖兔子洞。
// The above description skipped over the possibility of the gcc-compiled
// function f calling back into Go.  If that happens, we continue down
// the rabbit hole during the execution of f.
//
// 为了能够让gcc编译的C代码调用Go函数p.GoF，cgo生成一个gcc编译的函数GoF(不是p.GoF，因为gcc没有包的概念)。
// gcc编译的C函数f会调用GoF
// To make it possible for gcc-compiled C code to call a Go function p.GoF,
// cgo writes a gcc-compiled function named GoF (not p.GoF, since gcc doesn't
// know about packages).  The gcc-compiled C function f calls GoF.
//
// GoF调用crosscall2(_cgoexp_GoF, frame, framesize). Crosscall2是一个两参数的适配器，
// 从一个ABI的gcc函数调用，到6c的函数调用ABI.它从gcc函数调用6c函数。这种情况下它调用_cgoexp_GoF(frame,framesize)，
// 仍然运行在m->g0栈并且不受$GOMAXPROCS限制。因此，这个代码不能直接调用任意的Go代码并且不能分配内存或者用尽m->g0的栈。
// GoF calls crosscall2(_cgoexp_GoF, frame, framesize).  Crosscall2
// (in cgo/gcc_$GOARCH.S, a gcc-compiled assembly file) is a two-argument
// adapter from the gcc function call ABI to the 6c function call ABI.
// It is called from gcc to call 6c functions.  In this case it calls
// _cgoexp_GoF(frame, framesize), still running on m->g0's stack
// and outside the $GOMAXPROCS limit.  Thus, this code cannot yet
// call arbitrary Go code directly and must be careful not to allocate
// memory or use up m->g0's stack.
//
// _cgoexp_GoF调用runtime.cgocallback(p.GoF, frame, framesize).使用_cgoexp_GoF而不是写一个crosscall3来直接调用，
// 原因是，它是由6c编译的，而不是gcc，因此可以引用到.名字比如runtime.cgocallback和p.GoF
// _cgoexp_GoF calls runtime.cgocallback(p.GoF, frame, framesize).
// (The reason for having _cgoexp_GoF instead of writing a crosscall3
// to make this call directly is that _cgoexp_GoF, because it is compiled
// with 6c instead of gcc, can refer to dotted names like
// runtime.cgocallback and p.GoF.)
//
// runtime.cgocallback从m->g0的栈切换到原来的g的栈，在这个栈中调用runtime.cgocallbackg(p.GoF, frame, framesize).
// SP是m->g0->sched.sp，这样任何在执行回调函数过程中使用m->g0的栈都是在存在的栈桢的下方。
// 在覆盖m->g0->sched.sp之前，它会将旧值push到m->g0栈中，这样后面可以恢复。
// runtime.cgocallback (in asm_$GOARCH.s) switches from m->g0's
// stack to the original g (m->curg)'s stack, on which it calls
// runtime.cgocallbackg(p.GoF, frame, framesize).
// As part of the stack switch, runtime.cgocallback saves the current
// SP as m->g0->sched.sp, so that any use of m->g0's stack during the
// execution of the callback will be done below the existing stack frames.
// Before overwriting m->g0->sched.sp, it pushes the old value on the
// m->g0 stack, so that it can be restored later.
//
// runtime.cgocallbackg现在是运行在一个真实的goroutine栈中（不是m->g0栈）。
// 它首先调用runtime.exitsyscall,这会阻塞这条goroutine直到满足$GOMAXPROCS限制条件。
// 一旦从exitsyscall返回，则可以安全地执行像调用内存分配或者是调用Go的回调函数p.GoF。
// runtime.cgocallbackg先defer一个函数来unwind m->g0.sched.sp，这样如果p.GoF panic了，
// m->g0.sched.sp将恢复为它的旧值：m->g0栈m->curg栈将会unwound in lock step
// 然后它调用p.GoF. 最后它会pop但是不会执行defered函数，调用runtime.entersyscall，返回到runtime.cgocallback.
// runtime.cgocallbackg (below) is now running on a real goroutine
// stack (not an m->g0 stack).  First it calls runtime.exitsyscall, which will
// block until the $GOMAXPROCS limit allows running this goroutine.
// Once exitsyscall has returned, it is safe to do things like call the memory
// allocator or invoke the Go callback function p.GoF.  runtime.cgocallbackg
// first defers a function to unwind m->g0.sched.sp, so that if p.GoF
// panics, m->g0.sched.sp will be restored to its old value: the m->g0 stack
// and the m->curg stack will be unwound in lock step.
// Then it calls p.GoF.  Finally it pops but does not execute the deferred
// function, calls runtime.entersyscall, and returns to runtime.cgocallback.
//
// 在runtime.cgocallback重获控制权之后，它切换回m->g0栈，从栈中恢复之前的m->g0.sched.sp值，返回到_cgoexp_GoF.
// After it regains control, runtime.cgocallback switches back to
// m->g0's stack (the pointer is still in m->g0.sched.sp), restores the old
// m->g0.sched.sp value from the stack, and returns to _cgoexp_GoF.
//
// _cgoexp_GoF立即返回到crosscall2，它会恢复被调者为gcc保存的寄存器并返回到GoF，然后返回到f.
// _cgoexp_GoF immediately returns to crosscall2, which restores the
// callee-save registers for gcc and returns to GoF, which returns to f.

void *_cgo_init;	/* filled in by dynamic linker when Cgo is available */
static int64 cgosync;  /* represents possible synchronization in C code */

// These two are only used by the architecture where TLS based storage isn't
// the default for g and m (e.g., ARM)
void *_cgo_load_gm; /* filled in by dynamic linker when Cgo is available */
void *_cgo_save_gm; /* filled in by dynamic linker when Cgo is available */

static void unwindm(void);

// Call from Go to C.

static void endcgo(void);
static FuncVal endcgoV = { endcgo };

// Gives a hint that the next syscall
// executed by the current goroutine will block.
// Currently used only on windows.
void
net·runtime_blockingSyscallHint(void)
{
	g->blockingsyscall = true;
}

void
runtime·cgocall(void (*fn)(void*), void *arg)
{
	Defer d;

	if(m->racecall) {
		runtime·asmcgocall(fn, arg);
		return;
	}

	if(!runtime·iscgo && !Windows)
		runtime·throw("cgocall unavailable");

	if(fn == 0)
		runtime·throw("cgocall nil");

	if(raceenabled)
		runtime·racereleasemerge(&cgosync);

	m->ncgocall++;

	/*
	 * Lock g to m to ensure we stay on the same stack if we do a
	 * cgo callback. Add entry to defer stack in case of panic.
	 */
	runtime·lockOSThread();
	d.fn = &endcgoV;
	d.siz = 0;
	d.link = g->defer;
	d.argp = (void*)-1;  // unused because unlockm never recovers
	d.special = true;
	d.free = false;
	g->defer = &d;

	m->ncgo++;

	/*
	 * Announce we are entering a system call
	 * so that the scheduler knows to create another
	 * M to run goroutines while we are in the
	 * foreign code.
	 *
	 * The call to asmcgocall is guaranteed not to
	 * split the stack and does not allocate memory,
	 * so it is safe to call while "in a system call", outside
	 * the $GOMAXPROCS accounting.
	 */
	/* 宣布我们进入了系统调用，这样调度器知道在我们运行外部代码时，它可以创建一个新的M来运行goroutine
	 * 调用asmcgocall是不会分裂栈并且不会分配内存的，因此可以完全地在"syscall call"时调用，不管$GOMAXPROCS计数
	 */
	if(g->blockingsyscall) {
		g->blockingsyscall = false;
		runtime·entersyscallblock();
	} else
		runtime·entersyscall();
	runtime·asmcgocall(fn, arg);
	runtime·exitsyscall();

	if(g->defer != &d || d.fn != &endcgoV)
		runtime·throw("runtime: bad defer entry in cgocallback");
	g->defer = d.link;
	endcgo();
}

static void
endcgo(void)
{
	runtime·unlockOSThread();
	m->ncgo--;
	if(m->ncgo == 0) {
		// We are going back to Go and are not in a recursive
		// call.  Let the GC collect any memory allocated via
		// _cgo_allocate that is no longer referenced.
		m->cgomal = nil;
	}

	if(raceenabled)
		runtime·raceacquire(&cgosync);
}

//CgoCall的数量
void
runtime·NumCgoCall(int64 ret)
{
	M *mp;

	ret = 0;
	for(mp=runtime·atomicloadp(&runtime·allm); mp; mp=mp->alllink)
		ret += mp->ncgocall;
	FLUSH(&ret);
}

// Helper functions for cgo code.
// cgo代码的辅助函数
void (*_cgo_malloc)(void*);
void (*_cgo_free)(void*);

void*
runtime·cmalloc(uintptr n)
{
	struct {
		uint64 n;
		void *ret;
	} a;

	a.n = n;
	a.ret = nil;
	runtime·cgocall(_cgo_malloc, &a);
	return a.ret;
}

void
runtime·cfree(void *p)
{
	runtime·cgocall(_cgo_free, p);
}

// Call from C back to Go.
// C调用Go函数
static FuncVal unwindmf = {unwindm};

// fn是Go函数
void
runtime·cgocallbackg(FuncVal *fn, void *arg, uintptr argsize)
{
	Defer d;

	if(m->racecall) {
		reflect·call(fn, arg, argsize);
		return;
	}

	if(g != m->curg)
		runtime·throw("runtime: bad g in cgocallback");

	runtime·exitsyscall();	// coming out of cgo call

	if(m->needextram) {
		m->needextram = 0;
		runtime·newextram();
	}

	// Add entry to defer stack in case of panic.
	d.fn = &unwindmf;
	d.siz = 0;
	d.link = g->defer;
	d.argp = (void*)-1;  // unused because unwindm never recovers
	d.special = true;
	d.free = false;
	g->defer = &d;

	if(raceenabled)
		runtime·raceacquire(&cgosync);

	// Invoke callback.
	reflect·call(fn, arg, argsize);

	if(raceenabled)
		runtime·racereleasemerge(&cgosync);

	// Pop defer.
	// Do not unwind m->g0->sched.sp.
	// Our caller, cgocallback, will do that.
	if(g->defer != &d || d.fn != &unwindmf)
		runtime·throw("runtime: bad defer entry in cgocallback");
	g->defer = d.link;

	runtime·entersyscall();	// going back to cgo call
}

static void
unwindm(void)
{
	// Restore sp saved by cgocallback during
	// unwind of g's stack (see comment at top of file).
	switch(thechar){
	default:
		runtime·throw("runtime: unwindm not implemented");
	case '8':
	case '6':
	case '5':
		m->g0->sched.sp = *(uintptr*)m->g0->sched.sp;
		break;
	}
}

void
runtime·badcgocallback(void)	// called from assembly
{
	runtime·throw("runtime: misaligned stack in cgocallback");
}

void
runtime·cgounimpl(void)	// called from (incomplete) assembly
{
	runtime·throw("runtime: cgo not implemented");
}

// For cgo-using programs with external linking,
// export "main" (defined in assembly) so that libc can handle basic
// C runtime startup and call the Go program as if it were
// the C main function.
#pragma cgo_export_static main
