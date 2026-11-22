.arm

//The top two defines needs to be adjusted depending on how gCPUState struct is formated in CPU.h!! //Corn
//
#define _C0_Count	(0x100 + 9 * 4)	//CPU_Control_base + 9*8(64bit regs) or 9*4(32bit regs)
#define _AuxBase	0x280	//Base pointer to Aux regs
#define _CurrentPC	(_AuxBase + 0x00)
#define _TargetPC	(_AuxBase + 0x04)
#define _Delay		(_AuxBase + 0x08)
#define _StuffToDo	(_AuxBase + 0x0c)
#define _MultLo		(_AuxBase + 0x10)
#define _MultHi		(_AuxBase + 0x18)
#define _Temp1		(_AuxBase + 0x20)
#define _Temp2		(_AuxBase + 0x24)
#define _Temp3		(_AuxBase + 0x28)
#define _Temp4		(_AuxBase + 0x2C)
#define _Events		(_AuxBase + 0x30)

.extern CPU_UpdateCounter
.extern CPU_HANDLE_COUNT_INTERRUPT
.extern g_MemoryLookupTableRead
.extern g_MemoryLookupTableWrite
.extern gIndirectJumpTable
.extern HandleException_extern
.extern Write32BitsForDynaRec
.extern Write16BitsForDynaRec
.extern Write8BitsForDynaRec
.extern __aeabi_idivmod
.extern __aeabi_uidivmod
	
.align 4
.global _EnterDynaRec
.global _DirectExitCheckNoDelay
.global _DirectExitCheckDelay
.global _DirectExitCheckCheckCount
.global _IndirectExitCheck
.global _DynaRec_DIV
.global _DynaRec_DIVU

	.global _ReadFastPRead_u8
	.global _ReadFastPRead_s8
	.global _ReadFastPRead_u16
	.global _ReadFastPRead_s16
	.global _ReadFastPRead_u32

	.global _ReadBitsDirect_u8
	.global _ReadBitsDirect_s8
	.global _ReadBitsDirect_u16
	.global _ReadBitsDirect_s16
	.global _ReadBitsDirect_u32

	.global _ReadBitsDirectBD_u8
	.global _ReadBitsDirectBD_s8
	.global _ReadBitsDirectBD_u16
	.global _ReadBitsDirectBD_s16
	.global _ReadBitsDirectBD_u32

	.global _WriteBitsDirect_u32
	.global _WriteBitsDirect_u16
	.global _WriteBitsDirect_u8
	.global _WriteBitsDirectBD_u32
	.global _WriteBitsDirectBD_u16
	.global _WriteBitsDirectBD_u8
	
	
.type _EnterDynaRec, %function
.type _DirectExitCheckNoDelay, %function
.type _DirectExitCheckDelay, %function
.type _DirectExitCheckCheckCount, %function
.type _IndirectExitCheck, %function

