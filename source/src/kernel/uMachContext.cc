//                              -*- Mode: C++ -*- 
// 
// uC++ Version 7.0.0, Copyright (C) Peter A. Buhr 1994
// 
// uMachContext.cc -- 
// 
// Author           : Peter A. Buhr
// Created On       : Fri Feb 25 15:46:42 1994
// Last Modified By : Peter A. Buhr
// Last Modified On : Fri Apr 12 17:18:37 2019
// Update Count     : 821
//
// This  library is free  software; you  can redistribute  it and/or  modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software  Foundation; either  version 2.1 of  the License, or  (at your
// option) any later version.
// 
// This library is distributed in the  hope that it will be useful, but WITHOUT
// ANY  WARRANTY;  without even  the  implied  warranty  of MERCHANTABILITY  or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
// 
// You should  have received a  copy of the  GNU Lesser General  Public License
// along  with this library.
// 


#define __U_KERNEL__
#include <uC++.h>
#include <uAlign.h>
#ifdef __U_PROFILER__
#include <uProfiler.h>
#endif // __U_PROFILER__
#include <uHeapLmmm.h>

#include <uDebug.h>					// access: uDebugWrite
#undef __U_DEBUG_H__					// turn off debug prints

#include <cerrno>
#include <unistd.h>					// write

#if defined( __x86_64__ )
extern "C" void uInvokeStub( UPP::uMachContext * );
#endif


using namespace UPP;


extern "C" void pthread_deletespecific_( void * );	// see pthread simulation


#define MinStackSize 1000				// minimum feasible stack size in bytes


namespace UPP {
    void uMachContext::invokeCoroutine( uBaseCoroutine &This ) { // magically invoke the "main" of the most derived class
	// Called from the kernel when starting a coroutine or task so must switch back to user mode.

	This.setState( uBaseCoroutine::Active );	// set state of next coroutine to active
	THREAD_GETMEM( This )->enableInterrupts();

#ifdef __U_PROFILER__
	if ( uThisTask().profileActive && uProfiler::uProfiler_postallocateMetricMemory ) {
	    (*uProfiler::uProfiler_postallocateMetricMemory)( uProfiler::profilerInstance, uThisTask() );
	} // if
	// also appears in uBaseCoroutine::corCxtSw
	if ( ! THREAD_GETMEM( disableInt ) && uThisTask().profileActive && uProfiler::uProfiler_registerCoroutineUnblock ) {
	    (*uProfiler::uProfiler_registerCoroutineUnblock)( uProfiler::profilerInstance, uThisTask() );
	} // if
#endif // __U_PROFILER__

	// At this point, execution is on the stack of the new coroutine or task that has just been switched to by the
	// kernel.  Therefore, interrupts can legitimately occur now.

	try {
	    This.corStarter();				// moved from uCoroutineMain to allow recursion on "main"
	    uBaseCoroutine::asyncpoll();		// cancellation checkpoint
	    This.main();				// start coroutine's "main" routine
	} catch ( uBaseCoroutine::UnwindStack &u ) {
	    u.exec_dtor = false;			// defuse the bomb or otherwise unwinding will continue
	    This.notHalted = false;			// terminate coroutine
	} catch( uBaseCoroutine::UnhandledException &ex ) {
	    This.handleUnhandled( &ex );
	} catch ( uBaseEvent &ex ) {
	    This.handleUnhandled( &ex );
	} catch( ... ) {				// unknown exception ?
	    This.handleUnhandled();
	} // try

	// check outside handler so exception is freed before suspending
	if ( ! This.notHalted ) {			// exceptional ending ?
	    This.suspend();				// restart last resumer, which should immediately propagate the nonlocal exception
	} // if
	if ( &This != activeProcessorKernel ) {		// uProcessorKernel exit ?
	    This.corFinish();
	    abort( "internal error, uMachContext::invokeCoroutine, no return" );
	} // if
    } // uMachContext::invokeCoroutine


