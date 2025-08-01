/*
 * psp_osal.c
 *
 * Description:
 *
 * --------------------------------------------------------------------------
 *
 *      Pthreads-embedded (PTE) - POSIX Threads Library for embedded systems
 *      Copyright(C) 2008 Jason Schmidlapp
 *
 *      Contact Email: jschmidlapp@users.sourceforge.net
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library in the file COPYING.LIB;
 *      if not, write to the Free Software Foundation, Inc.,
 *      59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "pte_osal.h"
#include "pthread.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>

#include <vitasdk/utils.h>

/* For ftime */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/timeb.h>

// debug
#include <stdarg.h>

#define DEFAULT_STACK_SIZE_BYTES 0x1000

#define PTHREAD_EVID_CANCEL 0x1

#if 1
#include <psp2/kernel/clib.h>
#define DEBUG_PRINT(...) sceClibPrintf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

// TLS keys we are allowing for use here
#define TLS_SLOT_START 0x100
#define TLS_SLOT_END 0x200

#define MAX_THREADS 256

void* sceKernelGetReservedTLSAddr(unsigned key) {
  uintptr_t tpidruro;
  asm volatile("MRC p15, #0, %0, c13, c0, #3" : "=r"(tpidruro));
  return (void*)(tpidruro - 0x800 + 4 * key);
}

/*
 * Data stored on a per-thread basis - allocated in pte_osThreadCreate
 * and freed in pte_osThreadDelete.
 */
typedef struct pspThreadData
  {
    /* Thread ID of cancellation */
    SceUID threadId;

	/* Entry point and parameters to thread's main function */
	pte_osThreadEntryPoint entryPoint;
    void * argv;
    /* Semaphore used for cancellation.  Posted to by pte_osThreadCancel, 
       polled in pte_osSemaphoreCancellablePend */
	SceUID evid;

  } pspThreadData;


static pspThreadData thread_list[MAX_THREADS];

SceKernelLwMutexWork _tls_mutex __attribute__((aligned(8)));

static volatile uint32_t _last_tls_key = TLS_SLOT_START;

static inline int invert_priority(int priority)
{
	return (pte_osThreadGetMinPriority() - priority) + pte_osThreadGetMaxPriority();
}

int pspFindFreeThreadSlot()
{
	int i;
	for (i = 1; i < MAX_THREADS; i++)
	{
		if (thread_list[i].threadId == 0)
		{
			return i;
		}
	}
	return -1; // no free slots
}

int pspGetThreadIndex(SceUID threadId)
{
	int i;
	for (i = 0; i < MAX_THREADS; i++)
	{
		if (thread_list[i].threadId == threadId)
		{
			return i;
		}
	}
	return -1; // not found
}

/* A new thread's stub entry point.  It retrieves the real entry point from the per thread control
 * data as well as any parameters to this function, and then calls the entry point.
 */
int pspStubThreadEntry (unsigned int argc, void *argv)
{
	int index = pspGetThreadIndex(sceKernelGetThreadId());
	if (index < 0)
	{
		DEBUG_PRINT("pspStubThreadEntry: Invalid thread handle %x\n", sceKernelGetThreadId());
		return -1; // or some error code
	}
	if (thread_list[index].entryPoint == NULL)
	{
		DEBUG_PRINT("pspStubThreadEntry: No entry point set for thread %d\n", index);
		return -1; // or some error code
	}
	return (*(thread_list[index].entryPoint))(thread_list[index].argv);
}

/****************************************************************************
 *
 * Initialization
 *
 ***************************************************************************/

