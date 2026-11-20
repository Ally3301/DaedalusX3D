#include "stdafx.h"
#include "Citro3DContext.h"
#include "SysCTR/Utility/PerfProfilerCTR.h"

#include <cstring>
#include "daedalus_vshader_shbin.h"

namespace
{
	constexpr size_t kCmdBufSize       = 0x080000; // 512 KiB GX command buffer

	constexpr u32 kDisplayTransferFlags =
		GX_TRANSFER_FLIP_VERT(0) |
		GX_TRANSFER_OUT_TILED(0) |
		GX_TRANSFER_RAW_COPY(0) |
		GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
		GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

	bool               sInitialised    = false;
	bool               sGameFrameOpen  = false;
	bool               sUIFrameOpen    = false;
	u32                sDrawsInFrame   = 0;

	bool               sBatchActive    = false;
	int                sBatchFirst     = 0;
	int                sBatchCount     = 0;

	C3D_RenderTarget * sTopTarget      = nullptr;
	C3D_RenderTarget * sBotTarget      = nullptr;
	C3D_RenderTarget * sActiveTarget   = nullptr;

	DVLB_s *           sVshDvlb        = nullptr;
	shaderProgram_s    sProgram;
	int                sLocProjection  = -1;
	int                sLocUv1Xform    = -1;

	struct SStateCache
	{
		bool            valid;
		bool            depthEnable;
		bool            depthMask;
		bool            decalOffset;
		bool            cullEnable;
		bool            cullBack;
		bool            blendEnable;
		GPU_BLENDFACTOR blendSrc;
		GPU_BLENDFACTOR blendDst;
		bool            alphaEnable;
		GPU_TESTFUNC    alphaFunc;
		u8              alphaRef;
		bool            texEnabled;
		C3D_Tex *       boundTex[2];
		u32             boundTexParam[2];
		u32             texEnvMode;
		u32             texEnvColor;
		bool            tevDirty;
		bool            customTevActive;
		float           uv1Sx, uv1Sy, uv1Ox, uv1Oy;
		bool            uv1Valid;
		const void *    vtxBufPtr;
		size_t          vtxStride;
		bool            scissorEnable;
		u32             scissorL;
		u32             scissorT;
		u32             scissorR;
		u32             scissorB;
		u32             vpX;
		u32             vpY;
		u32             vpW;
		u32             vpH;
	};

	SStateCache sState = {};

	inline s32 ClampS32(s32 v, s32 lo, s32 hi)
	{
		return v < lo ? lo : (v > hi ? hi : v);
	}

	inline void CommitPendingBatch()
	{
		if (!sBatchActive)
			return;
		if (sBatchCount > 0)
		{
			C3D_DrawArrays(GPU_TRIANGLES, sBatchFirst, sBatchCount);
			++sDrawsInFrame;
		}
		sBatchActive = false;
		sBatchFirst  = 0;
		sBatchCount  = 0;
	}

	void ApplyTexEnvNow()
	{
		if (sState.customTevActive)
		{
			// Custom multi-stage PICA200 TEV program was uploaded directly via PicaTEV::ApplyProgram.
			sState.tevDirty = false;
			return;
		}

		C3D_TexEnv * env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);

		if (!sState.texEnabled)
		{
			C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}
		else
		{
			switch (sState.texEnvMode)
			{
			case CTR_TEXENV_REPLACE:
				C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
				C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
				break;

			case CTR_TEXENV_ADD:
				C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
				break;

			case CTR_TEXENV_DECAL:
				// RGB = Tex0 * Tex0.a + Primary * (1 - Tex0.a); Alpha = Primary.a
				C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
				C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
				break;

			case CTR_TEXENV_BLEND:
				// RGB = EnvColor * Tex0 + Primary * (1 - Tex0); Alpha = Primary.a * Tex0.a
				C3D_TexEnvColor(env, sState.texEnvColor);
				C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
				break;

			case CTR_TEXENV_MODULATE:
			default:
				C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
				break;
			}
		}

