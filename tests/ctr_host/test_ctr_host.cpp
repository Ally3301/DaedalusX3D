// Host tests link the actual ThreadCTR.cpp and TimingCTR.cpp, not copies.
#include "stdafx.h"
#include "Utility/Thread.h"
#include "Utility/Timing.h"
#include "DynaRec/FragmentCacheCoverage.h"
#include <3ds.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    std::exit(1); \
} } while (0)

// Track the nothrow allocation made by CreateThread, including both failure
// paths and the lifetime of the internal wrapper on successful creation.
bool fail_next_nothrow = false;
void *tracked[8] = {};
bool freed[8] = {};
int tracked_count = 0;
}

void *operator new(std::size_t size) {
    if (void *p = std::malloc(size)) return p;
    throw std::bad_alloc();
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    if (fail_next_nothrow) { fail_next_nothrow = false; return nullptr; }
    void *p = std::malloc(size);
    if (p && tracked_count < 8) { tracked[tracked_count] = p; freed[tracked_count++] = false; }
    return p;
}
void operator delete(void *p) noexcept {
    for (int i = 0; i < tracked_count; ++i)
        if (tracked[i] == p && !freed[i]) { freed[i] = true; break; }
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }

struct FakeThread { ThreadFunc entry; void *arg; Handle handle; };
namespace {
FakeThread threads[4] = {};
int create_calls = 0;
bool fail_create = false;
size_t stack_size_seen = 0;
int create_priority = 0, processor_seen = 0;
bool detached_seen = true;
Thread joined = nullptr, released = nullptr;
u64 join_timeout = 0;
Result join_result = 0;
Handle priority_handle = 0;
int priority_value = 0, priority_calls = 0;
s64 sleep_ns = -1;
u64 tick_value = 0;
}

Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int priority,
                    int processor_id, bool detached) {
    ++create_calls;
    stack_size_seen = stack_size; create_priority = priority;
    processor_seen = processor_id; detached_seen = detached;
    if (fail_create) return nullptr;
    CHECK(create_calls <= 4);
    FakeThread &t = threads[create_calls - 1];
    t = {entry, arg, static_cast<Handle>(100 + create_calls)};
    return &t;
}
Result threadJoin(Thread t, u64 timeout) { joined = t; join_timeout = timeout; return join_result; }
void threadFree(Thread t) { released = t; }
Handle threadGetHandle(Thread t) { return t->handle; }
Result svcSetThreadPriority(Handle h, int p) { priority_handle = h; priority_value = p; ++priority_calls; return 0; }
void svcSleepThread(s64 ns) { sleep_ns = ns; }
u64 svcGetSystemTick() { return tick_value; }

