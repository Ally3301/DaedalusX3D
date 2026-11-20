// Host tests link the actual ThreadCTR.cpp and TimingCTR.cpp, not copies.
#include "stdafx.h"
#include "Utility/Thread.h"
#include "Utility/Timing.h"
#include "SysCTR/UI/UITheme.h"
#include "SysCTR/UI/UITouchInput.h"
#include "SysCTR/UI/TouchControllerCTR.h"
#include "SysCTR/UI/CoverScraperCTR.h"
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
void test_ui_theme_and_touch() {
    // Theme helpers & Light/Dark toggle
    CHECK(UI::RGBA(0x12, 0x34, 0x56, 0x78) == 0x78563412u);
    CHECK(UI::WithAlpha(0xFF563412u, 0x40) == 0x40563412u);
    CHECK(UI::ToClearRGBA8(UI::RGBA(0x12, 0x34, 0x56, 0xFF)) == 0x123456FFu);
    CHECK(UI::LerpColor(UI::RGBA(0, 0, 0, 255), UI::RGBA(100, 200, 50, 255), 0.5f) ==
          UI::RGBA(50, 100, 25, 255));

    UI::SetThemeIndex(UI::THEME_DARK);
    CHECK(!UI::ActiveTheme().isLight);
    UI::ToggleLightDark();
    CHECK(UI::GetThemeIndex() == UI::THEME_LIGHT);
    CHECK(UI::ActiveTheme().isLight);
    UI::ToggleLightDark();
    CHECK(UI::GetThemeIndex() == UI::THEME_DARK);
    UI::CycleTheme(-1);
    CHECK(UI::GetThemeIndex() == UI::THEME_COUNT - 1);
    UI::CycleTheme(1);
    CHECK(UI::GetThemeIndex() == UI::THEME_DARK);

    // TouchTracker: ignore spurious (0,0) hardware edge frames
    UI::TouchTracker t;
    t.Reset();
    t.UpdateRaw(true, 0, 0);
    CHECK(!t.active && !t.justPressed);

    // Clean tap inside rect
    UI::Rect btn = { 20.0f, 40.0f, 80.0f, 30.0f };
    t.UpdateRaw(true, 50, 55);
    CHECK(t.active && t.justPressed && !t.dragged);
    CHECK(t.PressedIn(btn));
    CHECK(t.holdFrames == 1);
    t.UpdateRaw(true, 52, 56); // 2px jitter stays below 8px drag threshold
    CHECK(!t.dragged && t.holdFrames == 2);
    CHECK(t.HoldProgress(4) == 0.5f);
    t.UpdateRaw(false, 0, 0);
    CHECK(!t.active && t.justReleased && !t.dragged);
    CHECK(t.TappedIn(btn));

    // Drag gesture exceeds 8px threshold and does not fire tap on release
    t.UpdateRaw(true, 50, 55);
    t.UpdateRaw(true, 50, 70);
    CHECK(t.dragged && t.deltaY == 15.0f);
    t.UpdateRaw(false, 0, 0);
    CHECK(t.justReleased && !t.TappedIn(btn));

    // RepeatTimer initial trigger + repeat cadence
    UI::RepeatTimer rep;
    rep.Reset();
    CHECK(rep.Poll(1u, 1u, 1u, 0));
    int repeats = 0;
    for (int f = 0; f < 22; ++f) {
        if (rep.Poll(0u, 1u, 1u, 0))
            ++repeats;
    }
    CHECK(repeats == 2); // frames 18 and 22
    CHECK(!rep.Poll(0u, 0u, 1u, 0));
}

