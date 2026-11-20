#include "stdafx.h"
#include "RendererCTR.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <new>

#include "Combiner/BlendConstant.h"
#include "Combiner/CombinerTree.h"
#include "Combiner/RenderSettings.h"
#include "Core/ROM.h"
#include "Debug/Dump.h"
#include "Graphics/GraphicsContext.h"
#include "Graphics/NativeTexture.h"
#include "HLEGraphics/CachedTexture.h"
#include "HLEGraphics/DLDebug.h"
#include "HLEGraphics/RDPStateManager.h"
#include "HLEGraphics/TextureCache.h"
#include "Math/MathUtil.h"
#include "OSHLE/ultra_gbi.h"
#include "SysCTR/Graphics/Citro3DContext.h"
#include "SysCTR/Utility/PerfProfilerCTR.h"
#include "Utility/IO.h"
#include "Utility/Profiler.h"

BaseRenderer *gRenderer    = nullptr;
RendererCTR  *gRendererCTR = nullptr;

extern DaedalusVtx *gVtxBuffer;
extern DaedalusVtx *gVtxBufferPtr;
extern uint32_t     gVertexCount;
extern uint32_t     gMaxVertices;

struct ScePspFMatrix4
{
	float m[16];
};

ScePspFMatrix4		gProjection;
static u32			gProjectionGen = 1;

void sceGuSetMatrix(int type, const ScePspFMatrix4 * mtx)
{
	if (type == 0)
	{
		memcpy(&gProjection, mtx, sizeof(gProjection));
		++gProjectionGen;
	}
}

void RendererCTR::InstallNativeTexture(const CNativeTexture * texture, u32 wrap_s, u32 wrap_t, int unit, bool setTexEnabled)
{
	if (unit == 0 && setTexEnabled)
		Citro3D::SetTextureEnabled(true);
	const u32 cycle_mode = gRDPOtherMode.cycle_type;
	const GPU_TEXTURE_FILTER_PARAM filt = (((gRDPOtherMode.text_filt != G_TF_POINT) && cycle_mode != CYCLE_COPY) ||
		gGlobalPreferences.ForceLinearFilter) ? GPU_LINEAR : GPU_NEAREST;
	texture->ApplyParams(filt, static_cast<GPU_TEXTURE_WRAP_PARAM>(wrap_s), static_cast<GPU_TEXTURE_WRAP_PARAM>(wrap_t));
	Citro3D::BindTextureUnit(unit, texture->GetC3DTexture());
}

void RendererCTR::ApplyProjectionMatrix(const float (&mat_project)[16])
{
	const float * ptr = mat_project;
	const u32 gen = (ptr == gProjection.m) ? gProjectionGen : 0u;
	if (mLastProjPtr != ptr || mLastProjGen != gen)
	{
		Citro3D::SetProjectionGL(ptr);
		mLastProjPtr = ptr;
		mLastProjGen = gen;
	}
}

