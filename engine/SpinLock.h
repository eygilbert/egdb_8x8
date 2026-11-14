#pragma once
#define USE_CRITICAL_SECTION 0

#if USE_CRITICAL_SECTION

#define SPINLOCK CRITICAL_SECTION
#define ResetSpinLock(a) InitializeCriticalSection(&a)
#define AcquireSpinLock(a) EnterCriticalSection(&a)
#define ReleaseSpinLock(a) LeaveCriticalSection(&a)

#else

#include <intrin.h> 

typedef volatile CACHE_ALIGN long SPINLOCK[L1_CACHE_SIZE / sizeof(long)]; 
typedef volatile long *const SPINLOCK_P; 
#pragma intrinsic (_InterlockedExchange) 
#define LockedExchange(Target, Value) _InterlockedExchange(Target, Value) 

#define INLINE inline 
#define IsLocked(s) ((s)[0]) 
#define SetLocked(s, boolean) ((s)[0] = (boolean)) 
#define ResetSpinLock(s) SetLocked(s, 0) 
static INLINE void volatile ReleaseSpinLock(SPINLOCK_P s) {SetLocked(s, 0);} 
static INLINE int volatile TrySpinLock(SPINLOCK_P s) {return !(IsLocked(s) || LockedExchange(s, 1));} 
static INLINE void volatile AcquireSpinLock(SPINLOCK_P s) {
    while (IsLocked(s) || LockedExchange(s, 1))
        _mm_pause();
}
#endif