_DirectExitCheckNoDelay:
    ldr r4, [r12, #_C0_Count]	// COUNT register
    ldr r5, [r12, #_Events]		// Events[0].mCount
	
	add r4, r4, r0				// COUNT + ops_executed
	str r4, [r12, #_C0_Count]	// COUNT = COUNT + ops_executed
	
	str r1, [r12, #_CurrentPC]	// Current PC
	mov r1, #0
	str r1, [r12, #_Delay]		// Delay = NO_DELAY
	
	subs r5, r5, r0				// Events[0].mCount - ops_executed
	str r5, [r12, #_Events]
	ble	_DirectExitCheckCheckCount
	bx  lr

_DirectExitCheckDelay:
    ldr r4, [r12, #_C0_Count]	// COUNT register
    ldr r5, [r12, #_Events]		// Events[0].mCount
	
	add r4, r4, r0				// COUNT + ops_executed
	str r4, [r12, #_C0_Count]	// COUNT = COUNT + ops_executed
	
	str r1, [r12, #_CurrentPC]	// Current PC
	str r2, [r12, #_TargetPC]	// Target PC
	mov r1, #1					// EXEC_DELAY
	str r1, [r12, #_Delay]		// Delay = EXEC_DELAY
	
	subs r5, r5, r0				// Events[0].mCount - ops_executed
	str r5, [r12, #_Events]
	ble	_DirectExitCheckCheckCount
	bx  lr

#######################################################################################
#	Utility routine for _DirectExitCheckXX.
#
_DirectExitCheckCheckCount:
	mov		r4, lr		// Keep track of return addresss
	mov		r5, r12
	bl		CPU_HANDLE_COUNT_INTERRUPT
	mov		lr,  r4
	mov		r12, r5
	ldr		r0, [r12, #_StuffToDo]		// StuffToDo
	cmp		r0, #0
	popne	{r4-r12, pc}		// Exit the DynaRec
	bx		lr					// Return back to caller

#######################################################################################
#	Update counter. If StuffToDo flags is clear on return,
#	r0 - instructions executed
#	r1 - CIndirectExitMap pointer
#	r2 - exit pc (exit delay is always NO_DELAY)
_IndirectExitCheck:
	ldr		r3, [r12, #_C0_Count]
	ldr		r4, [r12, #_Events]
	add		r3, r3, r0
	subs	r4, r4, r0
	str		r3, [r12, #_C0_Count]
	str		r4, [r12, #_Events]
	mov		r4, r1					// p_map
	mov		r5, r2					// exit_pc
	mov		r6, r12					// gCPUState
	ble		_IndirectExitHandleEvent

	ldr		r0, [r6, #_StuffToDo]
	cmp		r0, #0
	bne		_IndirectExitToC

_IndirectExitLookup:
	// 1) Per-callsite MRU slot 0 & slot 1 (catches tight polymorphic / alternating callers in 3-6 cycles)
	ldr		r1, [r4, #4]
	cmp		r1, r5
	bne		0f
	ldr		r0, [r4, #8]
	cmp		r0, #0
	beq		0f
	mov		r12, r6
	bx		r0
0:
	ldr		r1, [r4, #12]
	cmp		r1, r5
	bne		1f
	ldr		r0, [r4, #16]
	cmp		r0, #0
	beq		1f
	mov		r12, r6
	bx		r0
1:
	// 2) Global 8192-entry direct-mapped native target table (Ari64 mini_ht):
	//    Resolves JR $ra, jump tables, and OSHLE returns (sinf/cosf/bcopy/guMtx*) in ~8 cycles
	//    with zero C++ call overhead across up to 8192 distinct return addresses.
	lsr		r1, r5, #2
	eor		r1, r1, r5, lsr #15
	lsl		r1, r1, #19
	lsr		r1, r1, #19
	ldr		r3, =gIndirectJumpTable
	add		r3, r3, r1, lsl #3
	ldmia	r3, {r0, r1}			// r0 = entry.addr ([r3, #0]), r1 = entry.target ([r3, #4])
	cmp		r0, r5
	bne		2f
	cmp		r1, #0
	beq		2f
	mov		r12, r6
	bx		r1
2:
	mov		r0, r4					// p_map
	mov		r1, r5					// exit_pc
	bl		IndirectExitMap_Lookup

	# r0 holds pointer to indirect target. If it's 0, it means it's not compiled yet
	cmp		r0, #0
	beq		_IndirectExitToC
	mov		r12, r6					// Restore the CPUState pointer
	bx		r0						// branch to the looked up fragment

_IndirectExitHandleEvent:
	str		r5, [r6, #_CurrentPC]
	mov		r0, #0
	str		r0, [r6, #_Delay]
	bl		CPU_HANDLE_COUNT_INTERRUPT
	ldr		r0, [r6, #_StuffToDo]
	cmp		r0, #0
	popne	{r4-r12, pc}			// Exit the DynaRec
	b		_IndirectExitLookup

_IndirectExitToC:
	str		r5, [r6, #_CurrentPC]
	mov		r0, #0
	str		r0, [r6, #_Delay]
	pop		{r4-r12, pc}			// Exit the DynaRec

_ReturnFromDynaRecAndHandleException:
	bl    HandleException_extern
	pop   {r4-r12,pc}
	
#######################################################################################
# Zero-flush fast pRead lookup for Buffer B dynamic loads (0x7F/0x70 GoldenEye, 0xA0 KSEG1):
#   Input:  r0 = pre-swizzled N64 address
#   Output: Z=0 (NE) on hit with r0 = loaded value; Z=1 (EQ) on miss with r0 preserved.
#   Clobbers ONLY r2, r3 (preserves ALL cached GPRs r4-r12 and ALL VFP regs s0-s31!).
.macro READ_FAST_PREAD function, load_instruction
.type \function, %function
\function:
	lsr		r2, r0, #0x12
	ldr		r3, =g_MemoryLookupTableRead
	ldr		r3, [r3, r2, lsl #3]	// m.pRead
	cmp		r3, #0
	bxeq	lr						// Z=1 -> miss, r0 unchanged
	add		r0, r3, r0
	\load_instruction	r0, [r0]
	tst		r3, r3					// r3 != 0 -> Z=0 (hit)
	bx		lr
.endm

	READ_FAST_PREAD _ReadFastPRead_u8,  ldrb
	READ_FAST_PREAD _ReadFastPRead_s8,  ldrsb
	READ_FAST_PREAD _ReadFastPRead_u16, ldrh
	READ_FAST_PREAD _ReadFastPRead_s16, ldrsh
	READ_FAST_PREAD _ReadFastPRead_u32, ldr

.macro READ_BITS	function, load_instruction
\function:
	// Fast direct lookup via g_MemoryLookupTableRead[addr >> 18].pRead
	// (handles GoldenEye 0x7F/0x70 TLB-hacked ROM/RAM, KSEG1 RDRAM 0xA0, etc.
	// with zero C++ call, zero TLB miss, and matching Memory.h ReadAddress()).
	lsr		r2, r0, #0x12
	ldr		r3, =g_MemoryLookupTableRead
	add		r2, r3, r2, lsl #3 // * 8 to index the two pointer struct
	ldr		r3, [r2, #0]	// m.pRead
	cmp		r3, #0
	beq		1f
	add		r0, r3, r0
	\load_instruction	r0, [r0]
	bx		lr
1:
	ldr		r2, [r2,#4]	//offset 4 to get the second pointer (m.ReadFunc)
	
	str		r1, [r12,#_CurrentPC] 	// CurrentPC
	
	// execute memread function
	mov		r5, r12
	mov		r6, lr
	blx		r2
	mov		lr, r6
	mov		r12, r5
	
	// check exceptions
	ldr     r1, [r12, #_StuffToDo]
	cmp     r1, #0
	bne     _ReturnFromDynaRecAndHandleException
	
	//execute the load
	\load_instruction	r0, [r0]		// e.g. lbu, lhu, lw etc
	
	// return
	bx lr
.endm

.macro READ_BITS_BD	function, load_instruction
\function:
	// Fast direct lookup via g_MemoryLookupTableRead[addr >> 18].pRead
	lsr		r2, r0, #0x12
	ldr		r3, =g_MemoryLookupTableRead
	add		r2, r3, r2, lsl #3 // * 8 to index the two pointer struct
	ldr		r3, [r2, #0]	// m.pRead
	cmp		r3, #0
	beq		1f
	add		r0, r3, r0
	\load_instruction	r0, [r0]
	bx		lr
1:
	ldr		r2, [r2,#4]	//offset 4 to get the second pointer (m.ReadFunc)
	
	str		r1, [r12,#_CurrentPC] 	// CurrentPC
	
	mov		r3, #1 // EXEC_DELAY
	str		r3, [r12, #_Delay]
	
	// execute memread function
	mov		r5, r12
	mov		r6, lr
	blx		r2
	mov		lr, r6
	mov		r12, r5
	
	// check exceptions
	ldr     r1, [r12, #_StuffToDo]
	cmp     r1, #0
	bne     _ReturnFromDynaRecAndHandleException

	mov     r1, #0
	str     r1, [r12, #_Delay]
	
	// execute the load
	\load_instruction	r0, [r0]		// e.g. lbu, lhu, lw etc
	
	// return
	bx lr
.endm

	READ_BITS _ReadBitsDirect_u8, ldrb
	READ_BITS _ReadBitsDirect_s8, ldrsb
	READ_BITS _ReadBitsDirect_u16, ldrh
	READ_BITS _ReadBitsDirect_s16, ldrsh
	READ_BITS _ReadBitsDirect_u32, ldr
	
	READ_BITS_BD _ReadBitsDirectBD_u8, ldrb
	READ_BITS_BD _ReadBitsDirectBD_s8, ldrsb
	READ_BITS_BD _ReadBitsDirectBD_u16, ldrh
	READ_BITS_BD _ReadBitsDirectBD_s16, ldrsh
	READ_BITS_BD _ReadBitsDirectBD_u32, ldr

.macro WRITE_BITS	function, store_func
\function:
	str		r2, [r12,#_CurrentPC] 	// CurrentPC
	
	// do the write 
	mov  r5, r12
	mov  r6, lr
	bl		\store_func
	mov  lr, r6
	mov	  r12, r5
	
	// check exceptions
	ldr     r1, [r12, #_StuffToDo]
	cmp     r1, #0
	bne     _ReturnFromDynaRecAndHandleException

	bx lr
.endm

.macro WRITE_BITS_BD	function, store_func
\function:
	str		r2, [r12,#_CurrentPC] 	// CurrentPC
	
	mov		r2, #1 // EXEC_DELAY
	str		r2, [r12, #_Delay]
	
	// do the write 
	mov  r5, r12
	mov  r6, lr
	bl		\store_func
	mov  lr, r6
	mov	  r12, r5
	
	
	// check exceptions
	ldr     r1, [r12, #_StuffToDo]
	cmp     r1, #0
	bne     _ReturnFromDynaRecAndHandleException
	
	mov     r0, #0
	str     r0, [r12, #_Delay]
	
	bx		lr
.endm



	WRITE_BITS _WriteBitsDirect_u8, Write8BitsForDynaRec
	WRITE_BITS _WriteBitsDirect_u16, Write16BitsForDynaRec
	WRITE_BITS _WriteBitsDirect_u32, Write32BitsForDynaRec

	WRITE_BITS_BD _WriteBitsDirectBD_u8, Write8BitsForDynaRec
	WRITE_BITS_BD _WriteBitsDirectBD_u16, Write16BitsForDynaRec
	WRITE_BITS_BD _WriteBitsDirectBD_u32, Write32BitsForDynaRec
	
_EnterDynaRec:
    push {r4-r12, lr}
    mov r12, r1
    mov r10, r2
	mov r9,  r3
    bx  r0

#######################################################################################
# Fast native ARMv6 DIV / DIVU helpers:
#   r0 = dividend, r1 = divisor, r12 = &gCPUState
#   Preserves r4-r11 and ALL VFP registers (s0-s31), avoiding full register cache flush!
.type _DynaRec_DIV, %function
_DynaRec_DIV:
	cmp		r1, #0
	bxeq	lr
	// Guard INT32_MIN / -1 overflow (MIPS R4300i: LO = INT32_MIN, HI = 0)
	cmn		r1, #1
	bne		1f
	cmp		r0, #0x80000000
	bne		1f
	mov		r2, r0, asr #31
	str		r0, [r12, #_MultLo]
	str		r2, [r12, #_MultLo + 4]
	mov		r2, #0
	str		r2, [r12, #_MultHi]
	str		r2, [r12, #_MultHi + 4]
	bx		lr
1:
	push	{r4, lr}
	mov		r4, r12
	bl		__aeabi_idivmod
	mov		r2, r0, asr #31
	mov		r3, r1, asr #31
	str		r0, [r4, #_MultLo]
	str		r2, [r4, #_MultLo + 4]
	str		r1, [r4, #_MultHi]
	str		r3, [r4, #_MultHi + 4]
	mov		r12, r4
	pop		{r4, pc}

.type _DynaRec_DIVU, %function
_DynaRec_DIVU:
	cmp		r1, #0
	bxeq	lr
	push	{r4, lr}
	mov		r4, r12
	bl		__aeabi_uidivmod
	mov		r2, r0, asr #31
	mov		r3, r1, asr #31
	str		r0, [r4, #_MultLo]
	str		r2, [r4, #_MultLo + 4]
	str		r1, [r4, #_MultHi]
	str		r3, [r4, #_MultHi + 4]
	mov		r12, r4
	pop		{r4, pc}