    void uMachContext::invokeTask( uBaseTask &This ) {	// magically invoke the "main" of the most derived class
	// Called from the kernel when starting a coroutine or task so must switch back to user mode.

#if defined(__U_MULTI__)
	assert( THREAD_GETMEM( activeTask ) == &This );
#endif

	errno = 0;					// reset errno for each task
	This.currCoroutine->setState( uBaseCoroutine::Active ); // set state of next coroutine to active
	This.setState( uBaseTask::Running );

	// At this point, execution is on the stack of the new coroutine or task that has just been switched to by the
	// kernel.  Therefore, interrupts can legitimately occur now.
	THREAD_GETMEM( This )->enableInterrupts();

//	uHeapControl::startTask();

#ifdef __U_PROFILER__
	if ( uThisTask().profileActive && uProfiler::uProfiler_postallocateMetricMemory ) {
	    (*uProfiler::uProfiler_postallocateMetricMemory)( uProfiler::profilerInstance, uThisTask() );
	} // if
#endif // __U_PROFILER__

	uPthreadable *pthreadable = dynamic_cast< uPthreadable * > (&This);

	try {
	    try {
		uBaseCoroutine::asyncpoll();		// cancellation checkpoint
		This.main();				// start task's "main" routine
	    } catch( uBaseCoroutine::UnwindStack &evt ) {
		evt.exec_dtor = false;			// defuse the unwinder	
	    } catch( uBaseEvent &ex ) {
		ex.defaultTerminate();
		cleanup( This );			// preserve current exception
	    } catch( ... ) {
		if ( ! This.cancelInProgress() ) {
		    cleanup( This );			// preserve current exception
		    // CONTROL NEVER REACHES HERE!
		} // if
		if ( pthreadable ) {
		    pthreadable->stop_unwinding = true;	// prevent continuation of unwinding
		} // if
	    } // try
	} catch (...) {
	    uEHM::terminate();				// if defaultTerminate or std::terminate throws exception
	} // try
	// NOTE: this code needs further protection as soon as asynchronous cancellation is supported

	// Clean up storage associated with the task for pthread thread-specific data, e.g., exception handling
	// associates thread-specific data with any task.

	if ( This.pthreadData != nullptr ) {
	    pthread_deletespecific_( This.pthreadData );
	} // if

//	uHeapControl::finishTask();

	This.notHalted = false;
	This.setState( uBaseTask::Terminate );

	This.getSerial().leave2();
	// CONTROL NEVER REACHES HERE!
	abort( "(uMachContext &)%p.invokeTask() : internal error, attempt to return.", &This );
    } // uMachContext::invokeTask


    void uMachContext::cleanup( uBaseTask &This ) {
	try {
	    std::terminate();				// call task terminate routine
	} catch( ... ) {				// control should not return
	} // try

	if ( This.pthreadData != nullptr ) {		// see above for explanation
	    pthread_deletespecific_( This.pthreadData );
	} // if

	uEHM::terminate();				// call abort terminate routine
    } // uMachContext::cleanup


    void uMachContext::extraSave() {
	uContext *context;
	for ( uSeqIter<uContext> iter(additionalContexts); iter >> context; ) {
	    context->save();
	} // for
    } // uMachContext::extraSave


    void uMachContext::extraRestore() {
	uContext *context;
	for ( uSeqIter<uContext> iter(additionalContexts); iter >> context; ) {
	    context->restore();
	} // for
    } // uMachContext::extraRestore


