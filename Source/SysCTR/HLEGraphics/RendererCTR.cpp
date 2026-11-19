#include "stdafx.h"
#include "RendererCTR.h"

#include <GL/picaGL.h>
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
#include "Utility/IO.h"
#include "Utility/Profiler.h"

BaseRenderer *gRenderer    = nullptr;
RendererCTR  *gRendererCTR = nullptr;

extern DaedalusVtx *gVtxBuffer;
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
	if (type == GL_PROJECTION)
	{
		memcpy(&gProjection, mtx, sizeof(gProjection));
		++gProjectionGen;
	}
}

void RendererCTR::ApplyBlendState(bool enable, u32 src, u32 dst)
{
	if (!mGlCache.valid || mGlCache.blend != enable)
	{
		if (enable) glEnable(GL_BLEND);
		else glDisable(GL_BLEND);
		mGlCache.blend = enable;
	}
	if (enable && (!mGlCache.valid || mGlCache.blendSrc != src || mGlCache.blendDst != dst))
	{
		glBlendFunc(src, dst);
		mGlCache.blendSrc = src;
		mGlCache.blendDst = dst;
	}
}

void RendererCTR::ApplyDepthState(bool enable, bool mask, bool decal)
{
	if (!mGlCache.valid || mGlCache.decalOffset != decal)
	{
		if (decal) glPolygonOffset(-1.0f, -1.0f);
		else glPolygonOffset(0.0f, 0.0f);
		mGlCache.decalOffset = decal;
	}
	if (!mGlCache.valid || mGlCache.depthTest != enable)
	{
		if (enable) glEnable(GL_DEPTH_TEST);
		else glDisable(GL_DEPTH_TEST);
		mGlCache.depthTest = enable;
	}
	if (!mGlCache.valid || mGlCache.depthMask != mask)
	{
		glDepthMask(mask ? GL_TRUE : GL_FALSE);
		mGlCache.depthMask = mask;
	}
}

void RendererCTR::ApplyAlphaState(bool enable, u32 func, float ref)
{
	if (!mGlCache.valid || mGlCache.alphaTest != enable)
	{
		if (enable) glEnable(GL_ALPHA_TEST);
		else glDisable(GL_ALPHA_TEST);
		mGlCache.alphaTest = enable;
	}
	if (enable && (!mGlCache.valid || mGlCache.alphaFunc != func || mGlCache.alphaRef != ref))
	{
		glAlphaFunc(func, ref);
		mGlCache.alphaFunc = func;
		mGlCache.alphaRef = ref;
	}
}

void RendererCTR::ApplyTexture2DState(bool enable)
{
	if (!mGlCache.valid || mGlCache.tex2d != enable)
	{
		if (enable)
		{
			glEnable(GL_TEXTURE_2D);
		}
		else
		{
			glDisable(GL_TEXTURE_2D);
			// picaGL clears TEXUNIT0 enable without marking dirty bit 0x80 on the
			// subsequent glEnable(GL_TEXTURE_2D); remember to force a rebind.
			mGlCache.mForceTexRebind = true;
		}
		mGlCache.tex2d = enable;
	}
}

void RendererCTR::InstallNativeTexture(const CNativeTexture * texture, u32 wrap_s, u32 wrap_t)
{
	ApplyTexture2DState(true);
	if (mGlCache.mForceTexRebind)
	{
		glBindTexture(GL_TEXTURE_2D, 0);
		mGlCache.mForceTexRebind = false;
	}
	// Always call InstallTexture() because CNativeTexture::SetData() can bind a
	// newly uploaded texture outside RendererCTR; picaGL already no-ops same binds.
	texture->InstallTexture();
	const u32 cycle_mode = gRDPOtherMode.cycle_type;
	const GLint filt = (((gRDPOtherMode.text_filt != G_TF_POINT) && cycle_mode != CYCLE_COPY) ||
		gGlobalPreferences.ForceLinearFilter) ? GL_LINEAR : GL_NEAREST;
	texture->ApplyParams(filt, (GLint)wrap_s, (GLint)wrap_t);
}

void RendererCTR::ApplyTexEnvMode(u32 mode)
{
	if (!mGlCache.valid || mGlCache.texEnvMode != mode)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
		mGlCache.texEnvMode = mode;
	}
}

