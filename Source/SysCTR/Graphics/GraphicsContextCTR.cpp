#include "stdafx.h"
#include "Graphics/GraphicsContext.h"

#include <3ds.h>
#include <GL/picaGL.h>

#include "Config/ConfigOptions.h"
#include "Core/ROM.h"
#include "Debug/DBGConsole.h"
#include "Debug/Dump.h"
#include "Graphics/ColourValue.h"
#include "Graphics/PngUtil.h"
#include "HLEGraphics/DaedalusVtx.h"
#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/Profiler.h"
#include "Utility/VolatileMem.h"

#include "SysCTR/UI/InGameMenu.h"
#include "SysCTR/UI/UserInterface.h"

extern void HandleEndOfFrame();

#define SCR_WIDTH 400
#define SCR_HEIGHT 240

static bool newFrame = true;

uint32_t     gVertexCount = 0;

// Interleaved DaedalusVtx linear buffer allows single-memcpy submission in RendererCTR.
DaedalusVtx * gVtxBuffer = nullptr;
DaedalusVtx * gVtxBufferPtr = nullptr;

class IGraphicsContext : public CGraphicsContext
{
public:
	IGraphicsContext();
	virtual ~IGraphicsContext();

	bool				Initialise();
	bool				IsInitialised() const { return mInitialised; }

	void				SwitchToChosenDisplay();
	void				SwitchToLcdDisplay();
	void				StoreSaveScreenData();

	void				ClearAllSurfaces();

	void				ClearToBlack();
	void				ClearZBuffer();
	void				ClearColBuffer(const c32 &colour);
	void				ClearColBufferAndDepth(const c32 &colour);

	void				ResetVertexBuffer();

	void				BeginFrame();
	void				EndFrame();
	void				UpdateFrame(bool wait_for_vbl);
	void				GetScreenSize(u32 * width, u32 * height) const;

	void				SetDebugScreenTarget( ETargetSurface buffer );

	void				ViewportType(u32 *d_width, u32 *d_height) const;
	void				DumpScreenShot();
	void				DumpNextScreen()			{ mDumpNextScreen = 2; }

private:
	void				SaveScreenshot(const char* filename, s32 x, s32 y, u32 width, u32 height);
	void				SelectVertexBank(u32 bank);

private:
	static const u32	kNumVertexBanks = 2;

	bool				mInitialised;
	u32					mDumpNextScreen;
	u32					mCurrentBank;
	DaedalusVtx *		mVtxBanks[kNumVertexBanks];
};