void test_touch_controller() {
    UI::TouchPadEngine eng;
    UI::TouchPadOutput out;

    // Default layout must be C-Buttons Cross
    CHECK(eng.GetConfig().layout == UI::TOUCH_LAYOUT_CCROSS);

    // 1) C-Buttons Cross: cardinals + diagonal slide (pad centered at 216, 132)
    eng.Update(true, 216, 83, out); // C-Up
    CHECK(out.n64Buttons == UI::N64Mask::kCUp && !out.swapSticks);
    eng.Update(true, 265, 132, out); // Slide to C-Right (elastic capture stays locked!)
    CHECK(out.n64Buttons == UI::N64Mask::kCRight);
    eng.Update(true, 255, 93, out); // Diagonal C-Up + C-Right
    CHECK(out.n64Buttons == (UI::N64Mask::kCUp | UI::N64Mask::kCRight));
    eng.Update(false, 0, 0, out); // Release clears buttons
    CHECK(out.n64Buttons == 0u);

    // 2) Dedicated Z trigger (Hold) + Z-Lock latch/unlatch (left col x=10..104)
    eng.Update(true, 55, 75, out);
    CHECK(out.n64Buttons == UI::N64Mask::kZTrig && eng.IsZActive() && !eng.IsZLatched());
    eng.Update(false, 0, 0, out);
    CHECK(out.n64Buttons == 0u);
    eng.Update(true, 55, 134, out); // Tap Z-Lock pill (y=123..149)
    CHECK(eng.IsZLatched() && (out.n64Buttons & UI::N64Mask::kZTrig) != 0u);
    eng.Update(false, 0, 0, out);
    CHECK(eng.IsZLatched() && (out.n64Buttons & UI::N64Mask::kZTrig) != 0u);
    eng.Update(true, 55, 134, out); // Tap again unlocks
    CHECK(!eng.IsZLatched());
    eng.Update(false, 0, 0, out);

    // 3) Quick Save hold guard: must NOT fire before 21 frames, fires at frame 21
    bool qsFiredEarly = false;
    for (int f = 1; f < UI::TouchPadEngine::kQuickHoldFrames; ++f) {
        eng.Update(true, 30, 180, out);
        if (out.requestQuickSave) qsFiredEarly = true;
    }
    CHECK(!qsFiredEarly && eng.QuickSaveProgress() > 0.9f);
    eng.Update(true, 30, 180, out);
    CHECK(out.requestQuickSave);
    eng.Update(true, 30, 180, out); // One-shot: doesn't re-fire while still held
    CHECK(!out.requestQuickSave);
    eng.Update(false, 0, 0, out);

    // 4) Top bar mode cycle: C-Cross -> C-Stick -> Dual-Analog Shooter -> D-Pad
    eng.Update(true, 180, 12, out);
    eng.Update(false, 0, 0, out);
    CHECK(eng.GetConfig().layout == UI::TOUCH_LAYOUT_CSTICK);

    // C-Stick deadzone check (center does nothing; far left triggers C-Left)
    eng.Update(true, 220, 132, out);
    CHECK(out.n64Buttons == 0u);
    eng.Update(true, 120, 132, out); // Dragged past outer radius: elastic capture still holds!
    CHECK(out.n64Buttons == UI::N64Mask::kCLeft);
    eng.Update(false, 0, 0, out);

    // 5) Dual-Analog Shooter mode: proportional N64 Main Stick + swapSticks
    UI::TouchPadConfig cfg = eng.GetConfig();
    cfg.layout = UI::TOUCH_LAYOUT_DUAL_SHOOTER;
    cfg.invertY = true;
    eng.SetConfig(cfg);
    eng.Update(true, 216, 60, out); // Up on screen -> inverted Y gives negative analogY
    CHECK(out.swapSticks && out.analogY < -50 && out.n64Buttons == 0u);
    eng.Update(false, 0, 0, out);

    // 6) Left-Handed mirror puts pad at x=104 and Z trigger at x=216..310
    cfg.layout = UI::TOUCH_LAYOUT_DPAD;
    cfg.leftHanded = true;
    eng.SetConfig(cfg);
    eng.Update(true, 104, 80, out); // D-Up on left side
    CHECK(out.n64Buttons == UI::N64Mask::kDUp);
    eng.Update(false, 0, 0, out);
    eng.Update(true, 260, 75, out); // Z trigger on right side
    CHECK(out.n64Buttons == UI::N64Mask::kZTrig);
    eng.Update(false, 0, 0, out);

    // 7) Sleep & Wake + Top bar Pause button
    eng.Update(true, 250, 12, out); // Moon button
    eng.Update(false, 0, 0, out);
    CHECK(eng.GetConfig().layout == UI::TOUCH_LAYOUT_SLEEP);
    eng.Update(true, 160, 120, out); // Tap anywhere wakes back to D-Pad
    eng.Update(false, 0, 0, out);
    CHECK(eng.GetConfig().layout == UI::TOUCH_LAYOUT_DPAD);

    eng.Update(true, 290, 12, out); // Pause button
    eng.Update(false, 0, 0, out);
    CHECK(out.requestPause);
}

void test_cover_scraper() {
    CHECK(UI::CoverScraper::UrlEncodePathComponent("Super Mario 64 (USA).png") ==
          "Super%20Mario%2064%20%28USA%29.png");

    std::vector<std::string> urls;
    UI::CoverScraper::BuildCandidateUrls("Mario Kart 64 (ESP) [!].z64", "MARIOKART64", "EUR", urls);
    CHECK(urls.size() >= 3);
    CHECK(urls[0].find("Mario%20Kart%2064%20%28ESP%29.png") != std::string::npos);
    CHECK(urls[1].find("Mario%20Kart%2064%20%28USA%29.png") != std::string::npos);
    CHECK(urls[2].find("Mario%20Kart%2064%20%28Europe%29.png") != std::string::npos);

    UI::CoverScraper::BuildCandidateUrls("The Legend of Zelda - Ocarina of Time (v1.0).z64",
                                         "The Legend of Zelda - Ocarina of Time (v1.0)", "USA", urls);
    bool foundNoIntroThe = false;
    for (const std::string & u : urls) {
        if (u.find("Legend%20of%20Zelda%2C%20The%20-%20Ocarina%20of%20Time%20%28USA%29.png") != std::string::npos)
            foundNoIntroThe = true;
    }
    CHECK(foundNoIntroThe);
}
}

int main() {
    test_thread_creation_and_lifetime();
    test_thread_operations();
    test_fragment_cache_coverage();
    test_timing();
    test_ui_theme_and_touch();
    test_touch_controller();
    test_cover_scraper();
    std::printf("CTR host tests passed (%d checks)\n", checks);
    return 0;
}
