#include "stdafx.h"
#include "PicaTEVCombiner.h"

#include "HLEGraphics/RDP.h"
#include "SysCTR/Graphics/Citro3DContext.h"
#include <cstring>

namespace
{
	enum EN64Input : u8
	{
		IN_COMBINED = 0,
		IN_TEXEL0,
		IN_TEXEL1,
		IN_PRIMITIVE,
		IN_SHADE,
		IN_ENV,
		IN_1,
		IN_0,
		IN_COMBINED_ALPHA,
		IN_TEXEL0_ALPHA,
		IN_TEXEL1_ALPHA,
		IN_PRIMITIVE_ALPHA,
		IN_SHADE_ALPHA,
		IN_ENV_ALPHA,
		IN_PRIM_LOD_FRAC,
		IN_HALF,
		IN_UNSUPPORTED,
	};

	// Exact N64 RDP hardware input decode tables (per official RDP specification / GLideN64 / ctr64).
	static const EN64Input kRgbA[16] = {
		IN_COMBINED, IN_TEXEL0, IN_TEXEL1, IN_PRIMITIVE,
		IN_SHADE,    IN_ENV,    IN_1,      IN_HALF, // 7 = NOISE (approximated as 0.5)
		IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0
	};

	static const EN64Input kRgbB[16] = {
		IN_COMBINED, IN_TEXEL0, IN_TEXEL1, IN_PRIMITIVE,
		IN_SHADE,    IN_ENV,    IN_HALF,   IN_HALF, // 6=CENTER, 7=K4
		IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0
	};

	static const EN64Input kRgbC[32] = {
		IN_COMBINED,       IN_TEXEL0,        IN_TEXEL1,         IN_PRIMITIVE,
		IN_SHADE,          IN_ENV,           IN_HALF,           IN_COMBINED_ALPHA, // 6=SCALE
		IN_TEXEL0_ALPHA,   IN_TEXEL1_ALPHA,  IN_PRIMITIVE_ALPHA,IN_SHADE_ALPHA,
		IN_ENV_ALPHA,      IN_0,             IN_PRIM_LOD_FRAC,  IN_HALF,           // 13=LOD_FRAC (0), 15=K5
		IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0,
		IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0, IN_0
	};

	static const EN64Input kRgbD[8] = {
		IN_COMBINED, IN_TEXEL0, IN_TEXEL1, IN_PRIMITIVE,
		IN_SHADE,    IN_ENV,    IN_1,      IN_0
	};

	static const EN64Input kAlphaABD[8] = {
		IN_COMBINED_ALPHA, IN_TEXEL0_ALPHA, IN_TEXEL1_ALPHA, IN_PRIMITIVE_ALPHA,
		IN_SHADE_ALPHA,    IN_ENV_ALPHA,    IN_1,            IN_0
	};

	static const EN64Input kAlphaC[8] = {
		IN_0,            IN_TEXEL0_ALPHA, IN_TEXEL1_ALPHA, IN_PRIMITIVE_ALPHA,
		IN_SHADE_ALPHA,  IN_ENV_ALPHA,    IN_PRIM_LOD_FRAC, IN_0 // 0=LOD_FRAC (0)
	};

	static inline bool IsConstInput(EN64Input in)
	{
		switch (in)
		{
		case IN_0:
		case IN_1:
		case IN_HALF:
		case IN_PRIMITIVE:
		case IN_PRIMITIVE_ALPHA:
		case IN_ENV:
		case IN_ENV_ALPHA:
		case IN_PRIM_LOD_FRAC:
			return true;
		default:
			return false;
		}
	}

	static inline EPicaConstSrc InputToConstRgb(EN64Input in)
	{
		switch (in)
		{
		case IN_0:               return PICA_CONST_ZERO;
		case IN_1:               return PICA_CONST_ONE;
		case IN_HALF:            return PICA_CONST_HALF;
		case IN_PRIMITIVE:       return PICA_CONST_PRIM_RGB;
		case IN_PRIMITIVE_ALPHA: return PICA_CONST_PRIM_ALPHA;
		case IN_ENV:             return PICA_CONST_ENV_RGB;
		case IN_ENV_ALPHA:       return PICA_CONST_ENV_ALPHA;
		case IN_PRIM_LOD_FRAC:   return PICA_CONST_PRIM_LOD;
		default:                 return PICA_CONST_NONE;
		}
	}

