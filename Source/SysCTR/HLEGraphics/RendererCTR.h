#pragma once

#include <citro3d.h>

#include "HLEGraphics/BaseRenderer.h"
#include "SysCTR/HLEGraphics/BlendModes.h"
#include "SysCTR/HLEGraphics/Combiner/PicaTEVCombiner.h"

class CBlendStates;

struct DebugBlendSettings
{
	u32 TexInstall;	//defaults to texture installed
	u32	SetRGB;		//defaults to OFF
	u32	SetA;		//defaults to OFF
	u32	SetRGBA;	//defaults to OFF
	u32	ModRGB;		//defaults to OFF
	u32	ModA;		//defaults to OFF
	u32	ModRGBA;	//defaults to OFF
	u32	SubRGB;		//defaults to OFF
	u32	SubA;		//defaults to OFF
	u32	SubRGBA;	//defaults to OFF
	u32	AOpaque;	//defaults to OFF
	u32	sceENV;		//defaults to OFF
	u32	TXTFUNC;	//defaults to MODULATE_RGB
	u32 ForceRGB;	//defaults to OFF
};

class RendererCTR : public BaseRenderer
{
public:
	RendererCTR();
	~RendererCTR();

	virtual void		RestoreRenderStates();

	virtual void		RenderTriangles(DaedalusVtx * p_vertices, u32 num_vertices, bool disable_zbuffer);

	virtual void		TexRect(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1);
	virtual void		TexRectFlip(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1);
	virtual void		FillRect(const v2 & xy0, const v2 & xy1, u32 color);

	virtual void		Draw2DTexture(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, const CNativeTexture * texture);
	virtual void		Draw2DTextureR(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, f32 s, f32 t, const CNativeTexture * texture);

	struct SBlendStateEntry
	{
		SBlendStateEntry() : OverrideFunction( NULL ), States( NULL ) { TevProgram.valid = false; }
		OverrideBlendModeFn			OverrideFunction;
		const CBlendStates *		States;
		SPicaTevProgram				TevProgram;
	};

	const SBlendStateEntry & LookupBlendState( u64 mux, bool two_cycles );

private:
	void				RenderUsingCurrentBlendMode(const float (&mat_project)[16], DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode, bool disable_zbuffer );
	void				RenderUsingRenderSettings( const CBlendStates * states, DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode );
	void				DrawPrimitives(DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode, bool has_texture);

	// Original colours for the fallback multipass case; expressions only modify Colour.
	u32					mVtx_Save[320];

	// BlendMode support
	CBlendStates *		mCopyBlendStates;
	CBlendStates *		mFillBlendStates;

	// Flat open-addressed hash table (zero heap pointer chasing on ARM11).
	static const u32	kBlendTableSize = 512;
	struct SBlendTableSlot
	{
		SBlendTableSlot() : key( 0 ), occupied( false ) {}
		u64					key;
		SBlendStateEntry	entry;
		bool				occupied;
	};
	SBlendTableSlot		mBlendTable[kBlendTableSize];
	SBlendStateEntry	mOverflowEntry;

	// Projection & TEV state caches to avoid redundant GPU uploads.
	const float *		mLastProjPtr;
	u32					mLastProjGen;
	const SPicaTevProgram * mLastTevProg;
	u32					mLastTevPrim;
	u32					mLastTevEnv;
	u8					mLastTevPrimLod;

	// Fast-path signature cache for back-to-back RenderUsingCurrentBlendMode calls.
	bool				mFastStateValid;
	u64					mFastMux;
	u64					mFastOtherMode;
	u32					mFastTnlFlags;
	bool				mFastDisableZ;
	u32					mFastPrimCol;
	u32					mFastEnvCol;
	u32					mFastBlendCol;
	u8					mFastPrimLod;
	const CNativeTexture * mFastTex0;
	const CNativeTexture * mFastTex1;
	u32					mFastWrap0U;
	u32					mFastWrap0V;
	u32					mFastWrap1U;
	u32					mFastWrap1V;
	u32					mFastTile0ST;
	u32					mFastTile1ST;
	u8					mFastConstMask;
	bool				mFastUsesTex0;
	bool				mFastUsesTex1;
	bool				mFastHasTex;

	void				InstallNativeTexture(const CNativeTexture * texture, u32 wrap_s = GPU_CLAMP_TO_EDGE, u32 wrap_t = GPU_CLAMP_TO_EDGE, int unit = 0, bool setTexEnabled = true);
	void				ApplyProjectionMatrix(const float (&mat_project)[16]);
	void				InitBlenderModeCached();
};

// NB: this is equivalent to gRenderer, but points to the implementation class, for platform-specific functionality.
extern RendererCTR * gRendererCTR;