namespace {
struct Argument { int value; int calls; int allocation_index; };
u32 callback(void *ptr) {
    Argument *arg = static_cast<Argument *>(ptr);
    CHECK(freed[arg->allocation_index]); // wrapper must be destroyed before callback
    ++arg->calls;
    return static_cast<u32>(arg->value);
}

void test_thread_creation_and_lifetime() {
    fail_next_nothrow = true;
    CHECK(CreateThread("allocation failure", callback, nullptr) == kInvalidThreadHandle);
    CHECK(create_calls == 0);
    CHECK(tracked_count == 0);

    fail_create = true;
    CHECK(CreateThread("creation failure", callback, nullptr) == kInvalidThreadHandle);
    CHECK(create_calls == 1);
    CHECK(tracked_count == 1 && freed[0]);
    fail_create = false;

    Argument a = {42, 0, 1}, b = {99, 0, 2};
    ThreadHandle ha = CreateThread("first", callback, &a);
    ThreadHandle hb = CreateThread("second", callback, &b);
    CHECK(ha != kInvalidThreadHandle && hb != kInvalidThreadHandle && ha != hb);
    CHECK(create_calls == 3 && tracked_count == 3);
    CHECK(stack_size_seen == 0x10000 && create_priority == 0x1a);
    CHECK(processor_seen == -2 && !detached_seen);
    CHECK(!freed[1] && !freed[2] && a.calls == 0 && b.calls == 0);
    // Invoke in reverse order, after CreateThread returned, as a scheduler may.
    Thread tb = reinterpret_cast<Thread>(hb), ta = reinterpret_cast<Thread>(ha);
    tb->entry(tb->arg);
    CHECK(b.calls == 1 && a.calls == 0 && freed[2] && !freed[1]);
    ta->entry(ta->arg);
    CHECK(a.calls == 1 && b.calls == 1 && freed[1]);
    ReleaseThreadHandle(ha);
    CHECK(released == ta);
}

void test_thread_operations() {
    Thread t = &threads[1];
    ThreadHandle handle = reinterpret_cast<ThreadHandle>(t);
    const int priorities[] = {0x1b, 0x1a, 0x19, 0x18};
    for (int i = 0; i < 4; ++i) {
        SetThreadPriority(handle, static_cast<EThreadPriority>(i));
        CHECK(priority_handle == t->handle && priority_value == priorities[i]);
    }
    CHECK(priority_calls == 4);
    SetThreadPriority(kInvalidThreadHandle, TP_NORMAL);
    SetThreadPriority(handle, static_cast<EThreadPriority>(-1));
    SetThreadPriority(handle, TP_NUM_PRIORITIES);
    CHECK(priority_calls == 4);

    struct JoinCase { s32 timeout; u64 expected; Result result; bool success; };
    const JoinCase cases[] = {
        {-1, UINT64_MAX, 0, true}, {INT32_MIN, UINT64_MAX, -1, false},
        {0, 0, 1, true}, {1, 1000000ULL, -1, false},
        {INT32_MAX, 2147483647000000ULL, 0, true}
    };
    for (const JoinCase &c : cases) {
        join_result = c.result;
        CHECK(JoinThread(handle, c.timeout) == c.success);
        CHECK(joined == t && join_timeout == c.expected);
    }
    const u32 milliseconds[] = {0, 1, UINT32_MAX};
    for (u32 ms : milliseconds) {
        ThreadSleepMs(ms);
        CHECK(sleep_ns == static_cast<s64>(ms) * 1000000LL);
    }
    const u32 ticks[] = {0, 1, 2, 268123479, 268123480, UINT32_MAX};
    for (u32 n : ticks) {
        ThreadSleepTicks(n);
        CHECK(sleep_ns == static_cast<s64>(static_cast<u64>(n) * 1000000000ULL / SYSCLOCK_ARM11));
    }
    ThreadYield();
    CHECK(sleep_ns == 1);
}


void test_fragment_cache_coverage() {
    CFragmentCacheCoverage coverage;
    CHECK(!coverage.IsCovered(0x80001000u, 4));
    coverage.ExtendCoverage(0x80001020u, 4);
    CHECK(coverage.IsCovered(0x80001020u, 1));
    CHECK(coverage.IsCovered(0x00001020u, 1));
    CHECK(coverage.IsCovered(0xa0001020u, 1));
    CHECK(coverage.IsCovered(0x80001000u, 64));
    CHECK(!coverage.IsCovered(0x80001040u, 1));
    CHECK(!coverage.IsCovered(0x80001020u, 0));

    // A write crossing a 64-byte boundary covers both blocks.
    coverage.ExtendCoverage(0x8000203eu, 4);
    CHECK(coverage.IsCovered(0x80002000u, 64));
    CHECK(coverage.IsCovered(0x80002040u, 1));
    CHECK(!coverage.IsCovered(0x80002080u, 1));

    // Range arithmetic must clamp at the end of the 8 MiB RDRAM window.
    coverage.ExtendCoverage(0x807fffffu, 16);
    CHECK(coverage.IsCovered(0x807fffffu, 1));
    CHECK(!coverage.IsCovered(0x80800000u, 1));
    CHECK(!coverage.IsCovered(0xc0000000u, 64));

    coverage.Reset();
    CHECK(!coverage.IsCovered(0x80001020u, 1));

    // Trace coverage must stay sparse across taken branches. A single trace
    // may contain instructions far apart in guest RAM; those gaps are not code.
    coverage.ExtendCoverage(0x80010000u, 4);
    coverage.ExtendCoverage(0x80020000u, 4);
    CHECK(coverage.IsCovered(0x80010000u, 4));
    CHECK(coverage.IsCovered(0x80020000u, 4));
    CHECK(!coverage.IsCovered(0x80018000u, 4));
}

void test_timing() {
    u64 value = 0;
    CHECK(NTiming::GetPreciseFrequency(&value) && value == SYSCLOCK_ARM11);
    const u64 times[] = {0, 1, UINT64_MAX};
    for (u64 t : times) {
        tick_value = t;
        CHECK(NTiming::GetPreciseTime(&value) && value == t);
    }
    const u64 ticks[] = {0, 1, SYSCLOCK_ARM11 / 1000 - 1,
        SYSCLOCK_ARM11 / 1000, SYSCLOCK_ARM11 / 1000 + 1,
        SYSCLOCK_ARM11 - 1, SYSCLOCK_ARM11, SYSCLOCK_ARM11 + 1,
        UINT64_MAX / 1000, UINT64_MAX / 1000 + 1, UINT64_MAX};
    for (u64 t : ticks) {
        // Independent 128-bit arithmetic, including values whose t*1000 wraps u64.
        u64 expected = static_cast<u64>((static_cast<unsigned __int128>(t) * 1000) / SYSCLOCK_ARM11);
        CHECK(NTiming::ToMilliseconds(t) == expected);
    }
}
}

int main() {
    test_thread_creation_and_lifetime();
    test_thread_operations();
    test_fragment_cache_coverage();
    test_timing();
    std::printf("CTR host tests passed (%d checks)\n", checks);
    return 0;
}