		sState.tevDirty = false;
	}

	void SetupVertexAttribs()
	{
		C3D_AttrInfo * attrInfo = C3D_GetAttrInfo();
		AttrInfo_Init(attrInfo);
		AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 2);         // v0: Texture (u, v)
		AttrInfo_AddLoader(attrInfo, 1, GPU_UNSIGNED_BYTE, 4); // v1: Colour  (r, g, b, a)
		AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 3);         // v2: Position (x, y, z)
	}
	static void FlushLinearCache(const void * ptr, size_t size)
	{
		if (!ptr || size == 0)
			return;
		static bool sDirectSvc = true;
		if (sDirectSvc && R_SUCCEEDED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)ptr, (u32)size)))
			return;
		sDirectSvc = false;
		GSPGPU_FlushDataCache(ptr, size);
	}
}

namespace Citro3D
{

void FlushDataCache(const void * ptr, size_t size)
{
	FlushLinearCache(ptr, size);
}

bool Init()
{
	if (sInitialised)
		return true;

	if (!C3D_Init(kCmdBufSize))
		return false;

	// Use GPU_RB_DEPTH24_STENCIL8 (4 bytes/px, 32-bit aligned) on both targets, matching
	// picaGL, ctr64, sm64_3ds, and devkitPro's 3ds-examples for 100% real PICA200 compatibility.
	sTopTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!sTopTarget)
	{
		C3D_Fini();
		return false;
	}
	C3D_RenderTargetSetOutput(sTopTarget, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);

	sBotTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!sBotTarget)
	{
		C3D_RenderTargetDelete(sTopTarget);
		sTopTarget = nullptr;
		C3D_Fini();
		return false;
	}
	C3D_RenderTargetSetOutput(sBotTarget, GFX_BOTTOM, GFX_LEFT, kDisplayTransferFlags);

	sVshDvlb = DVLB_ParseFile(reinterpret_cast<u32 *>(const_cast<u8 *>(daedalus_vshader_shbin)), daedalus_vshader_shbin_size);
	if (!sVshDvlb)
	{
		C3D_RenderTargetDelete(sBotTarget);
		C3D_RenderTargetDelete(sTopTarget);
		sBotTarget = nullptr;
		sTopTarget = nullptr;
		C3D_Fini();
		return false;
	}

	shaderProgramInit(&sProgram);
	shaderProgramSetVsh(&sProgram, &sVshDvlb->DVLE[0]);
	C3D_BindProgram(&sProgram);

	sLocProjection = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
	sLocUv1Xform   = shaderInstanceGetUniformLocation(sProgram.vertexShader, "uv1_xform");

	SetupVertexAttribs();

	// Reset TEV stages 1..5 to passthrough once.
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));

	sActiveTarget = sTopTarget;
	sGameFrameOpen = false;
	sUIFrameOpen = false;
	sDrawsInFrame = 0;
	sInitialised = true;

	InvalidateStateCache();
	return true;
}

void FlushPendingBatch()
{
	CommitPendingBatch();
}

void Shutdown()
{
	if (!sInitialised)
		return;

	CommitPendingBatch();
	if (sGameFrameOpen || sUIFrameOpen)
	{
		C3D_FrameEnd(GX_CMDLIST_FLUSH);
		sGameFrameOpen = false;
		sUIFrameOpen = false;
		sDrawsInFrame = 0;
	}
	// Do NOT call C3D_FrameSync() during Shutdown: when exiting via the 3DS HOME
	// Menu (!aptMainLoop()), GSP has already suspended VBlank delivery to the
	// application, so C3D_FrameSync() -> GSPGPU_WaitForVBlank() would hang forever.

	shaderProgramFree(&sProgram);
	if (sVshDvlb)
	{
		DVLB_Free(sVshDvlb);
		sVshDvlb = nullptr;
	}

	if (sBotTarget)
	{
		C3D_RenderTargetDelete(sBotTarget);
		sBotTarget = nullptr;
	}
	if (sTopTarget)
	{
		C3D_RenderTargetDelete(sTopTarget);
		sTopTarget = nullptr;
	}
	sActiveTarget = nullptr;

	C3D_Fini();
	sInitialised = false;
}