void RendererCTR::ApplyProjectionMatrix(const float (&mat_project)[16])
{
	const float * ptr = mat_project;
	const u32 gen = (ptr == gProjection.m) ? gProjectionGen : 0u;
	if (!mGlCache.valid || mGlCache.projPtr != ptr || mGlCache.projGen != gen)
	{
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(const_cast<float *>(ptr));
		mGlCache.projPtr = ptr;
		mGlCache.projGen = gen;
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
			ApplyBlendState(true, GL_ZERO, GL_ONE);
			break;
		default:
			ApplyBlendState(false);
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
		ApplyBlendState(false);
		break;
	case 0x55F0: // Bust-A-Move 3 DX
		ApplyBlendState(true, GL_ONE, GL_SRC_ALPHA);
		break;
	case 0x0F1A:
		if (cycle_type == CYCLE_1CYCLE)
			ApplyBlendState(false);
		else
			ApplyBlendState(true, GL_ZERO, GL_ONE);
		break;
	case 0x0448: // Space Invaders
	case 0x0554:
		ApplyBlendState(true, GL_ONE, GL_ONE);
		break;
	case 0x0F5A: // Zelda: MM
	case 0x0FA5: // OOT menu
	case 0x5055: // Paper Mario intro
	case 0xAF50: // Zelda: MM
	case 0xC712: // Pokemon Stadium
		ApplyBlendState(true, GL_ZERO, GL_ONE);
		break;
	case 0x0C40: // Extreme-G
	case 0x0C48: // Star Wars: Shadow of the Empire text and hud
	case 0x4C40: // Wave Race
	case 0x5F50:
		ApplyBlendState(true, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
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
		if (!(!alpha_cvg_sel || cvg_x_alpha)) ApplyBlendState(false);
		else ApplyBlendState(true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case 0xC912: // 40 Winks
		ApplyBlendState(true, GL_SRC_ALPHA, GL_ONE);
		break;
	case 0x0C19:
	case 0xC811:
		ApplyBlendState(true, GL_SRC_ALPHA, GL_DST_ALPHA);
		break;
	case 0x5000: // V8 explosions
		ApplyBlendState(true, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
		break;
	default:
		if (!(!alpha_cvg_sel || cvg_x_alpha)) ApplyBlendState(false);
		else ApplyBlendState(true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

RendererCTR::RendererCTR()
{
	memset(&mGlCache, 0, sizeof(mGlCache));
	//
	//	Set up RGB = T0, A = T0
	//
	mCopyBlendStates = new CBlendStates;
	{
		CAlphaRenderSettings *	alpha_settings( new CAlphaRenderSettings( "Copy" ) );
		CRenderSettingsModulate *	colour_settings( new CRenderSettingsModulate( "Copy" ) );

		alpha_settings->AddTermTexel0();
		colour_settings->AddTermTexel0();

		mCopyBlendStates->SetAlphaSettings( alpha_settings );
		mCopyBlendStates->AddColourSettings( colour_settings );
	}

	//
	//	Set up RGB = Diffuse, A = Diffuse
	//
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
	delete mFillBlendStates;
	delete mCopyBlendStates;
}

void RendererCTR::RestoreRenderStates()
{	
	// Initialise the device to our default state
	glEnable(GL_TEXTURE_2D);
	
	// No fog
	glDisable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);

	glScissor(0,0, 400, 240);
	glEnable(GL_SCISSOR_TEST);
	
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glDisable( GL_BLEND );
	
	// Default is ZBuffer disabled
	glDepthMask(GL_FALSE);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_DEPTH_TEST);
	
	// Enable this for rendering decals (glPolygonOffset).
	glEnable(GL_POLYGON_OFFSET_FILL);
		
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	
	float envcolor[4] = { c32::White.GetRf(), c32::White.GetGf(), c32::White.GetBf(), c32::White.GetAf() };

	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envcolor);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glColor3f(1.0f, 1.0f, 1.0f);

	// Reset local GL state cache to match the restored defaults.
	memset(&mGlCache, 0, sizeof(mGlCache));
	mGlCache.valid = false;
	mGlCache.mForceTexRebind = true;
}

RendererCTR::SBlendStateEntry RendererCTR::LookupBlendState( u64 mux, bool two_cycles )
{
	#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	DAEDALUS_PROFILE( "RendererCtr::LookupBlendState" );
	mRecordedCombinerStates.insert( mux );
	#endif

	REG64 key;
	key._u64 = mux;

	// Top 8 bits are never set - use the very top one to differentiate between 1/2 cycles
	key._u32_1 |= (two_cycles << 31);

	const u32 slotIdx = (key._u32_0 ^ (key._u32_0 >> 11) ^ key._u32_1 ^ (key._u32_1 >> 13)) & (kBlendCacheSize - 1);
	SBlendCacheSlot & cacheSlot = mBlendCache[slotIdx];
	if (cacheSlot.valid && cacheSlot.key == key._u64)
	{
		return cacheSlot.entry;
	}

	BlendStatesMap::const_iterator	it( mBlendStatesMap.find( key._u64 ) );
	if( it != mBlendStatesMap.end() )
	{
		cacheSlot.key   = key._u64;
		cacheSlot.entry = it->second;
		cacheSlot.valid = true;
		return it->second;
	}

	// Blendmodes with Inexact blends either get an Override blend or a Default blend (GU_TFX_MODULATE)
	// If its not an Inexact blend then we check if we need to Force a blend mode none the less// Salvy
	//
	SBlendStateEntry entry;
	CCombinerTree tree( mux, two_cycles );
	entry.States = tree.GetBlendStates();

	if( entry.States->IsInexact() )
	{
		entry.OverrideFunction = LookupOverrideBlendModeInexact( mux );
	}
	else
	{
		// This is for non-inexact blends, errg hacks and such to be more precise
		entry.OverrideFunction = LookupOverrideBlendModeForced( mux );
	}

	#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	printf( "Adding %08x%08x - %d cycles - %s\n", u32(mux>>32), u32(mux), two_cycles ? 2 : 1, entry.States->IsInexact() ?  IsCombinerStateDefault(mux) ? "Inexact(Default)" : "Inexact(Override)" : entry.OverrideFunction==nullptr ? "Auto" : "Forced");
	#endif

	//Add blend mode to the Blend States Map
	mBlendStatesMap[ key._u64 ] = entry;

	cacheSlot.key   = key._u64;
	cacheSlot.entry = entry;
	cacheSlot.valid = true;

	return entry;
}

void RendererCTR::DrawPrimitives(DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode, bool has_texture)
{
	(void)has_texture;
	if (num_vertices < 3 || gMaxVertices < 3)
		return;

	// Only these modes are used by this renderer. Do not split an unknown
	// topology into batches whose interpretation might be different.
	if (triangle_mode != GL_TRIANGLES && triangle_mode != GL_TRIANGLE_STRIP && triangle_mode != GL_TRIANGLE_FAN)
		return;

	u32 offset = 0;
	while (offset < num_vertices)
	{
		u32 remaining = num_vertices - offset;
		u32 count = remaining < gMaxVertices ? remaining : gMaxVertices;
		if (triangle_mode == GL_TRIANGLE_FAN && offset != 0)
			count = (remaining < gMaxVertices - 1 ? remaining : gMaxVertices - 1) + 1;
		u32 mode = triangle_mode;
		bool strip_triangle = false;

		if (triangle_mode == GL_TRIANGLES)
		{
			// Incomplete trailing vertices cannot form a triangle.
			count -= count % 3;
			if (count == 0)
				break;
		}
		else if (triangle_mode == GL_TRIANGLE_STRIP && gMaxVertices == 3)
		{
			// A three-vertex strip cannot restart at an even offset.
			mode = GL_TRIANGLES;
			strip_triangle = true;
		}
		else if (triangle_mode == GL_TRIANGLE_STRIP && remaining > gMaxVertices)
			count -= count % 2; // Preserve strip winding after the restart.

		if (gVertexCount > gMaxVertices || count > gMaxVertices - gVertexCount)
		{
			// Commands may still reference the old buffer contents.
			glFinish();
			CGraphicsContext::Get()->ResetVertexBuffer();
		}

		if (triangle_mode != GL_TRIANGLE_FAN && !strip_triangle)
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
				if (triangle_mode == GL_TRIANGLE_FAN && offset != 0)
					index = i == 0 ? 0 : offset + (i - 1);
				else if (strip_triangle && (offset & 1) && i < 2)
					index = offset + 1 - i;
				else
					index = offset + i;

				dst[i] = p_vertices[index];
			}
			gVtxBuffer += count;
		}

		glDrawArrays(mode, gVertexCount, count);
		gVertexCount += count;

		if (triangle_mode == GL_TRIANGLES)
			offset += count;
		else if (triangle_mode == GL_TRIANGLE_STRIP)
		{
			if (remaining <= count)
				break;
			offset += count - 2;
		}
		else
		{
			// Subsequent fan batches contain the original centre and the
			// previous rim vertex; offset points at that rim vertex.
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

void RendererCTR::RenderUsingRenderSettings( const CBlendStates * states, DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode)
{
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

		// Vertex expressions change only Colour; each pass starts from the
		// original colours rather than the previous pass's output.
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
			u32	tfx = GL_MODULATE;
			switch( out.BlendMode )
			{
			case PBM_MODULATE:
				tfx = GL_MODULATE;
				break;
			case PBM_REPLACE:
				tfx = GL_REPLACE;
				break;
			case PBM_BLEND:
				tfx = GL_BLEND;
				float envcolor[4] = { out.TextureFactor.GetRf(), out.TextureFactor.GetGf(), out.TextureFactor.GetBf(), out.TextureFactor.GetAf() };
				glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envcolor);
				break;
			}
			
			ApplyTexEnvMode(tfx);

			if( g_ROM.T1_HACK )
			{
				// NB if install_texture0 and install_texture1 are both set, 1 wins out
				texture_idx = install_texture1;
			}
			else
			{
				// NB if install_texture0 and install_texture1 are both set, 0 wins out
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
				InstallNativeTexture(texture, mTexWrap[texture_idx].u, mTexWrap[texture_idx].v);
				installed_texture = true;
			}
		}

		// If no texture was specified, or if we couldn't load it, clear it out
		if( !installed_texture )
			ApplyTexture2DState(false);

		DrawPrimitives(p_vertices, num_vertices, triangle_mode, installed_texture);

		/*if ( mTnL.Flags.Fog )
		{
			RenderFog( p_FogVtx, num_vertices, triangle_mode, render_flags );
		}*/
	}
	if (saved_colours != mVtx_Save)
		free(saved_colours);
}


void RendererCTR::RenderUsingCurrentBlendMode(const float (&mat_project)[16], DaedalusVtx * p_vertices, u32 num_vertices, u32 triangle_mode, bool disable_zbuffer )
{
	ApplyProjectionMatrix(mat_project);

	if ( disable_zbuffer )
	{
		ApplyDepthState(false, false, false);
	}
	else
	{
		const bool decal = (gRDPOtherMode.zmode == 3);
		const bool depth_enable = ((mTnL.Flags.Zbuffer & gRDPOtherMode.z_cmp) || gRDPOtherMode.z_upd) != 0;
		const bool depth_mask = gRDPOtherMode.z_upd != 0;
		ApplyDepthState(depth_enable, depth_mask, decal);
	}
	
	u32 cycle_mode = gRDPOtherMode.cycle_type;
	
	// Initiate Blender
	//
	if(cycle_mode < CYCLE_COPY && gRDPOtherMode.force_bl)
	{
		InitBlenderModeCached();
	}
	else if (gRDPOtherMode.clr_on_cvg)
	{
		if ((cycle_mode == CYCLE_1CYCLE && gRDPOtherMode.c1_m2a == 1) ||
		    (cycle_mode == CYCLE_2CYCLE && gRDPOtherMode.c2_m2a == 1))
			ApplyBlendState(true, GL_ZERO, GL_ONE);
		else
			ApplyBlendState(false);
	}
	else
	{
		ApplyBlendState(false);
	}
	
	// Initiate Alpha test
	//
	if( (gRDPOtherMode.alpha_compare == G_AC_THRESHOLD) && !gRDPOtherMode.alpha_cvg_sel )
	{
		u8 alpha_threshold = mBlendColour.GetA();
		ApplyAlphaState(true, (alpha_threshold || g_ROM.ALPHA_HACK) ? GL_GEQUAL : GL_GREATER, mBlendColour.GetAf());
	}
	else if (gRDPOtherMode.cvg_x_alpha)
	{
		ApplyAlphaState(true, GL_GEQUAL, 0.04392f);
	}
	else
	{
		ApplyAlphaState(false);
	}

	mGlCache.valid = true;
	
	SBlendStateEntry		blend_entry;

	switch ( cycle_mode )
	{
		case CYCLE_COPY:		blend_entry.States = mCopyBlendStates; break;
		case CYCLE_FILL:		blend_entry.States = mFillBlendStates; break;
		case CYCLE_1CYCLE:		blend_entry = LookupBlendState( mMux, false ); break;
		case CYCLE_2CYCLE:		blend_entry = LookupBlendState( mMux, true ); break;
	}
	
	if( blend_entry.OverrideFunction != nullptr )
	{
		// Local vars for now
		SBlendModeDetails details;

		details.EnvColour = mEnvColour;
		details.PrimColour = mPrimitiveColour;
		details.InstallTexture = true;
		details.ColourAdjuster.Reset();

		blend_entry.OverrideFunction( details );
		// Override functions call glTexEnvi directly; invalidate cached mode.
		mGlCache.texEnvMode = 0;

		bool installed_texture = false;

		if( details.InstallTexture )
		{
			int texture_idx = g_ROM.T1_HACK ? 1 : 0;

			if( mBoundTexture[ texture_idx ] )
			{
				InstallNativeTexture(mBoundTexture[ texture_idx ], mTexWrap[texture_idx].u, mTexWrap[texture_idx].v);
				installed_texture = true;
			}
		}
		
		// If no texture was specified, or if we couldn't load it, clear it out
		if( !installed_texture )
		{
			ApplyTexture2DState(false);
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
		#ifdef DAEDALUS_DEBUG_CONSOLE
		// Set default states
		DAEDALUS_ERROR( "Unhandled blend mode" );
		#endif
		ApplyTexture2DState(false);
		DrawPrimitives(p_vertices, num_vertices, triangle_mode, false);
	}

}

void RendererCTR::RenderTriangles(DaedalusVtx *p_vertices, u32 num_vertices, bool disable_zbuffer)
{
	// Tile snapshots and UV scaling are already fused into BaseRenderer::FlushTris /
	// PrepareTrisUnclipped on CTR to avoid a second pass over every vertex.
	if (mTnL.Flags.Texture)
	{
		ApplyTexture2DState(true);
	}
	
	RenderUsingCurrentBlendMode(gProjection.m, p_vertices, num_vertices, GL_TRIANGLES, disable_zbuffer);
}

void RendererCTR::TexRect(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1)
{
	// FIXME(strmnnrmn): in copy mode, depth buffer is always disabled. Might not need to check this explicitly.
	UpdateTileSnapshots( tile_idx );

	CNativeTexture *texture = mBoundTexture[0];
	if (texture == nullptr)
		return;

	// NB: we have to do this after UpdateTileSnapshot, as it set up mTileTopLeft etc.
	// We have to do it before PrepareRenderState, because those values are applied to the graphics state.
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

	float scale_x = texture->GetScaleX();
	float scale_y = texture->GetScaleY();

	DaedalusVtx p_vertices[4];

	p_vertices[0].Texture.x = uv0.x * scale_x;
	p_vertices[0].Texture.y = uv0.y * scale_y;
	p_vertices[1].Texture.x = uv1.x * scale_x;
	p_vertices[1].Texture.y = uv0.y * scale_y;
	p_vertices[2].Texture.x = uv0.x * scale_x;
	p_vertices[2].Texture.y = uv1.y * scale_y;
	p_vertices[3].Texture.x = uv1.x * scale_x;
	p_vertices[3].Texture.y = uv1.y * scale_y;
	
	p_vertices[0].Position.x = screen0.x;
	p_vertices[0].Position.y = screen0.y;
	p_vertices[0].Position.z = depth;
	p_vertices[0].Colour = c32(0xffffffff);

	p_vertices[1].Position.x = screen1.x;
	p_vertices[1].Position.y = screen0.y;
	p_vertices[1].Position.z = depth;
	p_vertices[1].Colour = c32(0xffffffff);

	p_vertices[2].Position.x = screen0.x;
	p_vertices[2].Position.y = screen1.y;
	p_vertices[2].Position.z = depth;
	p_vertices[2].Colour = c32(0xffffffff);

	p_vertices[3].Position.x = screen1.x;
	p_vertices[3].Position.y = screen1.y;
	p_vertices[3].Position.z = depth;
	p_vertices[3].Colour = c32(0xffffffff);

	ApplyTexture2DState(true);

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GL_TRIANGLE_STRIP, gRDPOtherMode.depth_source ? false : true);
}

void RendererCTR::TexRectFlip(u32 tile_idx, const v2 & xy0, const v2 & xy1, TexCoord st0, TexCoord st1)
{
	// FIXME(strmnnrmn): in copy mode, depth buffer is always disabled. Might not need to check this explicitly.
	UpdateTileSnapshots( tile_idx );

	CNativeTexture *texture = mBoundTexture[0];
	if (texture == nullptr)
		return;

	// NB: we have to do this after UpdateTileSnapshot, as it set up mTileTopLeft etc.
	// We have to do it before PrepareRenderState, because those values are applied to the graphics state.
	PrepareTexRectUVs(&st0, &st1);

	v2 uv0( (float)st0.s / 32.f, (float)st0.t / 32.f );
	v2 uv1( (float)st1.s / 32.f, (float)st1.t / 32.f );

	v2 screen0;
	v2 screen1;
	ConvertN64ToScreen( xy0, screen0 );
	ConvertN64ToScreen( xy1, screen1 );

	float scale_x = texture->GetScaleX();
	float scale_y = texture->GetScaleY();

	DaedalusVtx p_vertices[4];

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

	p_vertices[3].Position.x = screen1.x;
	p_vertices[3].Position.y = screen1.y;
	p_vertices[3].Position.z = 0.0f;
	p_vertices[3].Colour = c32(0xffffffff);
	p_vertices[3].Texture.x = uv1.x * scale_x;
	p_vertices[3].Texture.y = uv1.y * scale_y;

	ApplyTexture2DState(true);

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GL_TRIANGLE_STRIP, gRDPOtherMode.depth_source ? false : true);
}