void RendererCTR::InitBlenderModeCached()
{
	u32 cycle_type    = gRDPOtherMode.cycle_type;
	u32 cvg_x_alpha   = gRDPOtherMode.cvg_x_alpha;
	u32 alpha_cvg_sel = gRDPOtherMode.alpha_cvg_sel;
	u32 blendmode     = gRDPOtherMode.blender;

	// NB: If we're running in 1cycle mode, ignore the 2nd cycle.
	u32 active_mode = (cycle_type == CYCLE_2CYCLE) ? blendmode : (blendmode & 0xCCCC);

	if (alpha_cvg_sel && (gRDPOtherMode.L & 0x7000) != 0x7000) {
		switch (active_mode) {
		case 0x4055: // Mario Golf
		case 0x5055: // Paper Mario Intro
			Citro3D::SetBlendState(true, GPU_ZERO, GPU_ONE);
			break;
		default:
			Citro3D::SetBlendState(false);
		}
		return;
	}

	switch (active_mode)
	{
	case 0x00C0: // ISS 64
	case 0x0091: // Mace special blend mode
	case 0x0302: // Bomberman 2 special blend mode
	case 0x0382: // Mace objects
	case 0x07C2: // ISS 64
	case 0x0C08: // 1080 sky
	case 0xC302: // ISS 64
	case 0xC702: // Donald Duck: Quack Attack
	case 0xC800: // Conker's Bad Fur Day
	case 0x0F0A: // DK64 blueprints
	case 0xA500: // Sin and Punishment
	case 0xFA00: // Bomberman second attack
		Citro3D::SetBlendState(false);
		break;
	case 0x55F0: // Bust-A-Move 3 DX
		Citro3D::SetBlendState(true, GPU_ONE, GPU_SRC_ALPHA);
		break;
	case 0x0F1A:
		if (cycle_type == CYCLE_1CYCLE)
			Citro3D::SetBlendState(false);
		else
			Citro3D::SetBlendState(true, GPU_ZERO, GPU_ONE);
		break;
	case 0x0448: // Space Invaders
	case 0x0554:
		Citro3D::SetBlendState(true, GPU_ONE, GPU_ONE);
		break;
	case 0x0F5A: // Zelda: MM
	case 0x0FA5: // OOT menu
	case 0x5055: // Paper Mario intro
	case 0xAF50: // Zelda: MM
	case 0xC712: // Pokemon Stadium
		Citro3D::SetBlendState(true, GPU_ZERO, GPU_ONE);
		break;
	case 0x0C40: // Extreme-G
	case 0x0C48: // Star Wars: Shadow of the Empire text and hud
	case 0x4C40: // Wave Race
	case 0x5F50:
		Citro3D::SetBlendState(true, GPU_ZERO, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	case 0x0010: // Diddy Kong rare logo
	case 0x0040: // F-Zero X
	case 0x0050: // A Bug's Life
	case 0x0051:
	case 0x0055:
	case 0x0150: // Spiderman
	case 0x0321:
	case 0x0440: // Bomberman 64
	case 0x04D0: // Conker's Bad Fur Day
	case 0x0550: // Bomberman 64
	case 0x0C18: // StarFox 64 main menu
	case 0x0F54: // Star Wars racers
	case 0xC410: // Donald Duck: Quack Attack dust
	case 0xC440: // Banjo-Kazooie / Banjo-Tooie
	case 0xC810: // AeroGauge
	case 0xCB02: // Doom 64 weapons
	case 0x0D18:
	case 0x8410: // Paper Mario
	case 0xF550:
		if (!(!alpha_cvg_sel || cvg_x_alpha)) Citro3D::SetBlendState(false);
		else Citro3D::SetBlendState(true, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	case 0xC912: // 40 Winks
		Citro3D::SetBlendState(true, GPU_SRC_ALPHA, GPU_ONE);
		break;
	case 0x0C19:
	case 0xC811:
		Citro3D::SetBlendState(true, GPU_SRC_ALPHA, GPU_DST_ALPHA);
		break;
	case 0x5000: // V8 explosions
		Citro3D::SetBlendState(true, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA);
		break;
	default:
		if (!(!alpha_cvg_sel || cvg_x_alpha)) Citro3D::SetBlendState(false);
		else Citro3D::SetBlendState(true, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

RendererCTR::RendererCTR()
:	mLastProjPtr(nullptr)
,	mLastProjGen(0)
,	mLastTevProg(nullptr)
,	mLastTevPrim(0)
,	mLastTevEnv(0)
,	mLastTevPrimLod(0)
,	mFastStateValid(false)
,	mFastMux(0)
,	mFastOtherMode(0)
,	mFastTnlFlags(0)
,	mFastDisableZ(false)
,	mFastPrimCol(0)
,	mFastEnvCol(0)
,	mFastBlendCol(0)
,	mFastPrimLod(0)
,	mFastTex0(nullptr)
,	mFastTex1(nullptr)
,	mFastWrap0U(0)
,	mFastWrap0V(0)
,	mFastWrap1U(0)
,	mFastWrap1V(0)
,	mFastHasTex(false)
{
	// Set up RGB = T0, A = T0
	mCopyBlendStates = new CBlendStates;
	{
		CAlphaRenderSettings *	alpha_settings( new CAlphaRenderSettings( "Copy" ) );
		CRenderSettingsModulate *	colour_settings( new CRenderSettingsModulate( "Copy" ) );

		alpha_settings->AddTermTexel0();
		colour_settings->AddTermTexel0();

		mCopyBlendStates->SetAlphaSettings( alpha_settings );
		mCopyBlendStates->AddColourSettings( colour_settings );
	}

	// Set up RGB = Diffuse, A = Diffuse
	mFillBlendStates = new CBlendStates;
	{
		CAlphaRenderSettings *	alpha_settings( new CAlphaRenderSettings( "Fill" ) );
		CRenderSettingsModulate *	colour_settings( new CRenderSettingsModulate( "Fill" ) );

		alpha_settings->AddTermConstant( new CBlendConstantExpressionValue( BC_SHADE ) );
		colour_settings->AddTermConstant(  new CBlendConstantExpressionValue( BC_SHADE ) );

		mFillBlendStates->SetAlphaSettings( alpha_settings );
		mFillBlendStates->AddColourSettings( colour_settings );
	}
}

RendererCTR::~RendererCTR()
{
	for (u32 i = 0; i < kBlendTableSize; ++i)
	{
		if (mBlendTable[i].occupied && mBlendTable[i].entry.States != nullptr)
			delete mBlendTable[i].entry.States;
	}
	if (mOverflowEntry.States != nullptr)
		delete mOverflowEntry.States;
	delete mFillBlendStates;
	delete mCopyBlendStates;
}

void RendererCTR::RestoreRenderStates()
{
	Citro3D::RestoreDefaultGameState();
	mLastProjPtr    = nullptr;
	mLastProjGen    = 0;
	mLastTevProg    = nullptr;
	mFastStateValid = false;
}

const RendererCTR::SBlendStateEntry & RendererCTR::LookupBlendState( u64 mux, bool two_cycles )
{
	REG64 key;
	key._u64 = mux;
	// Top 8 bits are never set - use bit 31 of high word to distinguish 1-cycle vs 2-cycle.
	key._u32_1 |= (static_cast<u32>(two_cycles) << 31);

	const u32 mask = kBlendTableSize - 1;
	u32 slotIdx = (key._u32_0 ^ (key._u32_0 >> 11) ^ key._u32_1 ^ (key._u32_1 >> 13)) & mask;

	for (u32 probe = 0; probe < 16; ++probe)
	{
		SBlendTableSlot & slot = mBlendTable[(slotIdx + probe) & mask];
		if (slot.occupied)
		{
			if (slot.key == key._u64)
				return slot.entry;
			continue;
		}

		// Populate new entry.
		SBlendStateEntry & entry = slot.entry;
		entry = SBlendStateEntry();

		// 1) Preserve explicit forced game hacks from BlendModes.cpp (small curated list).
		OverrideBlendModeFn forced = LookupOverrideBlendModeForced( mux );
		if (forced != nullptr)
		{
			entry.OverrideFunction = forced;
		}
		else if (PicaTEV::CompileMux( mux, two_cycles, entry.TevProgram ))
		{
			// Exact native PICA200 multi-stage hardware TEV program compiled!
			// Zero multipass overdraw, zero CPU vertex-color rewriting!
		}
		else
		{
			// Fallback to Daedalus combiner tree / inexact override for unsupported inputs (NOISE/K5/etc.).
			CCombinerTree tree( mux, two_cycles );
			entry.States = tree.GetBlendStates();
			if (entry.States->IsInexact())
				entry.OverrideFunction = LookupOverrideBlendModeInexact( mux );
		}

		slot.key = key._u64;
		slot.occupied = true;
		return slot.entry;
	}

	// Rare overflow (more than 512 distinct muxes with 16-probe collision): compile into scratch slot.
	if (mOverflowEntry.States != nullptr)
	{
		delete mOverflowEntry.States;
		mOverflowEntry.States = nullptr;
	}
	mOverflowEntry = SBlendStateEntry();
	OverrideBlendModeFn forced = LookupOverrideBlendModeForced( mux );
	if (forced != nullptr)
	{
		mOverflowEntry.OverrideFunction = forced;
	}
	else if (!PicaTEV::CompileMux( mux, two_cycles, mOverflowEntry.TevProgram ))
	{
		CCombinerTree tree( mux, two_cycles );
		mOverflowEntry.States = tree.GetBlendStates();
		if (mOverflowEntry.States->IsInexact())
			mOverflowEntry.OverrideFunction = LookupOverrideBlendModeInexact( mux );
	}
	return mOverflowEntry;
}

void RendererCTR::DrawPrimitives(DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode, bool has_texture)
{
	(void)has_texture;
	if (num_vertices < 3 || gMaxVertices < 3)
		return;

	if (triangle_mode != GPU_TRIANGLES && triangle_mode != GPU_TRIANGLE_STRIP && triangle_mode != GPU_TRIANGLE_FAN)
		return;

	// Zero-copy fast path: BaseRenderer::PrepareTrisUnclipped wrote DaedalusVtx directly
	// into gVtxBuffer! Skip memcpy completely and submit straight from linear RAM.
	if (p_vertices == gVtxBuffer && triangle_mode == GPU_TRIANGLES &&
		num_vertices <= gMaxVertices && gVertexCount <= gMaxVertices - num_vertices)
	{
		const u32 count = num_vertices - (num_vertices % 3);
		if (count >= 3)
		{
			Citro3D::DrawArrays(GPU_TRIANGLES, static_cast<int>(gVertexCount), static_cast<int>(count));
			gVtxBuffer   += count;
			gVertexCount += count;
		}
		return;
	}

	u32 offset = 0;
	while (offset < num_vertices)
	{
		u32 remaining = num_vertices - offset;
		u32 count = remaining < gMaxVertices ? remaining : gMaxVertices;
		if (triangle_mode == GPU_TRIANGLE_FAN && offset != 0)
			count = (remaining < gMaxVertices - 1 ? remaining : gMaxVertices - 1) + 1;
		GPU_Primitive_t mode = triangle_mode;
		bool strip_triangle = false;

		if (triangle_mode == GPU_TRIANGLES)
		{
			count -= count % 3;
			if (count == 0)
				break;
		}
		else if (triangle_mode == GPU_TRIANGLE_STRIP && gMaxVertices == 3)
		{
			mode = GPU_TRIANGLES;
			strip_triangle = true;
		}
		else if (triangle_mode == GPU_TRIANGLE_STRIP && remaining > gMaxVertices)
		{
			count -= count % 2;
		}

		if (gVertexCount > gMaxVertices || count > gMaxVertices - gVertexCount)
		{
			if (gVertexCount > 0 && gVtxBufferPtr != nullptr)
				Citro3D::FlushDataCache(gVtxBufferPtr, gVertexCount * sizeof(DaedalusVtx));
			Citro3D::SplitGameFrame();
			CGraphicsContext::Get()->ResetVertexBuffer();
			Citro3D::SetVertexBuffer(gVtxBufferPtr, sizeof(DaedalusVtx));
			mLastProjPtr    = nullptr;
			mLastProjGen    = 0;
			mLastTevProg    = nullptr;
			mFastStateValid = false;
		}

		if (triangle_mode != GPU_TRIANGLE_FAN && !strip_triangle)
		{
			memcpy(gVtxBuffer, p_vertices + offset, count * sizeof(DaedalusVtx));
			gVtxBuffer += count;
		}
		else
		{
			DaedalusVtx * dst = gVtxBuffer;
			for (u32 i = 0; i < count; ++i)
			{
				u32 index;
				if (triangle_mode == GPU_TRIANGLE_FAN && offset != 0)
					index = i == 0 ? 0 : offset + (i - 1);
				else if (strip_triangle && (offset & 1) && i < 2)
					index = offset + 1 - i;
				else
					index = offset + i;

				dst[i] = p_vertices[index];
			}
			gVtxBuffer += count;
		}

		Citro3D::DrawArrays(mode, static_cast<int>(gVertexCount), static_cast<int>(count));
		gVertexCount += count;

		if (triangle_mode == GPU_TRIANGLES)
			offset += count;
		else if (triangle_mode == GPU_TRIANGLE_STRIP)
		{
			if (remaining <= count)
				break;
			offset += count - 2;
		}
		else
		{
			if (offset == 0)
			{
				if (remaining <= count)
					break;
				offset = count - 1;
			}
			else
			{
				if (remaining <= count - 1)
					break;
				offset += count - 2;
			}
		}
	}
}

void RendererCTR::RenderUsingRenderSettings( const CBlendStates * states, DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode)
{
	mLastTevProg = nullptr;
	const CAlphaRenderSettings *	alpha_settings( states->GetAlphaSettings() );

	SRenderState	state;

	state.Vertices = p_vertices;
	state.NumVertices = num_vertices;
	state.PrimitiveColour = mPrimitiveColour;
	state.EnvironmentColour = mEnvColour;

	const bool multipass = states->GetNumStates() > 1;
	u32 * saved_colours = mVtx_Save;
	if (multipass && num_vertices > sizeof(mVtx_Save) / sizeof(mVtx_Save[0]))
	{
		if (num_vertices > SIZE_MAX / sizeof(*saved_colours))
		{
			DAEDALUS_ERROR("Too many vertices to save multipass colours");
			return;
		}
		saved_colours = static_cast<u32 *>(malloc(num_vertices * sizeof(*saved_colours)));
		if (saved_colours == nullptr)
		{
			DAEDALUS_ERROR("Could not save multipass vertex colours");
			return;
		}
	}
	if (multipass)
	{
		for (u32 i = 0; i < num_vertices; ++i)
			saved_colours[i] = p_vertices[i].Colour.GetColour();
	}

	for( u32 i = 0; i < states->GetNumStates(); ++i )
	{
		const CRenderSettings *		settings( states->GetColourSettings( i ) );

		bool install_texture0( settings->UsesTexture0() || alpha_settings->UsesTexture0() );
		bool install_texture1( settings->UsesTexture1() || alpha_settings->UsesTexture1() );

		SRenderStateOut out = {};

		settings->Apply( install_texture0 || install_texture1, state, out );
		alpha_settings->Apply( install_texture0 || install_texture1, state, out );

		if( i > 0 )
		{
			for (u32 j = 0; j < num_vertices; ++j)
				p_vertices[j].Colour = c32(saved_colours[j]);
		}

		if(out.VertexExpressionRGB != nullptr)
		{
			out.VertexExpressionRGB->ApplyExpressionRGB( state );
		}
		if(out.VertexExpressionA != nullptr)
		{
			out.VertexExpressionA->ApplyExpressionAlpha( state );
		}

		bool installed_texture = false;

		u32 texture_idx;

		if(install_texture0 || install_texture1)
		{
			u32	tfx = CTR_TEXENV_MODULATE;
			switch( out.BlendMode )
			{
			case PBM_MODULATE:
				tfx = CTR_TEXENV_MODULATE;
				break;
			case PBM_REPLACE:
				tfx = CTR_TEXENV_REPLACE;
				break;
			case PBM_BLEND:
				tfx = CTR_TEXENV_BLEND;
				Citro3D::SetTexEnvColor(out.TextureFactor.GetColour());
				break;
			}

			Citro3D::SetTexEnvMode(tfx);

			if( g_ROM.T1_HACK )
			{
				texture_idx = install_texture1;
			}
			else
			{
				texture_idx = install_texture0 ? 0 : 1;
			}

			CRefPtr<CNativeTexture> texture;

			if(out.MakeTextureWhite)
			{
				TextureInfo white_ti = mBoundTextureInfo[ texture_idx ];
				white_ti.SetWhite(true);
				texture = CTextureCache::Get()->GetOrCreateTexture( white_ti );
			}
			else
			{
				texture = mBoundTexture[ texture_idx ];
			}

			if(texture != nullptr)
			{
				InstallNativeTexture(texture, mTexWrap[texture_idx].u, mTexWrap[texture_idx].v, 0);
				installed_texture = true;
			}
		}

		if( !installed_texture )
			Citro3D::SetTextureEnabled(false);

		DrawPrimitives(p_vertices, num_vertices, triangle_mode, installed_texture);
	}
	if (saved_colours != mVtx_Save)
		free(saved_colours);
}

void RendererCTR::RenderUsingCurrentBlendMode(const float (&mat_project)[16], DaedalusVtx * p_vertices, u32 num_vertices, GPU_Primitive_t triangle_mode, bool disable_zbuffer )
{
	ApplyProjectionMatrix(mat_project);

	const u32 primCol  = mPrimitiveColour.GetColour();
	const u32 envCol   = mEnvColour.GetColour();
	const u32 blendCol = mBlendColour.GetColour();
	const float lodF   = mPrimLODFraction < 0.0f ? 0.0f : (mPrimLODFraction > 1.0f ? 1.0f : mPrimLODFraction);
	const u8 primLod   = static_cast<u8>(lodF * 255.0f + 0.5f);

	// Fast path: when consecutive triangle draws share identical RDP state, mux,
	// colours, and bound textures, skip all state setup and go straight to DrawPrimitives
	// (which coalesces contiguous GPU_TRIANGLES into a single hardware draw call!).
	if (mFastStateValid &&
		mFastMux        == mMux &&
		mFastOtherMode  == gRDPOtherMode._u64 &&
		mFastTnlFlags   == (mTnL.Flags._u32 & TNL_ZBUFFER) &&
		mFastDisableZ   == disable_zbuffer &&
		mFastPrimCol    == primCol &&
		mFastEnvCol     == envCol &&
		mFastBlendCol   == blendCol &&
		mFastPrimLod    == primLod &&
		mFastTex0       == mBoundTexture[0] &&
		mFastTex1       == mBoundTexture[1] &&
		mFastWrap0U     == mTexWrap[0].u &&
		mFastWrap0V     == mTexWrap[0].v &&
		mFastWrap1U     == mTexWrap[1].u &&
		mFastWrap1V     == mTexWrap[1].v)
	{
		DrawPrimitives(p_vertices, num_vertices, triangle_mode, mFastHasTex);
		return;
	}
	mFastStateValid = false;

	if ( disable_zbuffer )
	{
		Citro3D::SetDepthState(false, false, false);
	}
	else
	{
		const bool decal = (gRDPOtherMode.zmode == 3);
		const bool depth_enable = ((mTnL.Flags.Zbuffer & gRDPOtherMode.z_cmp) || gRDPOtherMode.z_upd) != 0;
		const bool depth_mask = gRDPOtherMode.z_upd != 0;
		Citro3D::SetDepthState(depth_enable, depth_mask, decal);
	}

	u32 cycle_mode = gRDPOtherMode.cycle_type;

	// Initiate Blender
	if(cycle_mode < CYCLE_COPY && gRDPOtherMode.force_bl)
	{
		InitBlenderModeCached();
	}
	else if (gRDPOtherMode.clr_on_cvg)
	{
		if ((cycle_mode == CYCLE_1CYCLE && gRDPOtherMode.c1_m2a == 1) ||
		    (cycle_mode == CYCLE_2CYCLE && gRDPOtherMode.c2_m2a == 1))
			Citro3D::SetBlendState(true, GPU_ZERO, GPU_ONE);
		else
			Citro3D::SetBlendState(false);
	}
	else
	{
		Citro3D::SetBlendState(false);
	}

	// Initiate Alpha test
	if( (gRDPOtherMode.alpha_compare == G_AC_THRESHOLD) && !gRDPOtherMode.alpha_cvg_sel )
	{
		u8 alpha_threshold = mBlendColour.GetA();
		Citro3D::SetAlphaTest(true, (alpha_threshold || g_ROM.ALPHA_HACK) ? GPU_GEQUAL : GPU_GREATER, alpha_threshold);
	}
	else if (gRDPOtherMode.cvg_x_alpha)
	{
		Citro3D::SetAlphaTest(true, GPU_GEQUAL, 11);
	}
	else
	{
		Citro3D::SetAlphaTest(false);
	}

	// Fast paths for CYCLE_COPY and CYCLE_FILL (avoid mux lookup completely).
	if (cycle_mode == CYCLE_COPY)
	{
		mLastTevProg = nullptr;
		if (mBoundTexture[0] != nullptr)
		{
			Citro3D::SetTexEnvMode(CTR_TEXENV_REPLACE);
			InstallNativeTexture(mBoundTexture[0], mTexWrap[0].u, mTexWrap[0].v, 0);

			mFastStateValid = true;
			mFastMux        = mMux;
			mFastOtherMode  = gRDPOtherMode._u64;
			mFastTnlFlags   = (mTnL.Flags._u32 & TNL_ZBUFFER);
			mFastDisableZ   = disable_zbuffer;
			mFastPrimCol    = primCol;
			mFastEnvCol     = envCol;
			mFastBlendCol   = blendCol;
			mFastPrimLod    = primLod;
			mFastTex0       = mBoundTexture[0];
			mFastTex1       = mBoundTexture[1];
			mFastWrap0U     = mTexWrap[0].u;
			mFastWrap0V     = mTexWrap[0].v;
			mFastWrap1U     = mTexWrap[1].u;
			mFastWrap1V     = mTexWrap[1].v;
			mFastHasTex     = true;

			DrawPrimitives(p_vertices, num_vertices, triangle_mode, true);
		}
		else
		{
			Citro3D::SetTextureEnabled(false);
			DrawPrimitives(p_vertices, num_vertices, triangle_mode, false);
		}
		return;
	}

	if (cycle_mode == CYCLE_FILL)
	{
		mLastTevProg = nullptr;
		Citro3D::SetTextureEnabled(false);
		DrawPrimitives(p_vertices, num_vertices, triangle_mode, false);
		return;
	}

	const SBlendStateEntry & blend_entry = LookupBlendState( mMux, cycle_mode == CYCLE_2CYCLE );

	// 1) Native PICA200 multi-stage hardware TEV fast path:
	//    Single GPU pass, dual simultaneous texture units, live hardware constants,
	//    ZERO CPU vertex-color rewriting!
	if (blend_entry.TevProgram.valid)
	{
		const SPicaTevProgram & prog = blend_entry.TevProgram;
		bool texturesOk = true;

		if (prog.usesTex0)
		{
			if (mBoundTexture[0] != nullptr)
				InstallNativeTexture(mBoundTexture[0], mTexWrap[0].u, mTexWrap[0].v, 0, false);
			else
				texturesOk = false;
		}
		if (prog.usesTex1 && texturesOk)
		{
			if (mBoundTexture[1] != nullptr)
			{
				InstallNativeTexture(mBoundTexture[1], mTexWrap[1].u, mTexWrap[1].v, 1, false);

				// Compute exact TEXUNIT1 UV transform relative to TEXUNIT0 vertices.
				const CNativeTexture * t0 = mBoundTexture[0];
				const CNativeTexture * t1 = mBoundTexture[1];
				if (t0 != nullptr)
				{
					const float s0x = t0->GetScaleX();
					const float s0y = t0->GetScaleY();
					const float s1x = t1->GetScaleX();
					const float s1y = t1->GetScaleY();
					const float rx = (s0x > 0.0f) ? (s1x / s0x) : 1.0f;
					const float ry = (s0y > 0.0f) ? (s1y / s0y) : 1.0f;
					const float off0x = mTileTopLeft[0].s * (0.25f * s0x);
					const float off0y = mTileTopLeft[0].t * (0.25f * s0y);
					const float off1x = mTileTopLeft[1].s * (0.25f * s1x);
					const float off1y = mTileTopLeft[1].t * (0.25f * s1y);
					Citro3D::SetUv1Transform(rx, ry, off0x * rx - off1x, off0y * ry - off1y);
				}
				else
				{
					Citro3D::SetUv1Transform(1.0f, 1.0f, 0.0f, 0.0f);
				}
			}
			else if (mBoundTexture[0] != nullptr)
			{
				// Some games reference TEXEL1 while only tile 0 is bound; mirror tile 0 safely.
				InstallNativeTexture(mBoundTexture[0], mTexWrap[0].u, mTexWrap[0].v, 1, false);
				Citro3D::SetUv1Transform(1.0f, 1.0f, 0.0f, 0.0f);
			}
			else
			{
				texturesOk = false;
			}
		}

		if (texturesOk)
		{
			if (mLastTevProg != &prog || mLastTevPrim != primCol || mLastTevEnv != envCol || mLastTevPrimLod != primLod)
			{
				PicaTEV::ApplyProgram(prog, primCol, envCol, primLod);
				mLastTevProg    = &prog;
				mLastTevPrim    = primCol;
				mLastTevEnv     = envCol;
				mLastTevPrimLod = primLod;
			}
			Citro3D::MarkCustomTevActive();

			mFastStateValid = !prog.usesTex1; // Cache fast-path for single-tex / untextured TEV programs
			mFastMux        = mMux;
			mFastOtherMode  = gRDPOtherMode._u64;
			mFastTnlFlags   = (mTnL.Flags._u32 & TNL_ZBUFFER);
			mFastDisableZ   = disable_zbuffer;
			mFastPrimCol    = primCol;
			mFastEnvCol     = envCol;
			mFastBlendCol   = blendCol;
			mFastPrimLod    = primLod;
			mFastTex0       = mBoundTexture[0];
			mFastTex1       = mBoundTexture[1];
			mFastWrap0U     = mTexWrap[0].u;
			mFastWrap0V     = mTexWrap[0].v;
			mFastWrap1U     = mTexWrap[1].u;
			mFastWrap1V     = mTexWrap[1].v;
			mFastHasTex     = prog.usesTex0 || prog.usesTex1;

			DrawPrimitives(p_vertices, num_vertices, triangle_mode, mFastHasTex);
			return;
		}
	}

	// 2) Forced override / fallback path for unsupported muxes (NOISE, K4/K5, >6 stages) or missing textures.
	if( blend_entry.OverrideFunction != nullptr )
	{
		mLastTevProg = nullptr;
		SBlendModeDetails details;

		details.EnvColour = mEnvColour;
		details.PrimColour = mPrimitiveColour;
		details.InstallTexture = true;
		details.ColourAdjuster.Reset();

		blend_entry.OverrideFunction( details );

		bool installed_texture = false;

		if( details.InstallTexture )
		{
			int texture_idx = g_ROM.T1_HACK ? 1 : 0;

			if( mBoundTexture[ texture_idx ] )
			{
				InstallNativeTexture(mBoundTexture[ texture_idx ], mTexWrap[texture_idx].u, mTexWrap[texture_idx].v, 0);
				installed_texture = true;
			}
		}

		if( !installed_texture )
		{
			Citro3D::SetTextureEnabled(false);
		}

		details.ColourAdjuster.Process(p_vertices, num_vertices);
		DrawPrimitives(p_vertices, num_vertices, triangle_mode, installed_texture);
	}
	else if( blend_entry.States != nullptr )
	{
		RenderUsingRenderSettings( blend_entry.States, p_vertices, num_vertices, triangle_mode );
	}
	else
	{
		mLastTevProg = nullptr;
		Citro3D::SetTextureEnabled(false);
		DrawPrimitives(p_vertices, num_vertices, triangle_mode, false);
	}
}

void RendererCTR::RenderTriangles(DaedalusVtx *p_vertices, u32 num_vertices, bool disable_zbuffer)
{
	const u64 drw_t0 = svcGetSystemTick();
	RenderUsingCurrentBlendMode(gProjection.m, p_vertices, num_vertices, GPU_TRIANGLES, disable_zbuffer);
	PerfCTR::AddDraw(svcGetSystemTick() - drw_t0);
}

void RendererCTR::TexRect(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1)
{
#ifdef DAEDALUS_CTR_XRAY
	const u64 r_t0 = svcGetSystemTick();
#endif
	UpdateTileSnapshots( tile_idx );

	CNativeTexture *texture = mBoundTexture[0];
	if (texture == nullptr)
		return;

	PrepareTexRectUVs(&st0, &st1);

	v2 uv0( (float)st0.s / 32.f, (float)st0.t / 32.f );
	v2 uv1( (float)st1.s / 32.f, (float)st1.t / 32.f );

	v2 screen0;
	v2 screen1;

	if( gGlobalPreferences.ViewportType == VT_FULLSCREEN_HD )
	{
		screen0.x = roundf( roundf( HD_SCALE * xy0.x ) * mN64ToScreenScale.x + 40 );
		screen0.y = roundf( roundf( xy0.y )            * mN64ToScreenScale.y + mN64ToScreenTranslate.y );

		screen1.x = roundf( roundf( HD_SCALE * xy1.x ) * mN64ToScreenScale.x + 40 );
		screen1.y = roundf( roundf( xy1.y )            * mN64ToScreenScale.y + mN64ToScreenTranslate.y );
	}
	else
	{
		ConvertN64ToScreen( xy0, screen0 );
		ConvertN64ToScreen( xy1, screen1 );
	}

	const f32 depth = gRDPOtherMode.depth_source ? mPrimDepth : 0.0f;

	const float scale_x = texture->GetScaleX();
	const float scale_y = texture->GetScaleY();
	const float u0 = uv0.x * scale_x;
	const float v0 = uv0.y * scale_y;
	const float u1 = uv1.x * scale_x;
	const float v1 = uv1.y * scale_y;

	// Write directly into gVtxBuffer when space allows (zero-copy fast path in DrawPrimitives).
	DaedalusVtx stack_verts[6];
	DaedalusVtx * p_vertices = (gVtxBuffer != nullptr && gVertexCount <= gMaxVertices - 6) ? gVtxBuffer : stack_verts;

	p_vertices[0].Texture.x = u0;
	p_vertices[0].Texture.y = v0;
	p_vertices[0].Position.x = screen0.x;
	p_vertices[0].Position.y = screen0.y;
	p_vertices[0].Position.z = depth;
	p_vertices[0].Colour = c32(0xffffffff);

	p_vertices[1].Texture.x = u1;
	p_vertices[1].Texture.y = v0;
	p_vertices[1].Position.x = screen1.x;
	p_vertices[1].Position.y = screen0.y;
	p_vertices[1].Position.z = depth;
	p_vertices[1].Colour = c32(0xffffffff);

	p_vertices[2].Texture.x = u0;
	p_vertices[2].Texture.y = v1;
	p_vertices[2].Position.x = screen0.x;
	p_vertices[2].Position.y = screen1.y;
	p_vertices[2].Position.z = depth;
	p_vertices[2].Colour = c32(0xffffffff);

	p_vertices[3] = p_vertices[2];
	p_vertices[4] = p_vertices[1];

	p_vertices[5].Texture.x = u1;
	p_vertices[5].Texture.y = v1;
	p_vertices[5].Position.x = screen1.x;
	p_vertices[5].Position.y = screen1.y;
	p_vertices[5].Position.z = depth;
	p_vertices[5].Colour = c32(0xffffffff);

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 6, GPU_TRIANGLES, gRDPOtherMode.depth_source ? false : true);
#ifdef DAEDALUS_CTR_XRAY
	PerfCTR::AddDlRect(svcGetSystemTick() - r_t0);
#endif
}

void RendererCTR::TexRectFlip(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1)
{
#ifdef DAEDALUS_CTR_XRAY
	const u64 r_t0 = svcGetSystemTick();
#endif
	UpdateTileSnapshots( tile_idx );

	CNativeTexture *texture = mBoundTexture[0];
	if (texture == nullptr)
		return;

	PrepareTexRectUVs(&st0, &st1);

	v2 uv0( (float)st0.s / 32.f, (float)st0.t / 32.f );
	v2 uv1( (float)st1.s / 32.f, (float)st1.t / 32.f );

	v2 screen0;
	v2 screen1;
	ConvertN64ToScreen( xy0, screen0 );
	ConvertN64ToScreen( xy1, screen1 );

	float scale_x = texture->GetScaleX();
	float scale_y = texture->GetScaleY();

	DaedalusVtx p_vertices[6];

	p_vertices[0].Position.x = screen0.x;
	p_vertices[0].Position.y = screen0.y;
	p_vertices[0].Position.z = 0.0f;
	p_vertices[0].Colour = c32(0xffffffff);
	p_vertices[0].Texture.x = uv0.x * scale_x;
	p_vertices[0].Texture.y = uv0.y * scale_y;

	p_vertices[1].Position.x = screen1.x;
	p_vertices[1].Position.y = screen0.y;
	p_vertices[1].Position.z = 0.0f;
	p_vertices[1].Colour = c32(0xffffffff);
	p_vertices[1].Texture.x = uv0.x * scale_x;
	p_vertices[1].Texture.y = uv1.y * scale_y;

	p_vertices[2].Position.x = screen0.x;
	p_vertices[2].Position.y = screen1.y;
	p_vertices[2].Position.z = 0.0f;
	p_vertices[2].Colour = c32(0xffffffff);
	p_vertices[2].Texture.x = uv1.x * scale_x;
	p_vertices[2].Texture.y = uv0.y * scale_y;

	p_vertices[3] = p_vertices[2];
	p_vertices[4] = p_vertices[1];

	p_vertices[5].Position.x = screen1.x;
	p_vertices[5].Position.y = screen1.y;
	p_vertices[5].Position.z = 0.0f;
	p_vertices[5].Colour = c32(0xffffffff);
	p_vertices[5].Texture.x = uv1.x * scale_x;
	p_vertices[5].Texture.y = uv1.y * scale_y;

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 6, GPU_TRIANGLES, gRDPOtherMode.depth_source ? false : true);
#ifdef DAEDALUS_CTR_XRAY
	PerfCTR::AddDlRect(svcGetSystemTick() - r_t0);
#endif
}

void RendererCTR::FillRect(const v2 & xy0, const v2 & xy1, u32 color)
{
	v2 screen0;
	v2 screen1;
	ScaleN64ToScreen( xy0, screen0 );
	ScaleN64ToScreen( xy1, screen1 );

	// Hardware DMA fast-path: a full-screen opaque CYCLE_FILL FillRect is a color clear;
	// use GX_MemoryFill via C3D_RenderTargetClear instead of rasterizing 96,000 pixels!
	if (gRDPOtherMode.cycle_type == CYCLE_FILL &&
		screen0.x <= 0.5f && screen0.y <= 0.5f &&
		screen1.x >= (float)mScreenWidth - 0.5f && screen1.y >= (float)mScreenHeight - 0.5f &&
		Citro3D::IsScissorFullScreen(400, 240))
	{
		const c32 c(color);
		const u32 clearRGBA8 = ((u32)c.GetR() << 24) |
		                       ((u32)c.GetG() << 16) |
		                       ((u32)c.GetB() << 8)  |
		                       ((u32)c.GetA());
		Citro3D::Clear(true, false, clearRGBA8);
		return;
	}

	DaedalusVtx p_vertices[6];

	p_vertices[0].Position.x = screen0.x;
	p_vertices[0].Position.y = screen0.y;
	p_vertices[0].Position.z = 0.0f;
	p_vertices[0].Colour = c32(color);
	p_vertices[0].Texture.x = 0.0f;
	p_vertices[0].Texture.y = 0.0f;

	p_vertices[1].Position.x = screen1.x;
	p_vertices[1].Position.y = screen0.y;
	p_vertices[1].Position.z = 0.0f;
	p_vertices[1].Colour = c32(color);
	p_vertices[1].Texture.x = 0.0f;
	p_vertices[1].Texture.y = 0.0f;

	p_vertices[2].Position.x = screen0.x;
	p_vertices[2].Position.y = screen1.y;
	p_vertices[2].Position.z = 0.0f;
	p_vertices[2].Colour = c32(color);
	p_vertices[2].Texture.x = 0.0f;
	p_vertices[2].Texture.y = 0.0f;

	p_vertices[3] = p_vertices[2];
	p_vertices[4] = p_vertices[1];

	p_vertices[5].Position.x = screen1.x;
	p_vertices[5].Position.y = screen1.y;
	p_vertices[5].Position.z = 0.0f;
	p_vertices[5].Colour = c32(color);
	p_vertices[5].Texture.x = 0.0f;
	p_vertices[5].Texture.y = 0.0f;

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 6, GPU_TRIANGLES, true);
}

void RendererCTR::Draw2DTexture(f32 x0, f32 y0, f32 x1, f32 y1,
								f32 u0, f32 v0, f32 u1, f32 v1,
								const CNativeTexture * texture)
{
	if (texture == nullptr)
		return;
	mFastStateValid = false;
	InstallNativeTexture(texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE, 0);

	float scale_x = texture->GetScaleX();
	float scale_y = texture->GetScaleY();

	float sx0 = N64ToScreenX(x0);
	float sy0 = N64ToScreenY(y0);

	float sx1 = N64ToScreenX(x1);
	float sy1 = N64ToScreenY(y1);

	DaedalusVtx p_vertices[4];

	p_vertices[0].Position.x = sx0;
	p_vertices[0].Position.y = sy0;
	p_vertices[0].Position.z = 0.0f;
	p_vertices[0].Colour = c32(0xffffffff);
	p_vertices[0].Texture.x = u0 * scale_x;
	p_vertices[0].Texture.y = v0 * scale_y;

	p_vertices[1].Position.x = sx1;
	p_vertices[1].Position.y = sy0;
	p_vertices[1].Position.z = 0.0f;
	p_vertices[1].Colour = c32(0xffffffff);
	p_vertices[1].Texture.x = u1 * scale_x;
	p_vertices[1].Texture.y = v0 * scale_y;

	p_vertices[2].Position.x = sx0;
	p_vertices[2].Position.y = sy1;
	p_vertices[2].Position.z = 0.0f;
	p_vertices[2].Colour = c32(0xffffffff);
	p_vertices[2].Texture.x = u0 * scale_x;
	p_vertices[2].Texture.y = v1 * scale_y;

	p_vertices[3].Position.x = sx1;
	p_vertices[3].Position.y = sy1;
	p_vertices[3].Position.z = 0.0f;
	p_vertices[3].Colour = c32(0xffffffff);
	p_vertices[3].Texture.x = u1 * scale_x;
	p_vertices[3].Texture.y = v1 * scale_y;

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GPU_TRIANGLE_STRIP, true);
}

void RendererCTR::Draw2DTextureR(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2,
								 f32 y2, f32 x3, f32 y3, f32 s, f32 t,
								 const CNativeTexture * texture)
{
	if (texture == nullptr)
		return;
	mFastStateValid = false;
	InstallNativeTexture(texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE, 0);

	float scale_x = texture->GetScaleX();
	float scale_y = texture->GetScaleY();

	DaedalusVtx p_vertices[4];

	p_vertices[0].Position.x = N64ToScreenX(x0);
	p_vertices[0].Position.y = N64ToScreenY(y0);
	p_vertices[0].Position.z = 0.0f;
	p_vertices[0].Colour = c32(0xffffffff);
	p_vertices[0].Texture.x = 0.0f;
	p_vertices[0].Texture.y = 0.0f;

	p_vertices[1].Position.x = N64ToScreenX(x1);
	p_vertices[1].Position.y = N64ToScreenY(y1);
	p_vertices[1].Position.z = 0.0f;
	p_vertices[1].Colour = c32(0xffffffff);
	p_vertices[1].Texture.x = s * scale_x;
	p_vertices[1].Texture.y = 0.0f;

	p_vertices[2].Position.x = N64ToScreenX(x2);
	p_vertices[2].Position.y = N64ToScreenY(y2);
	p_vertices[2].Position.z = 0.0f;
	p_vertices[2].Colour = c32(0xffffffff);
	p_vertices[2].Texture.x = s * scale_x;
	p_vertices[2].Texture.y = t * scale_y;

	p_vertices[3].Position.x = N64ToScreenX(x3);
	p_vertices[3].Position.y = N64ToScreenY(y3);
	p_vertices[3].Position.z = 0.0f;
	p_vertices[3].Colour = c32(0xffffffff);
	p_vertices[3].Texture.x = 0.0f;
	p_vertices[3].Texture.y = t * scale_y;

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GPU_TRIANGLE_FAN, true);
}

bool CreateRenderer()
{
	void * storage = memalign(alignof(RendererCTR), sizeof(RendererCTR));
	if (storage == nullptr)
		return false;
	gRendererCTR = new (storage) RendererCTR();
	gRenderer = gRendererCTR;
	return true;
}

void DestroyRenderer()
{
	if (gRendererCTR != nullptr)
	{
		gRendererCTR->~RendererCTR();
		free(gRendererCTR);
	}
	gRendererCTR = nullptr;
	gRenderer    = nullptr;
}