    void uMachContext::startHere( void (*uInvoke)( uMachContext & ) ) {
#if defined( __i386__ )
// https://software.intel.com/sites/default/files/article/402129/mpx-linux64-abi.pdf
// The control bits of the MXCSR register are callee-saved (preserved across calls), while the status bits are
// caller-saved (not preserved). The x87 status word register is caller-saved, whereas the x87 control word is
// callee-saved.

// Bits 16 through 31 of the MXCSR register are reserved and are cleared on a power-up or reset of the processor;
// attempting to write a non-zero value to these bits, using either the FXRSTOR or LDMXCSR instructions, will result in
// a general-protection exception (#GP) being generated.
	struct FakeStack {
	    void *fixedRegisters[3];			// fixed registers ebx, edi, esi (popped on 1st uSwitch, values unimportant)
	    uint32_t mxcsr;				// MXCSR control and status register
	    uint16_t fpucsr;				// x87 FPU control and status register
	    void *rturn;				// where to go on return from uSwitch
	    void *dummyReturn;				// fake return compiler would have pushed on call to uInvoke
	    void *argument;				// for 16-byte ABI, 16-byte alignment starts here
	    void *padding[3];				// padding to force 16-byte alignment, as "base" is 16-byte aligned
	}; // FakeStack

	((uContext_t *)context)->SP = (char *)base - sizeof( FakeStack );
	((uContext_t *)context)->FP = nullptr;		// terminate stack with null fp

	((FakeStack *)(((uContext_t *)context)->SP))->dummyReturn = nullptr;
	((FakeStack *)(((uContext_t *)context)->SP))->argument = this; // argument to uInvoke
	((FakeStack *)(((uContext_t *)context)->SP))->rturn = rtnAdr( (void (*)())uInvoke );
	((FakeStack *)(((uContext_t *)context)->SP))->fpucsr = fncw;
	((FakeStack *)(((uContext_t *)context)->SP))->mxcsr = mxcsr; // see note above

#elif defined( __x86_64__ )

	struct FakeStack {
	    void *fixedRegisters[5];			// fixed registers rbx, r12, r13, r14, r15
	    uint32_t mxcsr;				// MXCSR control and status register
	    uint16_t fpucsr;				// x87 FPU control and status register
	    void *rturn;				// where to go on return from uSwitch
	    void *dummyReturn;				// null return address to provide proper alignment
	}; // FakeStack

	((uContext_t *)context)->SP = (char *)base - sizeof( FakeStack );
	((uContext_t *)context)->FP = nullptr;		// terminate stack with null fp
	
	((FakeStack *)(((uContext_t *)context)->SP))->dummyReturn = nullptr;
	((FakeStack *)(((uContext_t *)context)->SP))->rturn = rtnAdr( (void (*)())uInvokeStub );
	((FakeStack *)(((uContext_t *)context)->SP))->fixedRegisters[0] = this;
	((FakeStack *)(((uContext_t *)context)->SP))->fixedRegisters[1] = rtnAdr( (void (*)())uInvoke );
	((FakeStack *)(((uContext_t *)context)->SP))->fpucsr = fncw;
	((FakeStack *)(((uContext_t *)context)->SP))->mxcsr = mxcsr; // see note above

#else
	#error uC++ : internal error, unsupported architecture
#endif
    } // uMachContext::startHere


    /**************************************************************
	s  |  ,-----------------. \
	t  |  |                 | |
	a  |  | __U_CONTEXT_T__ | } (multiple of 8)
	c  |  |                 | |
	k  |  `-----------------' / <--- context (16 byte align)
	   |  ,-----------------. \ <--- base (stack grows down)
	g  |  |                 | |
	r  |  |    task stack   | } size (multiple of 16)
	o  |  |                 | |
	w  |  `-----------------' / <--- limit (16 byte align)
	t  |  0/8                   <--- storage
	h  V  ,-----------------.
	      |   guard page    |   debug only
	      | write protected |
	      `-----------------'   <--- 4/8/16K page alignment
    **************************************************************/