void RendererCTR::FillRect(const v2 & xy0, const v2 & xy1, u32 color)
{
	v2 screen0;
	v2 screen1;
	ScaleN64ToScreen( xy0, screen0 );
	ScaleN64ToScreen( xy1, screen1 );
	
	DaedalusVtx p_vertices[4];
	
	p_vertices[0].Position.x = screen0.x;
	p_vertices[0].Position.y = screen0.y;
	p_vertices[0].Position.z = 0.0f;
	p_vertices[0].Colour = c32(color);

	p_vertices[1].Position.x = screen1.x;
	p_vertices[1].Position.y = screen0.y;
	p_vertices[1].Position.z = 0.0f;
	p_vertices[1].Colour = c32(color);

	p_vertices[2].Position.x = screen0.x;
	p_vertices[2].Position.y = screen1.y;
	p_vertices[2].Position.z = 0.0f;
	p_vertices[2].Colour = c32(color);

	p_vertices[3].Position.x = screen1.x;
	p_vertices[3].Position.y = screen1.y;
	p_vertices[3].Position.z = 0.0f;
	p_vertices[3].Colour = c32(color);
	
	ApplyTexture2DState(false);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GL_TRIANGLE_STRIP, true);

	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
}

void RendererCTR::Draw2DTexture(f32 x0, f32 y0, f32 x1, f32 y1,
								f32 u0, f32 v0, f32 u1, f32 v1,
								const CNativeTexture * texture)
{
	if (texture == nullptr)
		return;
	InstallNativeTexture(texture);

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

	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GL_TRIANGLE_STRIP, true);
}

void RendererCTR::Draw2DTextureR(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2,
								 f32 y2, f32 x3, f32 y3, f32 s, f32 t,
								 const CNativeTexture * texture)
{
	if (texture == nullptr)
		return;
	InstallNativeTexture(texture);
	
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
	
	RenderUsingCurrentBlendMode(mScreenToDevice.mRaw, p_vertices, 4, GL_TRIANGLE_FAN, true);
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