//*************************************************************************************
//
//*************************************************************************************
template<> bool CSingleton< CGraphicsContext >::Create()
{
#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT_Q(mpInstance == nullptr);
#endif
	mpInstance = new IGraphicsContext();
	if (mpInstance == nullptr)
		return false;

	if (!mpInstance->Initialise())
	{
		delete mpInstance;
		mpInstance = nullptr;
		return false;
	}
	return true;
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

uint32_t gMaxVertices = 20000;

IGraphicsContext::IGraphicsContext()
:	mInitialised(false)
,	mDumpNextScreen(false)
,	mCurrentBank(0)
{
	for (u32 b = 0; b < kNumVertexBanks; ++b)
	{
		mVtxBanks[b] = (DaedalusVtx*)linearAlloc(gMaxVertices * sizeof(DaedalusVtx));
		if (mVtxBanks[b] == nullptr)
		{
			for (u32 j = 0; j < b; ++j)
			{
				linearFree(mVtxBanks[j]);
				mVtxBanks[j] = nullptr;
			}
			for (u32 j = b; j < kNumVertexBanks; ++j)
				mVtxBanks[j] = nullptr;
			gVtxBufferPtr = nullptr;
			gVtxBuffer = nullptr;
			return;
		}
	}

	SelectVertexBank(0);
	ResetVertexBuffer();
}

IGraphicsContext::~IGraphicsContext()
{
	if (mInitialised)
		glFinish();
	for (u32 b = 0; b < kNumVertexBanks; ++b)
	{
		if (mVtxBanks[b] != nullptr)
		{
			linearFree(mVtxBanks[b]);
			mVtxBanks[b] = nullptr;
		}
	}
	gVtxBufferPtr = nullptr;
	gVtxBuffer = nullptr;
	gVertexCount = 0;
}

void IGraphicsContext::SelectVertexBank(u32 bank)
{
	mCurrentBank = bank;
	gVtxBufferPtr = mVtxBanks[bank];
}

bool IGraphicsContext::Initialise()
{
	if (gVtxBufferPtr == nullptr)
		return false;

	mInitialised = true;

	pglSelectScreen(GFX_TOP, GFX_LEFT);
	ClearAllSurfaces();

	return true;
}

void IGraphicsContext::ClearAllSurfaces()
{
	ClearToBlack();
	pglSwapBuffersEx(1, 0);
	ClearToBlack();
	pglSwapBuffersEx(1, 0);
}

void IGraphicsContext::ClearToBlack()
{
	glViewport(0,0,400,240);
	glDisable(GL_SCISSOR_TEST);

	glDepthMask(GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth( 1.0f );
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void IGraphicsContext::ClearZBuffer()
{
	glDepthMask(GL_TRUE);
	glClearDepth( 1.0f );
	glClear( GL_DEPTH_BUFFER_BIT );
}

void IGraphicsContext::ClearColBuffer(const c32 & colour)
{
	glClearColor( colour.GetRf(), colour.GetGf(), colour.GetBf(), colour.GetAf() );
	glClear( GL_COLOR_BUFFER_BIT );
}

void IGraphicsContext::ClearColBufferAndDepth(const c32 & colour)
{
	glDepthMask(GL_TRUE);
	glClearDepth( 1.0f );
	glClearColor( colour.GetRf(), colour.GetGf(), colour.GetBf(), colour.GetAf() );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

void IGraphicsContext::ResetVertexBuffer()
{
	gVtxBuffer   = gVtxBufferPtr;
	gVertexCount = 0;
}

void IGraphicsContext::BeginFrame()
{
	if(newFrame)
	{
		ClearToBlack();
		newFrame = false;
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(3, GL_FLOAT, sizeof(DaedalusVtx), &gVtxBufferPtr[0].Position);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DaedalusVtx), &gVtxBufferPtr[0].Colour);
	glTexCoordPointer(2, GL_FLOAT, sizeof(DaedalusVtx), &gVtxBufferPtr[0].Texture);
}

void IGraphicsContext::EndFrame()
{
	HandleEndOfFrame();
}

void IGraphicsContext::UpdateFrame(bool wait_for_vbl)
{
	(void)wait_for_vbl;
	// The pause menu (FrontendCTR) owns the screen while it is open: it runs
	// its own blocking picaGL loop, so the emulator must not swap behind its
	// back. DrawInGameMenu runs here — never inside BeginFrame — because
	// opening it mid-frame would present while the N64 frame is half built.
	if (UI::DrawInGameMenu())
	{
		// A frame was presented by the menu itself (or the game was closed).
		ResetVertexBuffer();
		return;
	}

	// Only transfer the top screen during gameplay; skipping the idle bottom
	// screen saves a full 320x240 GX_DisplayTransfer every frame on Old 3DS.
	pglSwapBuffersEx(1, 0);

	// Alternate between two linear vertex banks per frame so gVertexCount resets
	// cleanly without triggering mid-scene glFinish() stalls at 20k/30k verts.
	SelectVertexBank(mCurrentBank ^ 1);
	ResetVertexBuffer();

	newFrame = true;
}

void IGraphicsContext::ViewportType(u32 *d_width, u32 *d_height) const
{
	switch ( gGlobalPreferences.ViewportType )
	{
		case VT_UNSCALED_4_3:
			*d_width = 320;
			*d_height = 240;
			break;
		default:
			*d_width = SCR_WIDTH;
			*d_height = SCR_HEIGHT;
			break;
	}
}

void IGraphicsContext::GetScreenSize(u32 * p_width, u32 * p_height) const
{
	*p_width = SCR_WIDTH;
	*p_height = SCR_HEIGHT;
}

void IGraphicsContext::SetDebugScreenTarget(ETargetSurface buffer){}

void IGraphicsContext::SaveScreenshot(const char* filename, s32 x, s32 y, u32 width, u32 height){}

void IGraphicsContext::DumpScreenShot(){}

void IGraphicsContext::StoreSaveScreenData(){}