    void uMachContext::createContext( unsigned int storageSize ) { // used by all constructors
	size_t cxtSize = uCeiling( sizeof(__U_CONTEXT_T__), 8 ); // minimum alignment
	size_t size;

	if ( storage == nullptr ) {
	    size = uCeiling( storageSize, 16 );
	    // use malloc/memalign because "new" raises an exception for out-of-memory
#ifdef __U_DEBUG__
	    storage = memalign( pageSize, cxtSize + size + pageSize );
	    if ( ::mprotect( storage, pageSize, PROT_NONE ) == -1 ) {
		abort( "(uMachContext &)%p.createContext() : internal error, mprotect failure, error(%d) %s.", this, errno, strerror( errno ) );
	    } // if
#else
	    // assume malloc has 8 byte alignment so add 8 to allow rounding up to 16 byte alignment
	    storage = malloc( cxtSize + size + 8 );
#endif // __U_DEBUG__
	    if ( storage == nullptr ) {
		abort( "Attempt to allocate %zd bytes of storage for coroutine or task execution-state but insufficient memory available.", size );
	    } // if
#ifdef __U_DEBUG__
	    limit = (char *)storage + pageSize;
#else
	    limit = (char *)uCeiling( (unsigned long)storage, 16 ); // minimum alignment
#endif // __U_DEBUG__
	} else {
#ifdef __U_DEBUG__
	    if ( ((size_t)storage & (uAlign() - 1)) != 0 ) { // multiple of uAlign ?
		abort( "Stack storage %p for task/coroutine must be aligned on %d byte boundary.", storage, (int)uAlign() );
	    } // if
#endif // __U_DEBUG__
	    size = storageSize - cxtSize;
	    if ( size % 16 != 0 ) size -= 8;
	    limit = (void *)uCeiling( (unsigned long)storage, 16 ); // minimum alignment
	    storage = (void *)((uintptr_t)storage | 1);	// add user stack storage mark
	} // if
#ifdef __U_DEBUG__
	if ( size < MinStackSize ) {			// below minimum stack size ?
	    abort( "Stack size %zd provides less than minimum of %d bytes for a stack.", size, MinStackSize );
	} // if
#endif // __U_DEBUG__

	base = (char *)limit + size;
	context = base;

	extras.allExtras = 0;

//	uDebugPrt( "(uMachContext &)%p.createContext( %u ), storage:%p, limit:%p, base:%p, context:%p, size:0x%zd\n",	    
//		   this, storageSize, storage, limit, base, context, size );
    } // uMachContext::createContext


    void *uMachContext::stackPointer() const {
	if ( &uThisCoroutine() == this ) {		// accessing myself ?
	    void *sp;					// use my current stack value
#if defined( __i386__ )
	    asm( "movl %%esp,%0" : "=m" (sp) : );
#elif defined( __x86_64__ )
	    asm( "movq %%rsp,%0" : "=m" (sp) : );
#else
	    #error uC++ : internal error, unsupported architecture
#endif
	    return sp;
	} else {					// accessing another coroutine
	    return ((uContext_t *)context)->SP;
	} // if
    } // uMachContext::stackPointer


    ptrdiff_t uMachContext::stackFree() const {
	return (char *)stackPointer() - (char *)limit;
    } // uMachContext::stackFree


    ptrdiff_t uMachContext::stackUsed() const {
	return (char *)base - (char *)stackPointer();
    } // uMachContext::stackUsed


    void uMachContext::verify() {
	// Ignore boot task as it uses the UNIX stack.
	if ( storage == ((uBaseTask *)uKernelModule::bootTask)->storage ) return;

	void *sp = stackPointer();			// optimization

	if ( sp < limit ) {
	    abort( "Stack overflow detected: stack pointer %p below limit %p.\n"
		    "Possible cause is allocation of large stack frame(s) and/or deep call stack.",
		    sp, limit );
#define MINSTACKSIZE 1
	} else if ( stackFree() < MINSTACKSIZE * 1024 ) {
	    // Do not use fprintf because it uses a lot of stack space.
#define xstr(s) str(s)
#define str(s) #s
#define MINSTACKSIZEWARNING "uC++ Runtime warning : within " xstr(MINSTACKSIZE) "K of stack limit.\n"
	    uDebugWrite( STDERR_FILENO, MINSTACKSIZEWARNING, sizeof(MINSTACKSIZEWARNING) - 1 );
	} else if ( sp > base ) {
	    abort( "Stack underflow detected: stack pointer %p above base %p.\n"
		    "Possible cause is corrupted stack frame via overwriting memory.",
		    sp, base );
	} // if
    } // uMachContext::verify


    void *uMachContext::rtnAdr( void (*rtn)() ) {
	return (void *)rtn;
    } // uMachContext::rtnAdr
} // UPP


// Local Variables: //
// compile-command: "make install" //
// End: //