bool IsInitialised()
{
	return sInitialised;
}

void EnsureGameFrameBegun()
{
	if (!sInitialised || sGameFrameOpen)
		return;

	if (sUIFrameOpen)
	{
		CommitPendingBatch();
		C3D_FrameEnd(GX_CMDLIST_FLUSH);
		sUIFrameOpen = false;
	}

	// Do NOT pass C3D_FRAME_SYNCDRAW during gameplay: C3D_FrameSync() sleeps up to
	// 16.67 ms waiting for BOTH LCD VBlanks on every rendered frame! C3D_FrameBegin(0)
	// waits only for the previous PICA200 GPU command queue (~2 ms), while Daedalus's
	// own FramerateLimiter handles N64 speed sync and citro3d swaps tear-free on VBlank0.
	C3D_FrameBegin(0);
	sGameFrameOpen = true;
	sDrawsInFrame = 0;
	sActiveTarget = sTopTarget;
	C3D_FrameDrawOn(sTopTarget);
	C3D_BindProgram(&sProgram);
	SetupVertexAttribs();
	InvalidateStateCache();
}

void EndGameFrame()
{
	if (!sInitialised || !sGameFrameOpen)
		return;

	CommitPendingBatch();
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	sGameFrameOpen = false;
	sDrawsInFrame = 0;
}

void SplitGameFrame()
{
	if (!sInitialised || !sGameFrameOpen)
		return;

	CommitPendingBatch();
	// Submit the current frame's command list and open a fresh command buffer
	// so vertex buffer wrap-around never overwrites in-flight vertices.
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	C3D_FrameBegin(0);
	sDrawsInFrame = 0;
	sActiveTarget = sTopTarget;
	C3D_FrameDrawOn(sTopTarget);
	C3D_BindProgram(&sProgram);
	SetupVertexAttribs();
	InvalidateStateCache();
}

bool IsGameFrameActive()
{
	return sGameFrameOpen;
}

bool BeginGameBottomPass()
{
	if (!sInitialised || !sBotTarget)
		return false;

	if (!sGameFrameOpen)
		EnsureGameFrameBegun();

	CommitPendingBatch();
	sActiveTarget = sBotTarget;
	C3D_FrameDrawOn(sBotTarget);

	InvalidateStateCache();
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	SetUv1Transform(1.0f, 1.0f, 0.0f, 0.0f);
	SetScissorLandscape(false, 0, 0, 320, 240, 320, 240);
	SetViewportLandscape(0, 0, 320, 240, 320, 240);
	SetDepthState(false, false, false);
	SetCullMode(false, true);
	SetAlphaTest(false);
	SetBlendState(true, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	SetTextureEnabled(true);
	SetTexEnvMode(CTR_TEXENV_MODULATE);

	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 320.0f, 240.0f, 0.0f, -1.0f, 1.0f, true);
	SetProjectionMtx(&proj);
	return true;
}

void EndGameBottomPass()
{
	if (!sInitialised || !sGameFrameOpen)
		return;
	CommitPendingBatch();
	sActiveTarget = sTopTarget;
	InvalidateStateCache();
}

void BeginUIFrame()
{
	if (!sInitialised)
		return;

	if (sGameFrameOpen)
	{
		CommitPendingBatch();
		C3D_FrameEnd(GX_CMDLIST_FLUSH);
		sGameFrameOpen = false;
		sDrawsInFrame = 0;
	}
	if (!sUIFrameOpen)
	{
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		sUIFrameOpen = true;
		sDrawsInFrame = 0;
		C3D_BindProgram(&sProgram);
		SetupVertexAttribs();
		InvalidateStateCache();
	}
}

