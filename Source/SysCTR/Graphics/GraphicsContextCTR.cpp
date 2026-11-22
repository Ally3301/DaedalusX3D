#include "stdafx.h"
#include "Graphics/GraphicsContext.h"
#include "SysCTR/Graphics/Citro3DContext.h"
#include "SysCTR/Utility/PerfProfilerCTR.h"

#include <3ds.h>
#include <citro3d.h>

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
#include "SysCTR/UI/TouchControllerCTR.h"
#include "SysCTR/UI/UserInterface.h"

extern void HandleEndOfFrame();
extern bool shouldQuit;

#define SCR_WIDTH 400
#define SCR_HEIGHT 240

static bool newFrame = true;

uint32_t     gVertexCount = 0;

// Interleaved DaedalusVtx linear buffer allows zero-repack single-memcpy submission.
DaedalusVtx * gVtxBuffer = nullptr;
DaedalusVtx * gVtxBufferPtr = nullptr;

static inline u32 ColourToClearRGBA8(const c32 & colour)
{
	return ((u32)colour.GetR() << 24) |
	       ((u32)colour.GetG() << 16) |
	       ((u32)colour.GetB() << 8)  |
	       ((u32)colour.GetA());
}

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

uint32_t gMaxVertices = 24576;

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
	{
		Citro3D::EndGameFrame();
	}
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
	if (gVtxBufferPtr == nullptr || !Citro3D::IsInitialised())
		return false;

	mInitialised = true;
	newFrame = true;
	return true;
}

void IGraphicsContext::ClearAllSurfaces()
{
	Citro3D::EnsureGameFrameBegun();
	ClearToBlack();
	Citro3D::EndGameFrame();
	newFrame = true;
}

void IGraphicsContext::ClearToBlack()
{
	Citro3D::EnsureGameFrameBegun();
	Citro3D::SetViewportLandscape(0, 0, SCR_WIDTH, SCR_HEIGHT);
	Citro3D::SetScissorLandscape(false, 0, 0, SCR_WIDTH, SCR_HEIGHT);
	Citro3D::Clear(true, true, 0x00000000u, 0x00FFFFFFu);
}

void IGraphicsContext::ClearZBuffer()
{
	Citro3D::EnsureGameFrameBegun();
	Citro3D::Clear(false, true, 0x00000000u, 0x00FFFFFFu);
}

void IGraphicsContext::ClearColBuffer(const c32 & colour)
{
	Citro3D::EnsureGameFrameBegun();
	Citro3D::Clear(true, false, ColourToClearRGBA8(colour), 0x00FFFFFFu);
}

void IGraphicsContext::ClearColBufferAndDepth(const c32 & colour)
{
	Citro3D::EnsureGameFrameBegun();
	Citro3D::Clear(true, true, ColourToClearRGBA8(colour), 0x00FFFFFFu);
}

void IGraphicsContext::ResetVertexBuffer()
{
	gVtxBuffer   = gVtxBufferPtr;
	gVertexCount = 0;
}

void IGraphicsContext::BeginFrame()
{
	if (shouldQuit || Citro3D::IsExiting())
		return;

	if (newFrame)
	{
		Citro3D::EnsureGameFrameBegun();
		if (gCleanSceneEnabled)
			ClearToBlack();
		newFrame = false;
	}
	else if (!Citro3D::IsGameFrameActive())
	{
		Citro3D::EnsureGameFrameBegun();
	}

	Citro3D::SetVertexBuffer(gVtxBufferPtr, sizeof(DaedalusVtx));
}

void IGraphicsContext::EndFrame()
{
	// Do NOT call HandleEndOfFrame() (aptMainLoop) here: DLParser invokes
	// EndScene()/EndFrame() while the citro3d gameplay frame is still open
	// (C3D_FrameBegin without C3D_FrameEnd). Calling aptMainLoop() mid-frame
	// causes HOME -> Close (.cia) to deadlock GSP on real 3DS hardware.
	// HandleEndOfFrame() runs right after Citro3D::EndGameFrame() in UpdateFrame()
	// and via the VBL watchdog if a game stops presenting frames.
}

void IGraphicsContext::UpdateFrame(bool wait_for_vbl)
{
	(void)wait_for_vbl;

	if (shouldQuit || Citro3D::IsExiting())
		return;

	const u64 pres_t0 = svcGetSystemTick();

	// Flush accumulated vertex data once before presenting or opening the pause UI.
	if (gVertexCount > 0 && gVtxBufferPtr != nullptr)
		Citro3D::FlushDataCache(gVtxBufferPtr, gVertexCount * sizeof(DaedalusVtx));

	// Close the citro3d gameplay frame before the pause menu runs its own UI frame loop.
	if (UI::IsCapturingInput())
	{
		Citro3D::EndGameFrame();
		// Ensure the top LCD framebuffer transfer finishes so Pause captures a clean frame.
		C3D_FrameSync();
	}

	if (UI::DrawInGameMenu())
	{
		ResetVertexBuffer();
		UI::TouchController::MarkDirty();
		newFrame = true;
		PerfCTR::Reset();
		if (!shouldQuit && !Citro3D::IsExiting())
			HandleEndOfFrame();
		return;
	}

	// Draw the interactive bottom-screen extension controls ONLY when dirty (touch
	// change or 2 Hz FPS/battery refresh). Idle frames skip sBotTarget with 0 DMA cost!
	UI::TouchController::RenderBottomIfDirty();

	// Submit top screen (and bottom screen if touched/dirty) in one single command list.
	Citro3D::EndGameFrame();

	// Alternate between two linear vertex banks per frame so CPU writes to the
	// next bank while PICA200 consumes the previous one.
	SelectVertexBank(mCurrentBank ^ 1);
	ResetVertexBuffer();

	PerfCTR::AddPresent(svcGetSystemTick() - pres_t0);
	newFrame = true;

	// Safe point: citro3d frame is now closed (inFrame == false), so aptMainLoop()
	// can suspend to the 3DS HOME Menu or exit cleanly without deadlocking GSP.
	HandleEndOfFrame();
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