	static inline EPicaConstSrc InputToConstAlpha(EN64Input in)
	{
		switch (in)
		{
		case IN_0:               return PICA_CONST_ZERO;
		case IN_1:               return PICA_CONST_ONE;
		case IN_HALF:            return PICA_CONST_HALF;
		case IN_PRIMITIVE:
		case IN_PRIMITIVE_ALPHA: return PICA_CONST_PRIM_ALPHA;
		case IN_ENV:
		case IN_ENV_ALPHA:       return PICA_CONST_ENV_ALPHA;
		case IN_PRIM_LOD_FRAC:   return PICA_CONST_PRIM_LOD;
		default:                 return PICA_CONST_NONE;
		}
	}

	// Represents a single channel's (RGB or Alpha) compiled sub-stage list (1..3 stages per cycle).
	struct SChanOp
	{
		GPU_TEVSRC    src[3];
		u8            op[3]; // GPU_TEVOP_RGB or GPU_TEVOP_ALPHA
		GPU_COMBINEFUNC func;
		EPicaConstSrc constSrc;
	};

	struct SChanSeq
	{
		SChanOp ops[3];
		int     count;
		bool    usesCombined; // references Cycle 0 output (COMBINED / COMBINED_ALPHA)
		bool    usesCombinedAfterFirst; // references Cycle 0 output in ops[1] or ops[2]
		bool    usesTex0;
		bool    usesTex1;
	};

	static inline void RecordTexUsage(EN64Input in, SChanSeq & seq)
	{
		if (in == IN_TEXEL0 || in == IN_TEXEL0_ALPHA) seq.usesTex0 = true;
		if (in == IN_TEXEL1 || in == IN_TEXEL1_ALPHA) seq.usesTex1 = true;
	}

	static inline bool IsCombinedInput(EN64Input in)
	{
		return in == IN_COMBINED || in == IN_COMBINED_ALPHA;
	}

	// Maps an N64 input to PICA200 (GPU_TEVSRC, op) for RGB or Alpha channel.
	// If `oneMinus` is true, inverts the operand (1 - x).
	// `stageConst` tracks the stage's constant color; returns false on constant conflict.
	static bool MapOperand(
		EN64Input in, bool isAlpha, bool oneMinus, bool combinedInBuffer,
		EPicaConstSrc & stageConst, GPU_TEVSRC & outSrc, u8 & outOp)
	{
		if (in == IN_UNSUPPORTED)
			return false;

		if (IsConstInput(in))
		{
			EPicaConstSrc need = isAlpha ? InputToConstAlpha(in) : InputToConstRgb(in);
			bool invert = oneMinus;

			// ZERO and ONE are inverses of each other, so they can share a single constant slot!
			if (stageConst == PICA_CONST_NONE)
			{
				stageConst = need;
			}
			else if (stageConst == need)
			{
				// exact match
			}
			else if (stageConst == PICA_CONST_ONE && need == PICA_CONST_ZERO)
			{
				invert = !invert;
			}
			else if (stageConst == PICA_CONST_ZERO && need == PICA_CONST_ONE)
			{
				invert = !invert;
			}
			else
			{
				// Two distinct dynamic constants needed in the same stage -> conflict
				return false;
			}

			outSrc = GPU_CONSTANT;
			if (isAlpha)
				outOp = invert ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
			else
				outOp = invert ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR;
			return true;
		}

		bool readAlphaInRgb = false;
		switch (in)
		{
		case IN_COMBINED:
			outSrc = combinedInBuffer ? GPU_PREVIOUS_BUFFER : GPU_PREVIOUS;
			break;
		case IN_COMBINED_ALPHA:
			outSrc = combinedInBuffer ? GPU_PREVIOUS_BUFFER : GPU_PREVIOUS;
			readAlphaInRgb = !isAlpha;
			break;
		case IN_TEXEL0:
			outSrc = GPU_TEXTURE0;
			break;
		case IN_TEXEL0_ALPHA:
			outSrc = GPU_TEXTURE0;
			readAlphaInRgb = !isAlpha;
			break;
		case IN_TEXEL1:
			outSrc = GPU_TEXTURE1;
			break;
		case IN_TEXEL1_ALPHA:
			outSrc = GPU_TEXTURE1;
			readAlphaInRgb = !isAlpha;
			break;
		case IN_SHADE:
			outSrc = GPU_PRIMARY_COLOR;
			break;
		case IN_SHADE_ALPHA:
			outSrc = GPU_PRIMARY_COLOR;
			readAlphaInRgb = !isAlpha;
			break;
		default:
			return false;
		}

		if (isAlpha)
		{
			outOp = oneMinus ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
		}
		else if (readAlphaInRgb)
		{
			outOp = oneMinus ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA;
		}
		else
		{
			outOp = oneMinus ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR;
		}
		return true;
	}