void BeginUIScreen(gfxScreen_t screen, int width, int height, u32 clearRGBA8)
{
	if (!sInitialised)
		return;
	if (!sUIFrameOpen)
		BeginUIFrame();

	CommitPendingBatch();
	C3D_RenderTarget * target = (screen == GFX_BOTTOM) ? sBotTarget : sTopTarget;
	sActiveTarget = target;
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clearRGBA8, 0);
	C3D_FrameDrawOn(target);

	InvalidateStateCache();
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	SetUv1Transform(1.0f, 1.0f, 0.0f, 0.0f);
	SetScissorLandscape(false, 0, 0, width, height, width, height);
	SetViewportLandscape(0, 0, width, height, width, height);
	SetDepthState(false, false, false);
	SetCullMode(false, true);
	SetAlphaTest(false);
	SetBlendState(true, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	SetTextureEnabled(true);
	SetTexEnvMode(CTR_TEXENV_MODULATE);

	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, -1.0f, 1.0f, true);
	SetProjectionMtx(&proj);
}

void EndUIFrame()
{
	if (!sInitialised || !sUIFrameOpen)
		return;

	CommitPendingBatch();
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	sUIFrameOpen = false;
	sDrawsInFrame = 0;
}

void Clear(bool clearColor, bool clearDepth, u32 clearRGBA8, u32 clearDepthVal)
{
	if (!sInitialised || !sActiveTarget)
		return;

	u32 bits = 0;
	if (clearColor) bits |= C3D_CLEAR_COLOR;
	if (clearDepth) bits |= C3D_CLEAR_DEPTH;
	if (bits == 0)
		return;

	CommitPendingBatch();

	// If draw commands were already recorded into the current command buffer,
	// split the command list first so those draws execute on the GPU BEFORE
	// GX_MemoryFill touches the color/depth buffer (critical for mid-frame Z clears!).
	if ((sGameFrameOpen || sUIFrameOpen) && sDrawsInFrame > 0)
	{
		C3D_FrameSplit(GX_CMDLIST_FLUSH);
		sDrawsInFrame = 0;
	}

	C3D_RenderTargetClear(sActiveTarget, static_cast<C3D_ClearBits>(bits), clearRGBA8, clearDepthVal);
	// Re-bind active target and restore viewport/scissor after clear.
	if (sGameFrameOpen || sUIFrameOpen)
	{
		C3D_FrameDrawOn(sActiveTarget);
		if (sState.valid && sState.vpW > 0 && sState.vpH > 0)
			C3D_SetViewport(sState.vpX, sState.vpY, sState.vpW, sState.vpH);
		if (sState.valid && sState.scissorEnable)
			C3D_SetScissor(GPU_SCISSOR_NORMAL, sState.scissorL, sState.scissorT, sState.scissorR, sState.scissorB);
	}
}

void SetViewportLandscape(s32 x, s32 y, s32 w, s32 h, s32 screenW, s32 screenH)
{
	if (w <= 0 || h <= 0)
		return;

	const s32 fbX = ClampS32(y, 0, screenH);
	const s32 fbY = ClampS32(screenW - (x + w), 0, screenW);
	const s32 fbW = ClampS32(h, 1, screenH - fbX);
	const s32 fbH = ClampS32(w, 1, screenW - fbY);

	if (!sState.valid || sState.vpX != (u32)fbX || sState.vpY != (u32)fbY ||
		sState.vpW != (u32)fbW || sState.vpH != (u32)fbH)
	{
		CommitPendingBatch();
		C3D_SetViewport((u32)fbX, (u32)fbY, (u32)fbW, (u32)fbH);
		// NOTE: citro3d's C3D_SetViewport internally resets ctx->scissor[0] = GPU_SCISSOR_DISABLE!
		// Re-apply active scissor if enabled so C3D_SetViewport never silently drops N64 scissor!
		if (sState.scissorEnable)
		{
			C3D_SetScissor(GPU_SCISSOR_NORMAL, sState.scissorL, sState.scissorT, sState.scissorR, sState.scissorB);
		}
		sState.vpX = (u32)fbX;
		sState.vpY = (u32)fbY;
		sState.vpW = (u32)fbW;
		sState.vpH = (u32)fbH;
	}
}