pte_osResult pte_osInit(void)
{
	/* Allocate some memory for our per-thread control data.  We use this for:
	 * 1. Entry point and parameters for the user thread's main function.
	 * 2. Semaphore used for thread cancellation.
	 */
	memset(thread_list, 0, sizeof(thread_list));

	sceKernelCreateLwMutex(&_tls_mutex, "TLS Access Mutex", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, NULL);
	thread_list[0].threadId = sceKernelGetThreadId();
	thread_list[0].evid = sceKernelCreateEventFlag("", 0, 0, NULL);
	sceKernelUnlockLwMutex(&_tls_mutex, 1);

	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Threads
 *
 ***************************************************************************/

pte_osResult pte_osThreadCreate(pte_osThreadEntryPoint entryPoint,
                                int stackSize,
                                int initialPriority,
                                void *argv,
                                pte_osThreadHandle* ppte_osThreadHandle)
{
	/* pthread_create was called by non-pthread thread */
	if (pspGetThreadIndex(sceKernelGetThreadId()) == -1) {
		if (pte_osInit() != PTE_OS_OK) {
			return PTE_OS_NO_RESOURCES;
		}
	}

	SceUID thid;

	if (stackSize < DEFAULT_STACK_SIZE_BYTES)
		stackSize = DEFAULT_STACK_SIZE_BYTES;

	/* Allocate some memory for our per-thread control data.  We use this for:
	 * 1. Entry point and parameters for the user thread's main function.
	 * 2. Semaphore used for thread cancellation.
	 */
	sceKernelLockLwMutex(&_tls_mutex, 1, 0);
	int index = pspFindFreeThreadSlot();
	if (index < 0)
	{
		DEBUG_PRINT("No free thread slots available: PTE_OS_NO_RESOURCES\n");
		sceKernelUnlockLwMutex(&_tls_mutex, 1);
		return PTE_OS_NO_RESOURCES;
	}

	thid = sceKernelCreateThread("pthread",
								 pspStubThreadEntry,
								 invert_priority(initialPriority),
								 stackSize,
								 0,
								 0,
								 NULL);

	if (thid < 0)
	{
		// TODO: expand this further
		if (thid == SCE_KERNEL_ERROR_NO_MEMORY)
		{
			DEBUG_PRINT("sceKernelCreateThread: PTE_OS_NO_RESOURCES\n");
			sceKernelUnlockLwMutex(&_tls_mutex, 1);
			return PTE_OS_NO_RESOURCES;
		}
		else
		{
			DEBUG_PRINT("sceKernelCreateThread: PTE_OS_GENERAL_FAILURE: %x\n", thid);
			sceKernelUnlockLwMutex(&_tls_mutex, 1);
			return PTE_OS_GENERAL_FAILURE;
		}
	}

	thread_list[index].threadId = thid;
	thread_list[index].entryPoint = entryPoint;
	thread_list[index].argv = argv;
	thread_list[index].evid = sceKernelCreateEventFlag("", 0, 0, NULL);

	sceKernelUnlockLwMutex(&_tls_mutex, 1);
	*ppte_osThreadHandle = thid;
	return PTE_OS_OK;
}

pte_osResult pte_osThreadStart(pte_osThreadHandle osThreadHandle)
{
	sceKernelStartThread(osThreadHandle, 0, NULL);
	return PTE_OS_OK;
}

pte_osResult pte_osThreadDelete(pte_osThreadHandle handle)
{
	int index = pspGetThreadIndex(handle);
	sceKernelDeleteEventFlag(thread_list[index].evid);

	sceKernelLockLwMutex(&_tls_mutex, 1, 0);
	thread_list[index].threadId = 0;
	thread_list[index].evid = 0;
	thread_list[index].entryPoint = NULL;
	thread_list[index].argv = NULL;
	sceKernelUnlockLwMutex(&_tls_mutex, 1);

	sceKernelDeleteThread(handle);
	return PTE_OS_OK;
}

pte_osResult pte_osThreadExitAndDelete(pte_osThreadHandle handle)
{
	pte_osThreadDelete(handle);
	sceKernelExitDeleteThread(0);
	return PTE_OS_OK;
}

void pte_osThreadExit()
{
	sceKernelExitThread(0);
}

/*
 * This has to be cancellable, so we can't just call sceKernelWaitThreadEnd.
 * Instead, poll on this in a loop, like we do for a cancellable semaphore.
 */
pte_osResult pte_osThreadWaitForEnd(pte_osThreadHandle threadHandle)
{
	int status = 0;
	SceUID evid = thread_list[pspGetThreadIndex(sceKernelGetThreadId())].evid;
	while (1)
	{
		unsigned int bits = 0;
		sceKernelPollEventFlag(evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

		if (bits & PTHREAD_EVID_CANCEL)
		{
			return PTE_OS_INTERRUPTED;
		}

		SceUInt timeout = POLLING_DELAY_IN_us;
		int res = sceKernelWaitThreadEndCB(threadHandle, &status, &timeout);

		if (res < 0)
		{
			// TODO: associate error codes better
			if (res == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			{
				continue;
			}
			else
			{
				return PTE_OS_GENERAL_FAILURE;
			}
		}

		break;
	}

	return PTE_OS_OK;
}

pte_osThreadHandle pte_osThreadGetHandle(void)
{
	return sceKernelGetThreadId();
}

int pte_osThreadGetPriority(pte_osThreadHandle threadHandle)
{
	SceKernelThreadInfo thinfo;
	thinfo.size = sizeof(SceKernelThreadInfo);
	sceKernelGetThreadInfo(threadHandle, &thinfo);
	return invert_priority(thinfo.currentPriority);
}

pte_osResult pte_osThreadSetPriority(pte_osThreadHandle threadHandle, int newPriority)
{
	sceKernelChangeThreadPriority(threadHandle, invert_priority(newPriority));
	return PTE_OS_OK;
}

pte_osResult pte_osThreadCancel(pte_osThreadHandle threadHandle)
{
	int res = sceKernelSetEventFlag(thread_list[pspGetThreadIndex(threadHandle)].evid, PTHREAD_EVID_CANCEL);

	if (res < 0)
		return PTE_OS_GENERAL_FAILURE;

	return PTE_OS_OK;
}


pte_osResult pte_osThreadCheckCancel(pte_osThreadHandle threadHandle)
{
	unsigned int bits = 0;
	sceKernelPollEventFlag(thread_list[pspGetThreadIndex(threadHandle)].evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

	if (bits & PTHREAD_EVID_CANCEL)
		return PTE_OS_INTERRUPTED;

	return PTE_OS_OK;
}

void pte_osThreadSleep(unsigned int msecs)
{
	sceKernelDelayThread(msecs*1000);
}

int pte_osThreadGetMinPriority()
{
	return pte_osThreadGetDefaultPriority()-32;
}

int pte_osThreadGetMaxPriority()
{
	return pte_osThreadGetDefaultPriority()+31;
}

int pte_osThreadGetDefaultPriority()
{
	return 160;
}

int pte_osThreadGetAffinity(pte_osThreadHandle threadHandle)
{
	int affinity = sceKernelGetThreadCpuAffinityMask(threadHandle);
	if (affinity < 0)
	{
		return affinity;
	}
	// set to default
	if (affinity == 0) affinity = SCE_KERNEL_CPU_MASK_USER_ALL;
	affinity = affinity >> 16;
	return affinity;
}

pte_osResult pte_osThreadSetAffinity(pte_osThreadHandle threadHandle, int affinity)
{
	affinity = affinity << 16;
	if (sceKernelChangeThreadCpuAffinityMask(threadHandle, affinity) == 0)
		return PTE_OS_OK;
	return PTE_OS_INVALID_PARAM;
}

/****************************************************************************
 *
 * Mutexes
 *
 ****************************************************************************/

pte_osResult pte_osMutexCreate(pte_osMutexHandle *pHandle)
{
	SceUID muid = sceKernelCreateMutex("", 0, 0, NULL);

	if (muid < 0)
		return PTE_OS_GENERAL_FAILURE;

	*pHandle = muid;
	return PTE_OS_OK;
}

pte_osResult pte_osMutexDelete(pte_osMutexHandle handle)
{
	sceKernelDeleteMutex(handle);
	return PTE_OS_OK;
}


pte_osResult pte_osMutexLock(pte_osMutexHandle handle)
{
	sceKernelLockMutex(handle, 1, NULL);
	return PTE_OS_OK;
}

pte_osResult pte_osMutexTimedLock(pte_osMutexHandle handle, unsigned int timeoutMsecs)
{
	unsigned int timeoutUsecs = timeoutMsecs*1000;
	int status = sceKernelLockMutex(handle, 1, &timeoutUsecs);

	if (status < 0)
	{
		if (status == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			return PTE_OS_TIMEOUT;

		return PTE_OS_GENERAL_FAILURE;
	}

	return PTE_OS_OK;
}


pte_osResult pte_osMutexUnlock(pte_osMutexHandle handle)
{
	sceKernelUnlockMutex(handle, 1);
	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Semaphores
 *
 ***************************************************************************/

pte_osResult pte_osSemaphoreCreate(int initialValue, pte_osSemaphoreHandle *pHandle)
{
	SceUID handle = sceKernelCreateSema("",
									   0,              /* attributes (default) */
									   initialValue,   /* initial value        */
									   SEM_VALUE_MAX,  /* maximum value        */
									   0);             /* options (default)    */

	if (handle < 0)
		return PTE_OS_GENERAL_FAILURE;

	*pHandle = handle;
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphoreDelete(pte_osSemaphoreHandle handle)
{
	sceKernelDeleteSema(handle);
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphorePost(pte_osSemaphoreHandle handle, int count)
{
	sceKernelSignalSema(handle, count);
	return PTE_OS_OK;
}

pte_osResult pte_osSemaphorePend(pte_osSemaphoreHandle handle, unsigned int *pTimeoutMsecs)
{
	SceUInt timeoutus = 0;
	SceUInt *timeout = NULL;

	if (pTimeoutMsecs)
	{
		timeoutus = *pTimeoutMsecs * 1000;
		timeout = &timeoutus;
	}

	int result = sceKernelWaitSema(handle, 1, timeout);

	if (result < 0)
	{
		if (result == SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			return PTE_OS_TIMEOUT;

		return PTE_OS_GENERAL_FAILURE;
	}

	return PTE_OS_OK;
}

/*
 * Pend on a semaphore- and allow the pend to be cancelled.
 *
 * PSP OS provides no functionality to asynchronously interrupt a blocked call.  We simulte
 * this by polling on the main semaphore and the cancellation semaphore and sleeping in a loop.
 */
pte_osResult pte_osSemaphoreCancellablePend(pte_osSemaphoreHandle semHandle, unsigned int *pTimeout)
{
	SceUInt32 start = sceKernelGetProcessTimeLow();
	SceUID evid = thread_list[pspGetThreadIndex(sceKernelGetThreadId())].evid;
	while (1)
	{
		unsigned int bits = 0;
		sceKernelPollEventFlag(evid, PTHREAD_EVID_CANCEL, SCE_EVENT_WAITAND, &bits);

		if (bits & PTHREAD_EVID_CANCEL)
			return PTE_OS_INTERRUPTED;

		// only poll the semaphoreg
		SceUInt semTimeout = 5*POLLING_DELAY_IN_us;
		int res = sceKernelWaitSema(semHandle, 1, &semTimeout);

		if (res < 0)
		{
			// TODO: associate error codes better
			if (res != SCE_KERNEL_ERROR_WAIT_TIMEOUT)
			{
				return PTE_OS_GENERAL_FAILURE;
			}
		}
		else
		{
			break;
		}

		if (pTimeout)
		{
			if (sceKernelGetProcessTimeLow() - start > (*pTimeout) * 1000)
			{
				return PTE_OS_TIMEOUT;
			}
		}
	}

	return PTE_OS_OK;
}


/****************************************************************************
 *
 * Atomic Operations
 *
 ***************************************************************************/

int pte_osAtomicExchange(int *ptarg, int val)
{
	return atomic_exchange(ptarg, val);
}

int pte_osAtomicCompareExchange(int *pdest, int exchange, int comp)
{
	return __extension__ ({
		(void)(memory_order_seq_cst); (void)(memory_order_seq_cst);
		(__sync_val_compare_and_swap(pdest,
			comp, exchange));
	});
}

int pte_osAtomicExchangeAdd(int volatile* pAddend, int value)
{
	return atomic_fetch_add(pAddend, value);
}

int pte_osAtomicDecrement(int *pdest)
{
	return __sync_sub_and_fetch(pdest, 1);
}

int pte_osAtomicIncrement(int *pdest)
{
	return __sync_add_and_fetch(pdest, 1);
}

/****************************************************************************
 *
 * Thread Local Storage
 *
 ***************************************************************************/

pte_osResult pte_osTlsSetValue(unsigned int key, void *value)
{
    void *addr = sceKernelGetReservedTLSAddr(key);

    if (!addr)
    {
        DEBUG_PRINT("[TLS] ERROR: TLS address for key %u is NULL (possible invalid key or uninitialized TLS).\n", key);
        return PTE_OS_GENERAL_FAILURE;
    }

    *(void **)addr = value;
    return PTE_OS_OK;
}

void * pte_osTlsGetValue(unsigned int index)
{
	void **value = (void **)sceKernelGetReservedTLSAddr(index);
    return value ? *value : NULL;
}

pte_osResult pte_osTlsAlloc(unsigned int *pKey)
{
	if (_last_tls_key >= TLS_SLOT_END)
	{
		return PTE_OS_NO_RESOURCES; // no more TLS slots available
	}
	*pKey = _last_tls_key++;
	return PTE_OS_OK;
}

pte_osResult pte_osTlsFree(unsigned int index)
{
	// We should have enough slots. Don't worry about this for now
	return PTE_OS_OK;
}

/****************************************************************************
 *
 * Miscellaneous
 *
 ***************************************************************************/

int ftime(struct timeb *tb)
{
  struct timespec tv;

  clock_gettime(CLOCK_REALTIME, &tv);

  tb->time = tv.tv_sec;
  tb->millitm = tv.tv_nsec / 1000000;

  return 0;
}

/****************************************************************************
 *
 * Enable pthread before main
 *
 ***************************************************************************/

void __sinit(struct _reent *);

__attribute__((constructor(101)))
void pthread_setup(void) 
{
    pthread_init();
    __sinit(_REENT);
}