	static SChanOp MakePassthrough(bool isAlpha)
	{
		SChanOp op;
		op.src[0] = GPU_PREVIOUS;
		op.src[1] = GPU_PREVIOUS;
		op.src[2] = GPU_PREVIOUS;
		const u8 defOp = isAlpha ? (u8)GPU_TEVOP_A_SRC_ALPHA : (u8)GPU_TEVOP_RGB_SRC_COLOR;
		op.op[0] = defOp;
		op.op[1] = defOp;
		op.op[2] = defOp;
		op.func = GPU_REPLACE;
		op.constSrc = PICA_CONST_NONE;
		return op;
	}

	// Compiles one cycle's equation (a - b) * c + d for a single channel (RGB or Alpha).
	static bool CompileCycleChannel(
		EN64Input a, EN64Input b, EN64Input c, EN64Input d,
		bool isAlpha, bool isCycle2, SChanSeq & seq)
	{
		seq = {};

		// Cycle 0 cannot read COMBINED (evaluates to 0 in 1-cycle N64 hardware / GLideN64).
		// Also in 1-cycle mode, N64 hardware redirects TEXEL1 -> TEXEL0 (correctFirstStageParam).
		if (!isCycle2)
		{
			if (IsCombinedInput(a)) a = IN_0;
			if (IsCombinedInput(b)) b = IN_0;
			if (IsCombinedInput(c)) c = IN_0;
			if (IsCombinedInput(d)) d = IN_0;
		}

		// Algebraic simplifications (always exact on N64 RDP):
		// 1) If a == b or c == 0 -> output = d
		if (a == b || c == IN_0)
		{
			if (d == IN_UNSUPPORTED) return false;
			RecordTexUsage(d, seq);
			if (IsCombinedInput(d))
			{
				seq.usesCombined = true;
				// Exact passthrough of Cycle 0 output!
				if ((isAlpha && d == IN_COMBINED_ALPHA) || (!isAlpha && d == IN_COMBINED))
				{
					seq.count = 0; // caller emits passthrough only if needed
					return true;
				}
			}
			SChanOp op = MakePassthrough(isAlpha);
			if (!MapOperand(d, isAlpha, false, false, op.constSrc, op.src[0], op.op[0]))
				return false;
			op.func = GPU_REPLACE;
			seq.ops[0] = op;
			seq.count = 1;
			return true;
		}

		// Check unsupported inputs remaining in active equation
		if (a == IN_UNSUPPORTED || b == IN_UNSUPPORTED || c == IN_UNSUPPORTED || d == IN_UNSUPPORTED)
			return false;

		RecordTexUsage(a, seq);
		RecordTexUsage(b, seq);
		RecordTexUsage(c, seq);
		RecordTexUsage(d, seq);

		// Helper to emit a 1-stage binary/ternary op, automatically splitting with a
		// constant pre-load stage if two distinct dynamic constants collide!
		auto EmitOp3 = [&](GPU_COMBINEFUNC func,
			EN64Input i0, bool inv0,
			EN64Input i1, bool inv1,
			EN64Input i2, bool inv2,
			int arity) -> bool
		{
			const bool anyCombined = (arity >= 1 && IsCombinedInput(i0)) ||
			                         (arity >= 2 && IsCombinedInput(i1)) ||
			                         (arity >= 3 && IsCombinedInput(i2));
			if (anyCombined)
				seq.usesCombined = true;

			// Try direct 1-stage emission first.
			SChanOp op = MakePassthrough(isAlpha);
			op.func = func;
			bool ok = true;
			if (arity >= 1) ok = ok && MapOperand(i0, isAlpha, inv0, false, op.constSrc, op.src[0], op.op[0]);
			if (arity >= 2) ok = ok && MapOperand(i1, isAlpha, inv1, false, op.constSrc, op.src[1], op.op[1]);
			if (arity >= 3) ok = ok && MapOperand(i2, isAlpha, inv2, false, op.constSrc, op.src[2], op.op[2]);
			if (ok)
			{
				seq.ops[0] = op;
				seq.count = 1;
				return true;
			}

			// Constant conflict: pre-load one constant into GPU_PREVIOUS in Stage 0,
			// then reference GPU_PREVIOUS in Stage 1!
			// Pick the first constant operand to pre-load.
			EN64Input preloadIn = IN_UNSUPPORTED;
			if (arity >= 2 && IsConstInput(i1)) preloadIn = i1;
			else if (arity >= 3 && IsConstInput(i2)) preloadIn = i2;
			else if (arity >= 1 && IsConstInput(i0)) preloadIn = i0;
			if (preloadIn == IN_UNSUPPORTED)
				return false;

			SChanOp pre = MakePassthrough(isAlpha);
			pre.func = GPU_REPLACE;
			if (!MapOperand(preloadIn, isAlpha, false, false, pre.constSrc, pre.src[0], pre.op[0]))
				return false;

			// Stage 1 reads Cycle 0 COMBINED from GPU_PREVIOUS_BUFFER if needed!
			if (anyCombined)
				seq.usesCombinedAfterFirst = true;

			SChanOp mainOp = MakePassthrough(isAlpha);
			mainOp.func = func;
			auto mapWithPreload = [&](EN64Input in, bool inv, GPU_TEVSRC & dstSrc, u8 & dstOp) -> bool {
				if (in == preloadIn)
				{
					dstSrc = GPU_PREVIOUS;
					dstOp = isAlpha
						? (u8)(inv ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA)
						: (u8)(inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR);
					return true;
				}
				return MapOperand(in, isAlpha, inv, anyCombined, mainOp.constSrc, dstSrc, dstOp);
			};

			bool ok2 = true;
			if (arity >= 1) ok2 = ok2 && mapWithPreload(i0, inv0, mainOp.src[0], mainOp.op[0]);
			if (arity >= 2) ok2 = ok2 && mapWithPreload(i1, inv1, mainOp.src[1], mainOp.op[1]);
			if (arity >= 3) ok2 = ok2 && mapWithPreload(i2, inv2, mainOp.src[2], mainOp.op[2]);
			if (!ok2)
				return false;

			seq.ops[0] = pre;
			seq.ops[1] = mainOp;
			seq.count = 2;
			return true;
		};

		// 2) b == 0, d == 0: a * c
		if (b == IN_0 && d == IN_0)
		{
			if (c == IN_1)
				return EmitOp3(GPU_REPLACE, a, false, IN_0, false, IN_0, false, 1);
			if (a == IN_1)
				return EmitOp3(GPU_REPLACE, c, false, IN_0, false, IN_0, false, 1);
			return EmitOp3(GPU_MODULATE, a, false, c, false, IN_0, false, 2);
		}

		// 3) b == d: (a - b) * c + b = a * c + b * (1 - c) = INTERPOLATE(a, b, c)
		if (b == d)
		{
			if (a == IN_0)
				return EmitOp3(GPU_MODULATE, b, false, c, true, IN_0, false, 2);
			return EmitOp3(GPU_INTERPOLATE, a, false, b, false, c, false, 3);
		}

		// 4) b == 0, d != 0: a * c + d = MULTIPLY_ADD(a, c, d)
		if (b == IN_0)
		{
			if (a == IN_1)
				return EmitOp3(GPU_ADD, c, false, d, false, IN_0, false, 2);
			if (c == IN_1)
				return EmitOp3(GPU_ADD, a, false, d, false, IN_0, false, 2);
			return EmitOp3(GPU_MULTIPLY_ADD, a, false, c, false, d, false, 3);
		}

		// 4b) a == 1, b != 0: (1 - b) * c + d -> fold (1 - b) into hardware operand inversion!
		if (a == IN_1)
		{
			if (c == IN_1)
			{
				if (d == IN_0)
					return EmitOp3(GPU_REPLACE, b, true, IN_0, false, IN_0, false, 1);
				return EmitOp3(GPU_ADD, b, true, d, false, IN_0, false, 2);
			}
			if (d == IN_0)
				return EmitOp3(GPU_MODULATE, b, true, c, false, IN_0, false, 2);
			return EmitOp3(GPU_MULTIPLY_ADD, b, true, c, false, d, false, 3);
		}

		// 4c) a == 0, b != 0, d != 0: (0 - b) * c + d = d - b * c
		//     Avoids PICA200 TEV clamping (0 - b) to 0 in stage 0!
		if (a == IN_0 && d != IN_0)
		{
			if (c == IN_1)
				return EmitOp3(GPU_SUBTRACT, d, false, b, false, IN_0, false, 2);

			const bool cIn0 = IsCombinedInput(b) || IsCombinedInput(c);
			const bool cIn1 = IsCombinedInput(d);
			if (cIn0 || cIn1) seq.usesCombined = true;
			if (cIn1) seq.usesCombinedAfterFirst = true;

			SChanOp s0 = MakePassthrough(isAlpha);
			s0.func = GPU_MODULATE;
			if (!MapOperand(b, isAlpha, false, cIn1, s0.constSrc, s0.src[0], s0.op[0]) ||
			    !MapOperand(c, isAlpha, false, cIn1, s0.constSrc, s0.src[1], s0.op[1]))
				return false;

			SChanOp s1 = MakePassthrough(isAlpha);
			s1.func = GPU_SUBTRACT;
			if (!MapOperand(d, isAlpha, false, cIn1, s1.constSrc, s1.src[0], s1.op[0]))
				return false;
			s1.src[1] = GPU_PREVIOUS;
			s1.op[1] = isAlpha ? (u8)GPU_TEVOP_A_SRC_ALPHA : (u8)GPU_TEVOP_RGB_SRC_COLOR;

			seq.ops[0] = s0;
			seq.ops[1] = s1;
			seq.count = 2;
			return true;
		}

		// 5) c == 1: (a - b) + d = (a + d) - b (add first so (a - b) never clamps to 0 when a < b!)
		if (c == IN_1)
		{
			if (d == IN_0)
				return EmitOp3(GPU_SUBTRACT, a, false, b, false, IN_0, false, 2);
			if (d == IN_1)
				return EmitOp3(GPU_ADD, b, true, a, false, IN_0, false, 2);

			// 2 stages: s0 = a + d; out = s0 - b
			const bool cIn0 = IsCombinedInput(a) || IsCombinedInput(d);
			const bool cIn1 = IsCombinedInput(b);
			if (cIn0 || cIn1) seq.usesCombined = true;
			if (cIn1) seq.usesCombinedAfterFirst = true;

			SChanOp s0 = MakePassthrough(isAlpha);
			s0.func = GPU_ADD;
			if (!MapOperand(a, isAlpha, false, cIn1, s0.constSrc, s0.src[0], s0.op[0]) ||
			    !MapOperand(d, isAlpha, false, cIn1, s0.constSrc, s0.src[1], s0.op[1]))
				return false;

			SChanOp s1 = MakePassthrough(isAlpha);
			s1.func = GPU_SUBTRACT;
			s1.src[0] = GPU_PREVIOUS;
			s1.op[0] = isAlpha ? (u8)GPU_TEVOP_A_SRC_ALPHA : (u8)GPU_TEVOP_RGB_SRC_COLOR;
			if (!MapOperand(b, isAlpha, false, cIn1, s1.constSrc, s1.src[1], s1.op[1]))
				return false;

			seq.ops[0] = s0;
			seq.ops[1] = s1;
			seq.count = 2;
			return true;
		}

		// 6) General case: (a - b) * c + d -> 2 stages:
		//    s0: a - b
		//    s1: PREV * c + d (MULTIPLY_ADD)
		{
			const bool cIn0 = IsCombinedInput(a) || IsCombinedInput(b);
			const bool cIn1 = IsCombinedInput(c) || IsCombinedInput(d);
			if (cIn0 || cIn1) seq.usesCombined = true;
			if (cIn1) seq.usesCombinedAfterFirst = true;

			SChanOp s0 = MakePassthrough(isAlpha);
			s0.func = GPU_SUBTRACT;
			if (!MapOperand(a, isAlpha, false, cIn1, s0.constSrc, s0.src[0], s0.op[0]) ||
			    !MapOperand(b, isAlpha, false, cIn1, s0.constSrc, s0.src[1], s0.op[1]))
				return false;

			SChanOp s1 = MakePassthrough(isAlpha);
			s1.func = (d == IN_0) ? GPU_MODULATE : GPU_MULTIPLY_ADD;
			s1.src[0] = GPU_PREVIOUS;
			s1.op[0] = isAlpha ? (u8)GPU_TEVOP_A_SRC_ALPHA : (u8)GPU_TEVOP_RGB_SRC_COLOR;
			if (!MapOperand(c, isAlpha, false, cIn1, s1.constSrc, s1.src[1], s1.op[1]))
				return false;
			if (d != IN_0)
			{
				if (!MapOperand(d, isAlpha, false, cIn1, s1.constSrc, s1.src[2], s1.op[2]))
					return false;
			}

			seq.ops[0] = s0;
			seq.ops[1] = s1;
			seq.count = 2;
			return true;
		}
	}