void SetScissorLandscape(bool enable, s32 x, s32 y, s32 w, s32 h, s32 screenW, s32 screenH)
{
	if (!enable)
	{
		if (!sState.valid || sState.scissorEnable)
		{
			CommitPendingBatch();
			C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
			sState.scissorEnable = false;
		}
		return;
	}

	const s32 fbL = ClampS32(y, 0, screenH);
	const s32 fbR = ClampS32(y + h, fbL, screenH);
	const s32 fbT = ClampS32(screenW - (x + w), 0, screenW);
	const s32 fbB = ClampS32(screenW - x, fbT, screenW);

	if (!sState.valid || !sState.scissorEnable ||
		sState.scissorL != (u32)fbL || sState.scissorT != (u32)fbT ||
		sState.scissorR != (u32)fbR || sState.scissorB != (u32)fbB)
	{
		CommitPendingBatch();
		C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)fbL, (u32)fbT, (u32)fbR, (u32)fbB);
		sState.scissorEnable = true;
		sState.scissorL = (u32)fbL;
		sState.scissorT = (u32)fbT;
		sState.scissorR = (u32)fbR;
		sState.scissorB = (u32)fbB;
	}
}

void SetProjectionGL(const float * m)
{
	if (!m || sLocProjection < 0)
		return;

	CommitPendingBatch();

	C3D_Mtx mtx;
	mtx.r[0].x =  m[1];  mtx.r[0].y =  m[5];  mtx.r[0].z =  m[9];  mtx.r[0].w =  m[13];
	mtx.r[1].x = -m[0];  mtx.r[1].y = -m[4];  mtx.r[1].z = -m[8];  mtx.r[1].w = -m[12];
	mtx.r[2].x = -0.5f * (m[2]  + m[3]);
	mtx.r[2].y = -0.5f * (m[6]  + m[7]);
	mtx.r[2].z = -0.5f * (m[10] + m[11]);
	mtx.r[2].w = -0.5f * (m[14] + m[15]);
	mtx.r[3].x =  m[3];  mtx.r[3].y =  m[7];  mtx.r[3].z =  m[11]; mtx.r[3].w =  m[15];

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocProjection, &mtx);
}

void SetProjectionMtx(const C3D_Mtx * mtx)
{
	if (!mtx || sLocProjection < 0)
		return;
	CommitPendingBatch();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocProjection, mtx);
}

void SetDepthState(bool enable, bool writeMask, bool decal)
{
	if (!sState.valid || sState.decalOffset != decal)
	{
		CommitPendingBatch();
		C3D_DepthMap(true, -1.0f, decal ? -0.00025f : 0.0f);
		sState.decalOffset = decal;
	}
	if (!sState.valid || sState.depthEnable != enable || sState.depthMask != writeMask)
	{
		CommitPendingBatch();
		const GPU_WRITEMASK mask = writeMask ? GPU_WRITE_ALL : GPU_WRITE_COLOR;
		C3D_DepthTest(enable, enable ? GPU_LEQUAL : GPU_ALWAYS, mask);
		sState.depthEnable = enable;
		sState.depthMask = writeMask;
	}
}

void SetCullMode(bool enable, bool cullBack)
{
	if (!sState.valid || sState.cullEnable != enable || (enable && sState.cullBack != cullBack))
	{
		CommitPendingBatch();
		if (!enable)
			C3D_CullFace(GPU_CULL_NONE);
		else
			C3D_CullFace(cullBack ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW);
		sState.cullEnable = enable;
		sState.cullBack = cullBack;
	}
}

