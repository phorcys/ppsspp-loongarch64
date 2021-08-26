#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WINDOWS)

#include <windows.h>

#ifdef __MINGW32__
#include <excpt.h>
#endif

#define TLS_SUPPORTED

#elif defined(__ANDROID__)

#define TLS_SUPPORTED

#endif

#include <cstring>
#include <cstdint>

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"

#if PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(LINUX)
#define _GNU_SOURCE
#endif

#if !PPSSPP_PLATFORM(WINDOWS)
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#elif defined(__NetBSD__)
#include <lwp.h>
#endif

#ifdef TLS_SUPPORTED
static thread_local const char *curThreadName;
#endif

#ifdef __MINGW32__
#include <pshpack8.h>
typedef struct {
	DWORD dwType;
	LPCSTR szName;
	DWORD dwThreadID;
	DWORD dwFlags;
} THREADNAME_INFO;
#include <poppack.h>

static EXCEPTION_DISPOSITION NTAPI ignore_handler(EXCEPTION_RECORD *rec,
                                                  void *frame, CONTEXT *ctx,
                                                  void *disp)
{
	return ExceptionContinueExecution;
}
#endif

void SetCurrentThreadName(const char* threadName) {
#if PPSSPP_PLATFORM(WINDOWS)
	// Set the debugger-visible threadname through an unholy magic hack
	static const DWORD MS_VC_EXCEPTION = 0x406D1388;
#endif

	// TODO: Use the new function SetThreadDescription available since Windows 10, version 1607.

#if PPSSPP_PLATFORM(WINDOWS) && defined(__MINGW32__)
	// Thread information for VS compatible debugger. -1 sets current thread.
	THREADNAME_INFO ti;
	ti.dwType = 0x1000;
	ti.szName = threadName;
	ti.dwThreadID = -1;

	// Push an exception handler to ignore all following exceptions
	NT_TIB *tib = ((NT_TIB*)NtCurrentTeb());
	EXCEPTION_REGISTRATION_RECORD rec;
	rec.Next = tib->ExceptionList;
	rec.Handler = ignore_handler;
	tib->ExceptionList = &rec;

	// Visual Studio and compatible debuggers receive thread names from the
	// program through a specially crafted exception
	RaiseException(MS_VC_EXCEPTION, 0, sizeof(ti) / sizeof(ULONG_PTR),
	               (ULONG_PTR*)&ti);

	// Pop exception handler
	tib->ExceptionList = tib->ExceptionList->Next;
#elif PPSSPP_PLATFORM(WINDOWS)
#pragma pack(push,8)
	struct THREADNAME_INFO {
		DWORD dwType; // must be 0x1000
		LPCSTR szName; // pointer to name (in user addr space)
		DWORD dwThreadID; // thread ID (-1=caller thread)
		DWORD dwFlags; // reserved for future use, must be zero
	} info;
#pragma pack(pop)

	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = -1; //dwThreadID;
	info.dwFlags = 0;

#ifdef __MINGW32__
	__try1 (ehandler)
#else
	__try
#endif
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
#ifdef __MINGW32__
	__except1
#else
	__except(EXCEPTION_CONTINUE_EXECUTION)
#endif
	{}
#else

#if PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(LINUX)
	pthread_setname_np(pthread_self(), threadName);
#elif defined(__APPLE__)
	pthread_setname_np(threadName);
#elif defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	pthread_set_name_np(pthread_self(), threadName);
#elif defined(__NetBSD__)
	pthread_setname_np(pthread_self(), "%s", (void*)threadName);
#endif

	// Do nothing
#endif
	// Set the locally known threadname using a thread local variable.
#ifdef TLS_SUPPORTED
	curThreadName = threadName;
#endif
}

void AssertCurrentThreadName(const char *threadName) {
#ifdef TLS_SUPPORTED
	if (strcmp(curThreadName, threadName) != 0) {
		ERROR_LOG(SYSTEM, "Thread name assert failed: Expected %s, was %s", threadName, curThreadName);
	}
#endif
}

int GetCurrentThreadIdForDebug() {
#if __LIBRETRO__
	// Not sure why gettid() would not be available, but it isn't.
	// The return value of this function is only used in unit tests anyway...
	return 1;
#elif PPSSPP_PLATFORM(WINDOWS)
	return (int)GetCurrentThreadId();
#elif PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
	uint64_t tid = 0;
	pthread_threadid_np(NULL, &tid);
	return (int)tid;
#elif PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(LINUX)
	// See issue 14545
	return (int)syscall(__NR_gettid);
	// return (int)gettid();
#elif defined(__DragonFly__) || defined(__FreeBSD__)
	return pthread_getthreadid_np();
#elif defined(__NetBSD__)
	return _lwp_self();
#elif defined(__OpenBSD__)
	return getthrid();
#else
	return 1;
#endif
}