	static inline u32 ResolveStageColor(EPicaConstSrc rgbSrc, EPicaConstSrc alphaSrc,
		u32 primABGR, u32 envABGR, u8 primLodU8)
	{
		u32 rgb = 0x00FFFFFFu;
		switch (rgbSrc)
		{
		case PICA_CONST_ZERO:       rgb = 0x000000u; break;
		case PICA_CONST_ONE:        rgb = 0xFFFFFFu; break;
		case PICA_CONST_HALF:       rgb = 0x808080u; break;
		case PICA_CONST_PRIM_RGB:   rgb = primABGR & 0x00FFFFFFu; break;
		case PICA_CONST_PRIM_ALPHA: rgb = ((primABGR >> 24) & 0xFFu) * 0x010101u; break;
		case PICA_CONST_ENV_RGB:    rgb = envABGR & 0x00FFFFFFu; break;
		case PICA_CONST_ENV_ALPHA:  rgb = ((envABGR >> 24) & 0xFFu) * 0x010101u; break;
		case PICA_CONST_PRIM_LOD:   rgb = static_cast<u32>(primLodU8) * 0x010101u; break;
		case PICA_CONST_NONE:
		default:                    rgb = 0xFFFFFFu; break;
		}

		u32 a = 0xFF000000u;
		switch (alphaSrc)
		{
		case PICA_CONST_ZERO:       a = 0x00000000u; break;
		case PICA_CONST_ONE:        a = 0xFF000000u; break;
		case PICA_CONST_HALF:       a = 0x80000000u; break;
		case PICA_CONST_PRIM_RGB:
		case PICA_CONST_PRIM_ALPHA: a = primABGR & 0xFF000000u; break;
		case PICA_CONST_ENV_RGB:
		case PICA_CONST_ENV_ALPHA:  a = envABGR & 0xFF000000u; break;
		case PICA_CONST_PRIM_LOD:   a = static_cast<u32>(primLodU8) << 24; break;
		case PICA_CONST_NONE:
		default:                    a = 0xFF000000u; break;
		}

		return rgb | a;
	}
}