void SetBlendState(bool enable, GPU_BLENDFACTOR src, GPU_BLENDFACTOR dst)
{
	if (!enable)
	{
		if (!sState.valid || sState.blendEnable)
		{
			CommitPendingBatch();
			// Disable blending via standard PICA200 ONE/ZERO blend factors instead of
			// switching GPUREG_COLOR_OPERATION to logic-op mode (matches ctr64 & sm64_3ds).
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			sState.blendEnable = false;
		}
		return;
	}

	if (!sState.valid || !sState.blendEnable || sState.blendSrc != src || sState.blendDst != dst)
	{
		CommitPendingBatch();
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, src, dst);
		sState.blendEnable = true;
		sState.blendSrc = src;
		sState.blendDst = dst;
	}
}

void SetAlphaTest(bool enable, GPU_TESTFUNC func, u8 ref)
{
	if (!enable)
	{
		if (!sState.valid || sState.alphaEnable)
		{
			CommitPendingBatch();
			C3D_AlphaTest(false, GPU_ALWAYS, 0);
			sState.alphaEnable = false;
		}
		return;
	}

	if (!sState.valid || !sState.alphaEnable || sState.alphaFunc != func || sState.alphaRef != ref)
	{
		CommitPendingBatch();
		C3D_AlphaTest(true, func, ref);
		sState.alphaEnable = true;
		sState.alphaFunc = func;
		sState.alphaRef = ref;
	}
}

void SetTextureEnabled(bool enable)
{
	if (!sState.valid || sState.customTevActive || sState.texEnabled != enable)
	{
		CommitPendingBatch();
		if (sState.customTevActive)
		{
			for (int i = 1; i < 6; ++i)
				C3D_TexEnvInit(C3D_GetTexEnv(i));
			C3D_TexEnvBufUpdate(C3D_Both, 0);
			sState.customTevActive = false;
		}
		sState.texEnabled = enable;
		sState.tevDirty = true;
	}
}

void BindTexture(C3D_Tex * tex)
{
	BindTextureUnit(0, tex);
}

void BindTextureUnit(int unitId, C3D_Tex * tex)
{
	if (!tex || unitId < 0 || unitId > 1)
		return;
	if (!sState.valid || sState.boundTex[unitId] != tex || sState.boundTexParam[unitId] != tex->param)
	{
		CommitPendingBatch();
		C3D_TexBind(unitId, tex);
		sState.boundTex[unitId] = tex;
		sState.boundTexParam[unitId] = tex->param;
	}
}

void SetUv1Transform(float scaleX, float scaleY, float offX, float offY)
{
	if (sLocUv1Xform < 0)
		return;
	if (!sState.uv1Valid || sState.uv1Sx != scaleX || sState.uv1Sy != scaleY ||
		sState.uv1Ox != offX || sState.uv1Oy != offY)
	{
		CommitPendingBatch();
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocUv1Xform, scaleX, scaleY, offX, offY);
		sState.uv1Sx = scaleX;
		sState.uv1Sy = scaleY;
		sState.uv1Ox = offX;
		sState.uv1Oy = offY;
		sState.uv1Valid = true;
	}
}

void SetTexEnvMode(u32 mode)
{
	if (sState.customTevActive)
	{
		CommitPendingBatch();
		for (int i = 1; i < 6; ++i)
			C3D_TexEnvInit(C3D_GetTexEnv(i));
		C3D_TexEnvBufUpdate(C3D_Both, 0);
		sState.customTevActive = false;
		sState.tevDirty = true;
	}
	if (!sState.valid || sState.texEnvMode != mode)
	{
		CommitPendingBatch();
		sState.texEnvMode = mode;
		sState.tevDirty = true;
	}
}

