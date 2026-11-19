
// Taken from PicoDrive's libretro implementation for 3DS.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>

#include "MemoryCTR.h"

int __stacksize__ = 1 * 1024 * 1024;

// Custom heap allocator for Old 3DS / New 3DS.
// libctru's default reserves up to 32 MiB for linear RAM even on Old 3DS 64 MiB
// (.3dsx), starving the process heap (~24 MiB) where the dynarec code buffer,
// N64 RDRAM, ROM cache, and UI live. Size the linear heap to what picaGL,
// vertex buffers, NDSP, and GPU textures actually need, and hand the rest to
// the process heap.
extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern char *fake_heap_start;
extern char *fake_heap_end;
void mappableInit(u32 addrMin, u32 addrMax);

void __system_allocateHeaps(void)
{
	Handle reslimit = 0;
	s64 max_commit = 0;
	s64 cur_commit = 0;
	ResourceLimitType type = RESLIMIT_COMMIT;

	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
		svcBreak(USERBREAK_PANIC);
	svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
	svcCloseHandle(reslimit);

	u32 remaining = (max_commit > cur_commit)
		? ((u32)(max_commit - cur_commit) & ~0xFFFu)
		: 0;

	const u32 MB = 1024u * 1024u;
	u32 linear = 0;
	if (remaining >= 96u * MB)
	{
		// New 3DS (124 MiB+): generous linear heap and huge process heap.
		linear = 24u * MB;
	}
	else if (remaining >= 48u * MB)
	{
		// Old 3DS 64 MiB (.3dsx) or 80 MiB (.cia): 14 MiB linear heap leaves
		// ~44-60 MiB for the process heap while comfortably holding picaGL
		// buffers (~1.5 MiB), vertex arrays (~0.7 MiB), NDSP, and GPU textures.
		linear = 14u * MB;
	}
	else
	{
		linear = (remaining / 4u) & ~0xFFFu;
		if (linear < 8u * MB && remaining > 16u * MB)
			linear = 8u * MB;
	}

	if (linear >= remaining)
		linear = (remaining / 2u) & ~0xFFFu;

	u32 heap = (remaining - linear) & ~0xFFFu;
	if (heap > 96u * MB)
		heap = 96u * MB;

	__ctru_heap_size = heap;
	__ctru_linear_heap_size = linear;

	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0,
			__ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);

	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0,
			__ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
			MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);

	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

	fake_heap_start = (char *)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

// Last values read from the kernel's SVC access control mask. They are only
// used to tell whether the write below actually landed, never as the address.
static unsigned int s1, s2, s3, s0;

// Access control mask that allows every SVC.
static const unsigned int kSvcAccessControlEnabled[4] =
{
	0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0x3FFFFFFF
};

//-----------------------------------------------------------------------------
// Internal function that enables all services. This must be run via
// svcBackdoor or svcCustomBackdoor.
//-----------------------------------------------------------------------------
static Result ctrEnableAllServices(void)
{
	__asm__ volatile("cpsid aif");

	unsigned int *svc_access_control = *(*(unsigned int***)0xFFFF9000 + 0x22) - 0x6;

	s0 = svc_access_control[0];
	s1 = svc_access_control[1];
	s2 = svc_access_control[2];
	s3 = svc_access_control[3];

	svc_access_control[0] = kSvcAccessControlEnabled[0];
	svc_access_control[1] = kSvcAccessControlEnabled[1];
	svc_access_control[2] = kSvcAccessControlEnabled[2];
	svc_access_control[3] = kSvcAccessControlEnabled[3];

	return 0;
}

//-----------------------------------------------------------------------------
// svcCustomBackdoor (0x80) is Rosalina's variant of svcBackdoor. It does not
// borrow the interrupted thread's stack, so it stays valid if an interrupt
// lands while the callback is running. libctru has no wrapper for it.
//-----------------------------------------------------------------------------
static Result ctrCustomBackdoor(Result (*callback)(void))
{
	register Result (*r0)(void) asm("r0") = callback;
	__asm__ volatile("svc 0x80" : "+r"(r0) : : "r1", "r2", "r3", "memory");
	return (Result)(uintptr_t)r0;
}

