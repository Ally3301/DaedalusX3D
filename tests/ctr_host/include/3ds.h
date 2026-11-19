#pragma once

// Minimal libctru declarations used by the production CTR utility sources.
#include <stdint.h>
#include <stddef.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef int32_t Result;
typedef uint32_t Handle;
typedef struct FakeThread *Thread;
typedef void (*ThreadFunc)(void *);

#define SYSCLOCK_ARM11 268123480ULL
#define U64_MAX UINT64_MAX

Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int priority,
                    int processor_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
Handle threadGetHandle(Thread thread);
Result svcSetThreadPriority(Handle handle, int priority);
void svcSleepThread(s64 nanoseconds);
u64 svcGetSystemTick(void);