void SetTexEnvColor(u32 colorABGR)
{
	if (!sState.valid || sState.texEnvColor != colorABGR)
	{
		if (sState.texEnvMode == CTR_TEXENV_BLEND)
		{
			CommitPendingBatch();
			sState.tevDirty = true;
		}
		sState.texEnvColor = colorABGR;
	}
}

void MarkCustomTevActive()
{
	sState.customTevActive = true;
	sState.tevDirty = false;
	sState.valid = true;
}

bool IsScissorFullScreen(s32 screenW, s32 screenH)
{
	if (!sState.valid || !sState.scissorEnable)
		return true;
	return sState.scissorL == 0 && sState.scissorT == 0 &&
	       sState.scissorR >= (u32)screenH && sState.scissorB >= (u32)screenW;
}

void SetFogEnable(bool enable)
{
	(void)enable;
}

void SetFogMinMax(float fogNear, float fogFar)
{
	(void)fogNear;
	(void)fogFar;
}

void SetFogColor(u32 colorABGR)
{
	(void)colorABGR;
}

void SetVertexBuffer(const void * data, size_t stride)
{
	if (!data)
		return;
	if (!sState.valid || sState.vtxBufPtr != data || sState.vtxStride != stride)
	{
		CommitPendingBatch();
		C3D_BufInfo * bufInfo = C3D_GetBufInfo();
		BufInfo_Init(bufInfo);
		BufInfo_Add(bufInfo, data, static_cast<ptrdiff_t>(stride), 3, 0x210);
		sState.vtxBufPtr = data;
		sState.vtxStride = stride;
	}
}

void DrawArrays(GPU_Primitive_t prim, int first, int count)
{
	if (count <= 0)
		return;
	if (!sState.valid || sState.tevDirty)
	{
		CommitPendingBatch();
		ApplyTexEnvNow();
		sState.valid = true;
	}
	if (prim == GPU_TRIANGLES)
	{
		if (sBatchActive && first == sBatchFirst + sBatchCount)
		{
			sBatchCount += count;
			if (sGameFrameOpen)
				PerfCTR::CountBatchMerge(static_cast<u32>(count));
			return;
		}
		CommitPendingBatch();
		sBatchActive = true;
		sBatchFirst  = first;
		sBatchCount  = count;
		if (sGameFrameOpen)
			PerfCTR::CountDrawCall(static_cast<u32>(count));
		return;
	}
	CommitPendingBatch();
	C3D_DrawArrays(prim, first, count);
	if (sGameFrameOpen)
		PerfCTR::CountDrawCall(static_cast<u32>(count));
	++sDrawsInFrame;
}

void InvalidateStateCache()
{
	CommitPendingBatch();
	std::memset(&sState, 0, sizeof(sState));
	sState.valid = false;
	sState.tevDirty = true;
	sState.texEnvMode = CTR_TEXENV_MODULATE;
	sState.texEnvColor = 0xFFFFFFFFu;
}

void RestoreDefaultGameState()
{
	InvalidateStateCache();
	for (int i = 1; i < 6; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_TexEnvBufUpdate(C3D_Both, 0);
	sActiveTarget = sTopTarget;
	if (sGameFrameOpen)
		C3D_FrameDrawOn(sTopTarget);
	C3D_BindProgram(&sProgram);
	SetupVertexAttribs();
	SetUv1Transform(1.0f, 1.0f, 0.0f, 0.0f);
	SetViewportLandscape(0, 0, 400, 240, 400, 240);
	SetScissorLandscape(true, 0, 0, 400, 240, 400, 240);
	SetDepthState(false, false, false);
	SetCullMode(false, true);
	SetBlendState(false);
	SetAlphaTest(false);
	SetTextureEnabled(true);
	SetTexEnvColor(0xFFFFFFFFu);
	SetTexEnvMode(CTR_TEXENV_REPLACE);
}

} // namespace Citro3D