namespace PicaTEV
{

bool CompileMux(u64 mux, bool two_cycles, SPicaTevProgram & out)
{
	std::memset(&out, 0, sizeof(out));

	RDP_Combine m;
	m.mux = mux;

	// In 1-cycle mode, N64 RDP redirects TEXEL1 -> TEXEL0 (correctFirstStageParam in GLideN64/ctr64).
	auto fix1Cycle = [two_cycles](EN64Input in) -> EN64Input {
		if (!two_cycles)
		{
			if (in == IN_TEXEL1)       return IN_TEXEL0;
			if (in == IN_TEXEL1_ALPHA) return IN_TEXEL0_ALPHA;
		}
		return in;
	};

	// Cycle 0
	SChanSeq c0Rgb, c0Alpha;
	if (!CompileCycleChannel(fix1Cycle(kRgbA[m.aRGB0]), fix1Cycle(kRgbB[m.bRGB0]), fix1Cycle(kRgbC[m.cRGB0]), fix1Cycle(kRgbD[m.dRGB0]), false, false, c0Rgb))
		return false;
	if (!CompileCycleChannel(fix1Cycle(kAlphaABD[m.aA0]), fix1Cycle(kAlphaABD[m.bA0]), fix1Cycle(kAlphaC[m.cA0]), fix1Cycle(kAlphaABD[m.dA0]), true, false, c0Alpha))
		return false;

	int c0Stages = c0Rgb.count > c0Alpha.count ? c0Rgb.count : c0Alpha.count;
	if (c0Stages <= 0) c0Stages = 1;

	// Cycle 1 (optional)
	SChanSeq c1Rgb = {}, c1Alpha = {};
	int c1Stages = 0;
	if (two_cycles)
	{
		if (!CompileCycleChannel(kRgbA[m.aRGB1], kRgbB[m.bRGB1], kRgbC[m.cRGB1], kRgbD[m.dRGB1], false, true, c1Rgb))
			return false;
		if (!CompileCycleChannel(kAlphaABD[m.aA1], kAlphaABD[m.bA1], kAlphaC[m.cA1], kAlphaABD[m.dA1], true, true, c1Alpha))
			return false;

		c1Stages = c1Rgb.count > c1Alpha.count ? c1Rgb.count : c1Alpha.count;

		// If Cycle 1 does not read Cycle 0's output (COMBINED / COMBINED_ALPHA) in either channel,
		// Cycle 0 is completely overwritten by Cycle 1 (common when games leave G_CYC_2CYCLE set
		// with two identical 1-cycle macros). Skip Cycle 0 stages completely!
		if (!c1Rgb.usesCombined && !c1Alpha.usesCombined && c1Stages > 0)
		{
			c0Rgb   = c1Rgb;
			c0Alpha = c1Alpha;
			c0Stages = c1Stages;
			c1Rgb = {};
			c1Alpha = {};
			c1Stages = 0;
			two_cycles = false;
		}
	}

	const int totalStages = c0Stages + c1Stages;
	if (totalStages > 6)
		return false;

	// Check if Cycle 1 needs Cycle 0 saved in GPU_PREVIOUS_BUFFER.
	// PICA200 only supports buffering outputs of stages 0..3.
	const bool needRgbBuf   = two_cycles && c1Rgb.usesCombinedAfterFirst;
	const bool needAlphaBuf = two_cycles && c1Alpha.usesCombinedAfterFirst;
	const int  c0LastStage  = c0Stages - 1;
	if ((needRgbBuf || needAlphaBuf) && c0LastStage > 3)
		return false;

	// Also: if Cycle 1 RGB has 2 stages and Cycle 1 Alpha has 1 stage (or vice versa),
	// check whether the shorter channel's stage 0 overwrites GPU_PREVIOUS before
	// the longer channel's stage 1 reads COMBINED_ALPHA (cross-channel read)!
	// If Cycle 1 RGB reads COMBINED_ALPHA in stage 1 while Alpha wrote a new value in stage 0,
	// we must buffer Alpha at c0LastStage too!
	const bool crossAlphaNeeded = needRgbBuf && (c1Alpha.count > 0);
	if (crossAlphaNeeded && c0LastStage > 3)
		return false;

	if (needRgbBuf)
		out.bufWriteRgbMask = static_cast<u8>(1u << c0LastStage);
	if (needAlphaBuf || crossAlphaNeeded)
		out.bufWriteAlphaMask = static_cast<u8>(1u << c0LastStage);

	// Emit Cycle 0 stages (0 .. c0Stages - 1)
	for (int s = 0; s < c0Stages; ++s)
	{
		const SChanOp rgbOp   = (s < c0Rgb.count)   ? c0Rgb.ops[s]   : MakePassthrough(false);
		const SChanOp alphaOp = (s < c0Alpha.count) ? c0Alpha.ops[s] : MakePassthrough(true);

		SPicaTevStage & dst = out.stages[s];
		for (int k = 0; k < 3; ++k)
		{
			dst.srcRgb[k]   = rgbOp.src[k];
			dst.opRgb[k]    = static_cast<GPU_TEVOP_RGB>(rgbOp.op[k]);
			dst.srcAlpha[k] = alphaOp.src[k];
			dst.opAlpha[k]  = static_cast<GPU_TEVOP_A>(alphaOp.op[k]);
		}
		dst.funcRgb    = rgbOp.func;
		dst.funcAlpha  = alphaOp.func;
		dst.constRgb   = rgbOp.constSrc;
		dst.constAlpha = alphaOp.constSrc;
	}

	// Emit Cycle 1 stages (c0Stages .. totalStages - 1)
	for (int i = 0; i < c1Stages; ++i)
	{
		const int s = c0Stages + i;
		const SChanOp rgbOp   = (i < c1Rgb.count)   ? c1Rgb.ops[i]   : MakePassthrough(false);
		const SChanOp alphaOp = (i < c1Alpha.count) ? c1Alpha.ops[i] : MakePassthrough(true);

		SPicaTevStage & dst = out.stages[s];
		for (int k = 0; k < 3; ++k)
		{
			dst.srcRgb[k]   = rgbOp.src[k];
			dst.opRgb[k]    = static_cast<GPU_TEVOP_RGB>(rgbOp.op[k]);
			dst.srcAlpha[k] = alphaOp.src[k];
			dst.opAlpha[k]  = static_cast<GPU_TEVOP_A>(alphaOp.op[k]);
		}
		dst.funcRgb    = rgbOp.func;
		dst.funcAlpha  = alphaOp.func;
		dst.constRgb   = rgbOp.constSrc;
		dst.constAlpha = alphaOp.constSrc;
	}

	out.numStages = static_cast<u8>(totalStages);
	out.usesTex0  = c0Rgb.usesTex0 || c0Alpha.usesTex0 || c1Rgb.usesTex0 || c1Alpha.usesTex0;
	out.usesTex1  = c0Rgb.usesTex1 || c0Alpha.usesTex1 || c1Rgb.usesTex1 || c1Alpha.usesTex1;
	u8 cMask = 0;
	u8 sMask = 0;
	for (int s = 0; s < totalStages; ++s)
	{
		const EPicaConstSrc cs[2] = { out.stages[s].constRgb, out.stages[s].constAlpha };
		for (int k = 0; k < 2; ++k)
		{
			switch (cs[k])
			{
			case PICA_CONST_PRIM_RGB:
			case PICA_CONST_PRIM_ALPHA:
				cMask |= 1u;
				sMask |= static_cast<u8>(1u << s);
				break;
			case PICA_CONST_ENV_RGB:
			case PICA_CONST_ENV_ALPHA:
				cMask |= 2u;
				sMask |= static_cast<u8>(1u << s);
				break;
			case PICA_CONST_PRIM_LOD:
				cMask |= 4u;
				sMask |= static_cast<u8>(1u << s);
				break;
			default:
				break;
			}
		}
	}
	out.constUsedMask         = cMask;
	out.dynamicConstStageMask = sMask;
	out.valid                 = true;
	return true;
}

void ApplyProgram(const SPicaTevProgram & prog, u32 primABGR, u32 envABGR, u8 primLodU8)
{
	Citro3D::FlushPendingBatch();

	static int sPrevActiveStages = 6;
	const int n = prog.numStages;
	for (int s = 0; s < n; ++s)
	{
		const SPicaTevStage & st = prog.stages[s];
		C3D_TexEnv * env = C3D_GetTexEnv(s);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_RGB,   st.srcRgb[0],   st.srcRgb[1],   st.srcRgb[2]);
		C3D_TexEnvSrc(env, C3D_Alpha, st.srcAlpha[0], st.srcAlpha[1], st.srcAlpha[2]);
		C3D_TexEnvOpRgb(env,   st.opRgb[0],   st.opRgb[1],   st.opRgb[2]);
		C3D_TexEnvOpAlpha(env, st.opAlpha[0], st.opAlpha[1], st.opAlpha[2]);
		C3D_TexEnvFunc(env, C3D_RGB,   st.funcRgb);
		C3D_TexEnvFunc(env, C3D_Alpha, st.funcAlpha);
		if (st.constRgb != PICA_CONST_NONE || st.constAlpha != PICA_CONST_NONE)
		{
			C3D_TexEnvColor(env, ResolveStageColor(st.constRgb, st.constAlpha, primABGR, envABGR, primLodU8));
		}
	}
	const int maxReset = (sPrevActiveStages > n) ? sPrevActiveStages : n;
	for (int s = n; s < maxReset; ++s)
	{
		C3D_TexEnvInit(C3D_GetTexEnv(s));
	}
	sPrevActiveStages = n;
	C3D_TexEnvBufUpdate(C3D_RGB,   prog.bufWriteRgbMask);
	C3D_TexEnvBufUpdate(C3D_Alpha, prog.bufWriteAlphaMask);
}

void UpdateProgramColors(const SPicaTevProgram & prog, u32 primABGR, u32 envABGR, u8 primLodU8)
{
	u8 mask = prog.dynamicConstStageMask;
	if (mask == 0)
		return;
	Citro3D::FlushPendingBatch();
	for (int s = 0; mask != 0; ++s, mask >>= 1)
	{
		if (mask & 1u)
		{
			const SPicaTevStage & st = prog.stages[s];
			C3D_TexEnvColor(C3D_GetTexEnv(s), ResolveStageColor(st.constRgb, st.constAlpha, primABGR, envABGR, primLodU8));
		}
	}
}

} // namespace PicaTEV
