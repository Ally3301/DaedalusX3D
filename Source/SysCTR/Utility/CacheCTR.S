.arm

.bss
.align 2
_cache_flush_start:
	.word 0
_cache_flush_end:
	.word 0

.text
.global _InvalidateAndFlushCaches
.global _InvalidateAndFlushCacheRange

_ClearCacheKernel:
	cpsid aif
	mov r0, #0
	mcr p15, 0, r0, c7, c10, 0	@ Clean entire data cache
	mcr p15, 0, r0, c7, c10, 5	@ Data Memory Barrier
	mcr p15, 0, r0, c7, c5, 0	@ Invalidate entire instruction cache / Flush BTB
	mcr p15, 0, r0, c7, c10, 4	@ Data Sync Barrier
	mcr p15, 0, r0, c7, c5, 4	@ Flush Prefetch Buffer
	bx lr

_ClearCacheRangeKernel:
	cpsid aif
	ldr r3, =_cache_flush_start
	ldr r0, [r3]
	ldr r1, [r3, #4]
	mov r2, #0
	bic r0, r0, #31				@ Align start to 32-byte L1 cache line
	mov r3, r0
1:
	mcr p15, 0, r3, c7, c10, 1	@ Clean D-cache line by MVA
	add r3, r3, #32
	cmp r3, r1
	blo 1b
	mcr p15, 0, r2, c7, c10, 4	@ Data Sync Barrier
2:
	mcr p15, 0, r0, c7, c5, 1	@ Invalidate I-cache line by MVA
	add r0, r0, #32
	cmp r0, r1
	blo 2b
	mcr p15, 0, r2, c7, c5, 6	@ Flush Branch Target Cache (BTB)
	mcr p15, 0, r2, c7, c10, 4	@ Data Sync Barrier
	mcr p15, 0, r2, c7, c5, 4	@ Flush Prefetch Buffer
	bx lr

_InvalidateAndFlushCaches:
	ldr r0, =_ClearCacheKernel
	svc 0x80					@ svcCustomBackdoor
	bx lr

@ void _InvalidateAndFlushCacheRange(const void *start, unsigned int size)
_InvalidateAndFlushCacheRange:
	cmp r1, #0
	bxeq lr
	cmp r1, #16384				@ >= L1 cache size: full flush is faster
	bhs _InvalidateAndFlushCaches
	add r1, r0, r1
	ldr r2, =_cache_flush_start
	str r0, [r2]
	str r1, [r2, #4]
	ldr r0, =_ClearCacheRangeKernel
	svc 0x80					@ svcCustomBackdoor
	bx lr