//-----------------------------------------------------------------------------
// True when the mask holds the values written by the previous call.
//-----------------------------------------------------------------------------
static bool ctrAccessControlEnabled(void)
{
	return s0 == kSvcAccessControlEnabled[0] &&
		s1 == kSvcAccessControlEnabled[1] &&
		s2 == kSvcAccessControlEnabled[2] &&
		s3 == kSvcAccessControlEnabled[3];
}

//-----------------------------------------------------------------------------
// Runs the callback twice through the requested backdoor. The first call
// records the previous mask, the second one reads back what the first wrote,
// which proves the write reached the kernel instead of stray memory.
//-----------------------------------------------------------------------------
static bool ctrTryEnableAllServices(bool custom)
{
	s0 = s1 = s2 = s3 = 0;

	for (int i = 0; i < 2; ++i)
	{
		Result result = custom ? ctrCustomBackdoor(ctrEnableAllServices)
		                       : svcBackdoor(ctrEnableAllServices);
		if (R_FAILED(result))
			return false;
	}

	return ctrAccessControlEnabled();
}

//-----------------------------------------------------------------------------
// Sets the permissions for memory.
// You must ensure that buffer and size are 0x1000-aligned.
//-----------------------------------------------------------------------------
int _SetMemoryPermission(void *buffer, int size, int permission)
{
	Handle currentHandle;
	Result result = svcDuplicateHandle(&currentHandle, 0xFFFF8001);
	if (R_FAILED(result))
		return result;

	result = svcControlProcessMemory(currentHandle, (u32)(uintptr_t)buffer, 0, (u32)size, MEMOP_PROT, (u32)permission);
	svcCloseHandle(currentHandle);

	return result;
}

//-----------------------------------------------------------------------------
// Initializes the hack to gain kernel access to all services.
// The one that we really are interested is actually the
// svcControlProcessMemory, because once we have kernel access, we can
// grant read/write/execute access to memory blocks, and which means we
// can do dynamic recompilation.
//
// Newer firmware and CFW already grant those SVCs to homebrew, and the
// kernel layout this hack writes to is not guaranteed, so the caller must
// treat a failure here as "not verified" rather than "dynarec unusable".
// _ProbeExecutableMemory() is the authoritative check.
//-----------------------------------------------------------------------------
int _InitializeSvcHack(void)
{
		// svcBackdoor is the historic path and the one Luma3DS has always
		// patched in; try it first, then Rosalina's safer variant.
		if (ctrTryEnableAllServices(false))
			return 1;
		if (ctrTryEnableAllServices(true))
			return 1;

		return 0;
}

//-----------------------------------------------------------------------------
// Reports whether the process may use the SVCs the dynarec needs. SystemInfo
// type 0x10000 is a Luma3DS extension: it succeeds on Luma3DS and fails on
// stock firmware and on emulators. Purely informational.
//-----------------------------------------------------------------------------
int _DetectSvcAccess(void)
{
	s64 out = 0;
	return R_SUCCEEDED(svcGetSystemInfo(&out, 0x10000, 0)) ? 1 : 0;
}

//-----------------------------------------------------------------------------
// Reports whether we are running under a PC emulator. SystemInfo type 0x20000
// is answered by Citra (1) and Azahar (2); real consoles return an error.
// Those emulators do not implement the kernel calls the cache flush needs, so
// freshly written code goes stale and the dynarec crashes there.
//-----------------------------------------------------------------------------
int _RunningInEmulator(void)
{
	s64 out = 0;
	return (R_SUCCEEDED(svcGetSystemInfo(&out, 0x20000, 0)) && out > 0) ? 1 : 0;
}

//-----------------------------------------------------------------------------
// Allocates a single page and tries to make it executable, then releases it.
// Returns 0 when the dynarec can own executable memory, a negative errno-like
// value when allocation failed, or the failing Result otherwise.
//-----------------------------------------------------------------------------
int _ProbeExecutableMemory(void)
{
	void *page = memalign(0x1000, 0x1000);
	if (page == NULL)
		return -1;

	int result = _SetMemoryPermission(page, 0x1000, 7);
	free(page);

	return result;
}
