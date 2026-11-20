#pragma once

#include <3ds.h>
#include <citro3d.h>
#include "Utility/DaedalusTypes.h"

// Native PICA200 multi-stage TEV compiler for N64 RDP Color Combiner muxes.
// Compiles 1-cycle and 2-cycle N64 equations (A - B) * C + D directly into
// up to 6 hardware TEV stages with live Prim/Env/PrimLOD constants and dual
// texture units (TEXUNIT0 + TEXUNIT1) — eliminating CPU vertex-color rewriting
// and multipass overdraw on Old 3DS.

enum EPicaConstSrc : u8
{
	PICA_CONST_NONE = 0,
	PICA_CONST_ZERO,
	PICA_CONST_ONE,
	PICA_CONST_HALF,
	PICA_CONST_PRIM_RGB,
	PICA_CONST_PRIM_ALPHA,
	PICA_CONST_ENV_RGB,
	PICA_CONST_ENV_ALPHA,
	PICA_CONST_PRIM_LOD,
};

struct SPicaTevStage
{
	GPU_TEVSRC      srcRgb[3];
	GPU_TEVSRC      srcAlpha[3];
	GPU_TEVOP_RGB   opRgb[3];
	GPU_TEVOP_A     opAlpha[3];
	GPU_COMBINEFUNC funcRgb;
	GPU_COMBINEFUNC funcAlpha;
	EPicaConstSrc   constRgb;
	EPicaConstSrc   constAlpha;
};

struct SPicaTevProgram
{
	bool          valid;
	bool          usesTex0;
	bool          usesTex1;
	u8            numStages;
	u8            bufWriteRgbMask;   // bit s -> write stage s RGB to TEV buffer (s in 0..3)
	u8            bufWriteAlphaMask; // bit s -> write stage s Alpha to TEV buffer (s in 0..3)
	SPicaTevStage stages[6];
};

namespace PicaTEV
{
	// Compiles an N64 64-bit RDP mux into a PICA200 hardware TEV program.
	// Returns true and fills `out` if exact hardware compilation succeeded within 6 stages.
	// Returns false if the mux uses unsupported inputs (NOISE/K4/K5/LOD_FRACTION) or
	// exceeds PICA200 stage limits, signaling RendererCTR to use the fallback path.
	bool CompileMux(u64 mux, bool two_cycles, SPicaTevProgram & out);

	// Applies a compiled TEV program to PICA200 hardware registers using live
	// Primitive, Environment, and PrimLODFraction values.
	void ApplyProgram(const SPicaTevProgram & prog, u32 primABGR, u32 envABGR, u8 primLodU8);
}
