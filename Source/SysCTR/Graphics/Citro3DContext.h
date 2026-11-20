#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stddef.h>
#include <stdint.h>

// Texture environment modes matching N64/Daedalus BlendModes + RenderSettings.
enum ECtrTexEnvMode : u32
{
	CTR_TEXENV_MODULATE = 1,
	CTR_TEXENV_REPLACE  = 2,
	CTR_TEXENV_BLEND    = 3,
	CTR_TEXENV_ADD      = 4,
	CTR_TEXENV_DECAL    = 5,
};

namespace Citro3D
{
	bool Init();
	void Shutdown();
	bool IsInitialised();

	// Game frame lifecycle (single top-screen target during gameplay).
	void EnsureGameFrameBegun();
	void EndGameFrame();
	void SplitGameFrame();
	bool IsGameFrameActive();

	// UI frame lifecycle (top + bottom screens for FrontendCTR).
	void BeginUIFrame();
	void BeginUIScreen(gfxScreen_t screen, int width, int height, u32 clearRGBA8);
	void EndUIFrame();

	// Render target clears (clearRGBA8 is 0xRRGGBBAA; clearDepth is 24-bit, 0x00FFFFFF = far).
	void Clear(bool clearColor, bool clearDepth, u32 clearRGBA8 = 0x000000FFu, u32 clearDepthVal = 0x00FFFFFFu);

	// Landscape (400x240 / 320x240) viewport & scissor helpers (OpenGL bottom-left origin).
	void SetViewportLandscape(s32 x, s32 y, s32 w, s32 h, s32 screenW = 400, s32 screenH = 240);
	void SetScissorLandscape(bool enable, s32 x, s32 y, s32 w, s32 h, s32 screenW = 400, s32 screenH = 240);

	// Projection matrix upload.
	// SetProjectionGL converts an OpenGL column-major 4x4 matrix (NDC z in [-1, 1])
	// into a 3DS portrait-tilted, PICA200-depth ([-1, 0]) C3D_Mtx.
	void SetProjectionGL(const float * gl_mtx16);
	void SetProjectionMtx(const C3D_Mtx * mtx);

	// Cached PICA200 pipeline state setters.
	void SetDepthState(bool enable, bool writeMask, bool decal);
	void SetCullMode(bool enable, bool cullBack);
	void SetBlendState(bool enable, GPU_BLENDFACTOR src = GPU_SRC_ALPHA, GPU_BLENDFACTOR dst = GPU_ONE_MINUS_SRC_ALPHA);
	void SetAlphaTest(bool enable, GPU_TESTFUNC func = GPU_ALWAYS, u8 ref = 0);
	// Texture units & TEV pipeline.
	void SetTextureEnabled(bool enable);
	void BindTexture(C3D_Tex * tex);
	void BindTextureUnit(int unitId, C3D_Tex * tex);
	void SetUv1Transform(float scaleX, float scaleY, float offX, float offY);
	void SetTexEnvMode(u32 mode);
	void SetTexEnvColor(u32 colorABGR);
	void MarkCustomTevActive();
	bool IsScissorFullScreen(s32 screenW = 400, s32 screenH = 240);

	// Fog (currently no-op on CTR unless enabled later; kept for BaseRenderer interface).
	void SetFogEnable(bool enable);
	void SetFogMinMax(float fogNear, float fogFar);
	void SetFogColor(u32 colorABGR);

	// Vertex submission (DaedalusVtx 24-byte interleaved layout).
	void SetVertexBuffer(const void * data, size_t stride);
	void DrawArrays(GPU_Primitive_t prim, int first, int count);
	void FlushPendingBatch();
	void FlushDataCache(const void * ptr, size_t size);

	// Invalidate all cached PICA200 states (called on RestoreRenderStates / UI transitions).
	void InvalidateStateCache();
	void RestoreDefaultGameState();
}

// Inline compatibility shims for SysCTR/HLEGraphics/BlendModes.cpp so all
// game-specific blend overrides map straight to PICA200 TEV with zero GL overhead.
#ifndef CTR_GL_COMPAT_DEFINED
#define CTR_GL_COMPAT_DEFINED
#define GL_TEXTURE_ENV       0x2300u
#define GL_TEXTURE_ENV_MODE  0x2200u
#define GL_TEXTURE_ENV_COLOR 0x2201u
#define GL_MODULATE          CTR_TEXENV_MODULATE
#define GL_REPLACE           CTR_TEXENV_REPLACE
#define GL_BLEND             CTR_TEXENV_BLEND
#define GL_ADD               CTR_TEXENV_ADD
#define GL_DECAL             CTR_TEXENV_DECAL

static inline void glTexEnvi(u32 target, u32 pname, u32 param)
{
	(void)target;
	(void)pname;
	Citro3D::SetTexEnvMode(param);
}

static inline void glTexEnvfv(u32 target, u32 pname, const float * params)
{
	(void)target;
	(void)pname;
	if (!params)
		return;
	auto clampU8 = [](float f) -> u32 {
		if (f <= 0.0f) return 0u;
		if (f >= 1.0f) return 255u;
		return static_cast<u32>(f * 255.0f + 0.5f);
	};
	const u32 abgr = clampU8(params[0]) |
	                 (clampU8(params[1]) << 8) |
	                 (clampU8(params[2]) << 16) |
	                 (clampU8(params[3]) << 24);
	Citro3D::SetTexEnvColor(abgr);
}
#endif
