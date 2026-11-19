#pragma once

#include <map>
#include <set>

#include "HLEGraphics/BaseRenderer.h"
#include "SysCTR/HLEGraphics/BlendModes.h"

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
		SBlendStateEntry() : OverrideFunction( NULL ), States( NULL ) {}
		OverrideBlendModeFn			OverrideFunction;
		const CBlendStates *		States;
	};

	SBlendStateEntry	LookupBlendState( u64 mux, bool two_cycles );

private:
	void				RenderUsingCurrentBlendMode(const float (&mat_project)[16], DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode, bool disable_zbuffer );
	void				RenderUsingRenderSettings( const CBlendStates * states, DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode );
	void                DrawPrimitives(DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode, bool has_texture);
	
	// Original colours for the common multipass case; expressions only modify Colour.
	u32					mVtx_Save[320];
	
	// BlendMode support
	//
	CBlendStates *		mCopyBlendStates;
	CBlendStates *		mFillBlendStates;

	typedef std::map< u64, SBlendStateEntry > BlendStatesMap;
	BlendStatesMap		mBlendStatesMap;

	static const u32	kBlendCacheSize = 64;
	struct SBlendCacheSlot
	{
		SBlendCacheSlot() : key( 0 ), valid( false ) {}
		u64				key;
		SBlendStateEntry entry;
		bool			valid;
	};
	SBlendCacheSlot		mBlendCache[kBlendCacheSize];

	// Lightweight GL state cache to avoid redundant picaGL state changes per draw.
	struct SGlStateCache
	{
		bool   valid;
		bool   depthTest;
		bool   depthMask;
		bool   decalOffset;
		bool   blend;
		u32    blendSrc;
		u32    blendDst;
		bool   alphaTest;
		u32    alphaFunc;
		float  alphaRef;
		bool   tex2d;
		bool   mForceTexRebind;
		u32    texEnvMode;
		const float * projPtr;
		u32    projGen;
	};
	SGlStateCache		mGlCache;

	void				ApplyBlendState(bool enable, u32 src = 0, u32 dst = 0);
	void				ApplyDepthState(bool enable, bool mask, bool decal);
	void				ApplyAlphaState(bool enable, u32 func = 0, float ref = 0.0f);
	void				ApplyTexture2DState(bool enable);
	void				InstallNativeTexture(const CNativeTexture * texture, u32 wrap_s = 0, u32 wrap_t = 0);
	void				ApplyTexEnvMode(u32 mode);
	void				ApplyProjectionMatrix(const float (&mat_project)[16]);
	void				InitBlenderModeCached();
};

// NB: this is equivalent to gRenderer, but points to the implementation class, for platform-specific functionality.
extern RendererCTR * gRendererCTR;
