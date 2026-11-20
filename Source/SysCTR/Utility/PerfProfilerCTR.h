#pragma once

#ifdef DAEDALUS_CTR

#include <3ds.h>
#include <atomic>
#include <cstdio>

#include "Utility/DaedalusTypes.h"
#include "Utility/Preferences.h"

namespace PerfCTR
{
	struct SWindow
	{
		u64 startTick;
		u64 dlTicks;
		u64 vtxTicks;
		u64 texTicks;
		u64 drawTicks;
		u64 presentTicks;
		u64 aud0Ticks;
		std::atomic<u64> audC1Ticks;
		u32 totalFrames;
		u32 renderedFrames;
		u32 skippedFrames;
		u32 drawCalls;
		u32 mergedCalls;
		u32 vertices;
		u32 cacheResets;
#ifdef DAEDALUS_CTR_XRAY
		u64 jitTicks;
		u32 jitEntries;
		u64 compTicks;
		u32 compCount;
		u64 irqTicks;
		u32 irqCount;
		u64 dmaTicks;
		u32 dmaCount;
		u64 genTicks;
		u32 genCalls;
		u64 oshleTicks;
		u32 oshleCalls;
		u32 slowWriteCalls;
		u32 indMissCalls;
		u64 dlBeginTicks;
		u64 dlRectTicks;
		u64 dlMtxTicks;
		u32 resetReasons[4]; // 0=cap, 1=inv, 2=maxf, 3=hot
		u32 exitReasons[3];  // 0=stuff, 1=delay, 2=unlinked

		static const u32 kMaxSlots = 32;
		struct OpSlot { u32 key; u32 calls; u64 ticks; };
		OpSlot genSlots[kMaxSlots];
		struct FnSlot { const void * fn; u32 calls; u64 ticks; };
		FnSlot oshleSlots[kMaxSlots];
#endif
	};

	inline SWindow & State()
	{
		static SWindow s = {};
		return s;
	}

	inline void Reset()
	{
		SWindow & s = State();
		s.startTick      = svcGetSystemTick();
		s.dlTicks        = 0;
		s.vtxTicks       = 0;
		s.texTicks       = 0;
		s.drawTicks      = 0;
		s.presentTicks   = 0;
		s.aud0Ticks      = 0;
		s.audC1Ticks.store(0, std::memory_order_relaxed);
		s.totalFrames    = 0;
		s.renderedFrames = 0;
		s.skippedFrames  = 0;
		s.drawCalls      = 0;
		s.mergedCalls    = 0;
		s.vertices       = 0;
		s.cacheResets    = 0;
#ifdef DAEDALUS_CTR_XRAY
		s.jitTicks       = 0;
		s.jitEntries     = 0;
		s.compTicks      = 0;
		s.compCount      = 0;
		s.irqTicks       = 0;
		s.irqCount       = 0;
		s.dmaTicks       = 0;
		s.dmaCount       = 0;
		s.genTicks       = 0;
		s.genCalls       = 0;
		s.oshleTicks     = 0;
		s.oshleCalls     = 0;
		s.slowWriteCalls = 0;
		s.indMissCalls   = 0;
		s.dlBeginTicks   = 0;
		s.dlRectTicks    = 0;
		s.dlMtxTicks     = 0;
		for (u32 i = 0; i < 4; ++i) s.resetReasons[i] = 0;
		for (u32 i = 0; i < 3; ++i) s.exitReasons[i] = 0;
		for (u32 i = 0; i < SWindow::kMaxSlots; ++i)
		{
			s.genSlots[i]   = {0, 0, 0};
			s.oshleSlots[i] = {nullptr, 0, 0};
		}
#endif
	}

	inline void AddDL(u64 dt)      { State().dlTicks += dt; }
	inline void AddVtx(u64 dt)     { State().vtxTicks += dt; }
	inline void AddTex(u64 dt)     { State().texTicks += dt; }
	inline void AddDraw(u64 dt)    { State().drawTicks += dt; }
	inline void AddPresent(u64 dt) { State().presentTicks += dt; }
	inline void AddAud0(u64 dt)    { State().aud0Ticks += dt; }
	inline void AddAudC1(u64 dt)   { State().audC1Ticks.fetch_add(dt, std::memory_order_relaxed); }
	inline void CountCacheReset()  { ++State().cacheResets; }

#ifdef DAEDALUS_CTR_XRAY
	inline void AddJit(u64 dt)       { SWindow & s = State(); s.jitTicks += dt; ++s.jitEntries; }
	inline void AddComp(u64 dt)      { SWindow & s = State(); s.compTicks += dt; ++s.compCount; }
	inline void AddIrq(u64 dt)       { SWindow & s = State(); s.irqTicks += dt; ++s.irqCount; }
	inline void AddDma(u64 dt)       { SWindow & s = State(); s.dmaTicks += dt; ++s.dmaCount; }
	inline void CountSlowWrite()     { ++State().slowWriteCalls; }
	inline void CountIndMiss()       { ++State().indMissCalls; }
	inline void AddDlBegin(u64 dt)   { State().dlBeginTicks += dt; }
	inline void AddDlRect(u64 dt)    { State().dlRectTicks += dt; }
	inline void AddDlMtx(u64 dt)     { State().dlMtxTicks += dt; }
	inline void CountResetReason(u32 r) { if (r < 4) ++State().resetReasons[r]; }
	inline void CountExitReason(u32 r)  { if (r < 3) ++State().exitReasons[r]; }

	inline void RecordGenericOp(u32 op_word, u64 dt)
	{
		SWindow & s = State();
		s.genTicks += dt;
		++s.genCalls;
		const u32 op = op_word >> 26;
		u32 sub = 0;
		if (op == 0) sub = op_word & 0x3Fu;               // SPECIAL
		else if (op == 1) sub = (op_word >> 16) & 0x1Fu;  // REGIMM
		else if (op == 16 || op == 17) sub = ((op_word >> 21) & 0x1Fu) << 6 | (op_word & 0x3Fu);
		const u32 key = (op << 12) | sub;
		for (u32 i = 0; i < SWindow::kMaxSlots; ++i)
		{
			if (s.genSlots[i].calls == 0 || s.genSlots[i].key == key)
			{
				s.genSlots[i].key = key;
				++s.genSlots[i].calls;
				s.genSlots[i].ticks += dt;
				return;
			}
		}
	}

	inline void RecordOshleFn(const void * fn, u64 dt)
	{
		SWindow & s = State();
		s.oshleTicks += dt;
		++s.oshleCalls;
		for (u32 i = 0; i < SWindow::kMaxSlots; ++i)
		{
			if (s.oshleSlots[i].calls == 0 || s.oshleSlots[i].fn == fn)
			{
				s.oshleSlots[i].fn = fn;
				++s.oshleSlots[i].calls;
				s.oshleSlots[i].ticks += dt;
				return;
			}
		}
	}
#endif

	inline void CountDrawCall(u32 verts)
	{
		SWindow & s = State();
		++s.drawCalls;
		s.vertices += verts;
	}

	inline void CountBatchMerge(u32 verts)
	{
		SWindow & s = State();
		++s.mergedCalls;
		s.vertices += verts;
	}

	inline void OnScreenFrame(bool skipped, const char * gameName)
	{
		SWindow & s = State();
		const u64 now = svcGetSystemTick();
		if (s.startTick == 0)
		{
			s.startTick = now;
			return;
		}

		++s.totalFrames;
		if (skipped)
			++s.skippedFrames;
		else
			++s.renderedFrames;

		// Flush one concise breakdown line every ~3 seconds when FPS display is enabled.
		const u64 elapsed = now - s.startTick;
		if (elapsed < static_cast<u64>(SYSCLOCK_ARM11) * 3ull)
			return;

		if (gGlobalPreferences.DisplayFramerate && s.totalFrames > 0)
		{
			const double ticksPerMs = static_cast<double>(SYSCLOCK_ARM11) / 1000.0;
			const double elapsedSec = static_cast<double>(elapsed) / static_cast<double>(SYSCLOCK_ARM11);
			const double fps        = static_cast<double>(s.totalFrames) / elapsedSec;
			const double visFps     = static_cast<double>(s.renderedFrames) / elapsedSec;
			const double invFrames  = 1.0 / static_cast<double>(s.totalFrames);
			const double invRend    = (s.renderedFrames > 0) ? (1.0 / static_cast<double>(s.renderedFrames)) : 0.0;

			const double frameMs = (static_cast<double>(elapsed) / ticksPerMs) * invFrames;
			const double dlMs    = (static_cast<double>(s.dlTicks) / ticksPerMs) * invFrames;
			const double presMs  = (static_cast<double>(s.presentTicks) / ticksPerMs) * invFrames;
			const double aud0Ms  = (static_cast<double>(s.aud0Ticks) / ticksPerMs) * invFrames;
			const double audC1Ms = (static_cast<double>(s.audC1Ticks.load(std::memory_order_relaxed)) / ticksPerMs) * invFrames;
			double cpuMs         = frameMs - dlMs - presMs - aud0Ms;
			if (cpuMs < 0.0) cpuMs = 0.0;

			// Per-rendered-frame sub-breakdown (so Frameskip doesn't hide real render cost).
			const double dlPerRend  = (static_cast<double>(s.dlTicks) / ticksPerMs) * invRend;
			const double vtxPerRend = (static_cast<double>(s.vtxTicks) / ticksPerMs) * invRend;
			const double texPerRend = (static_cast<double>(s.texTicks) / ticksPerMs) * invRend;
			const double drwPerRend = (static_cast<double>(s.drawTicks) / ticksPerMs) * invRend;
			const u32 drawsPerRend  = (s.renderedFrames > 0) ? (s.drawCalls / s.renderedFrames) : 0;
			const u32 mergesPerRend = (s.renderedFrames > 0) ? (s.mergedCalls / s.renderedFrames) : 0;
			const u32 vertsPerRend  = (s.renderedFrames > 0) ? (s.vertices / s.renderedFrames) : 0;

			FILE * log = fopen("sdmc:/DaedalusX64.log", "a");
			if (log != nullptr)
			{
				fprintf(log,
					"perf[%s]: fps=%.1f (vis=%.1f) frame=%.1fms | CPU=%.1fms DL=%.1fms Pres=%.1fms Aud0=%.1fms | AudC1=%.1fms | perRend: DL=%.1fms (vtx=%.1f tex=%.1f drw=%.1f) draws=%lu merged=%lu verts=%lu resets=%lu\n",
					gameName ? gameName : "ROM",
					fps, visFps, frameMs,
					cpuMs, dlMs, presMs, aud0Ms,
					audC1Ms,
					dlPerRend, vtxPerRend, texPerRend, drwPerRend,
					static_cast<unsigned long>(drawsPerRend),
					static_cast<unsigned long>(mergesPerRend),
					static_cast<unsigned long>(vertsPerRend),
					static_cast<unsigned long>(s.cacheResets));
#ifdef DAEDALUS_CTR_XRAY
				const double jitMs   = (static_cast<double>(s.jitTicks) / ticksPerMs) * invFrames;
				const double compMs  = (static_cast<double>(s.compTicks) / ticksPerMs) * invFrames;
				const double irqMs   = (static_cast<double>(s.irqTicks) / ticksPerMs) * invFrames;
				const double dmaMs   = (static_cast<double>(s.dmaTicks) / ticksPerMs) * invFrames;
				const double genMs   = (static_cast<double>(s.genTicks) / ticksPerMs) * invFrames;
				const double oshleMs = (static_cast<double>(s.oshleTicks) / ticksPerMs) * invFrames;
				const double begRend = (static_cast<double>(s.dlBeginTicks) / ticksPerMs) * invRend;
				const double rctRend = (static_cast<double>(s.dlRectTicks) / ticksPerMs) * invRend;
				const double mtxRend = (static_cast<double>(s.dlMtxTicks) / ticksPerMs) * invRend;
				fprintf(log,
					"  xray: jit=%.1fms(%lu/f s=%lu d=%lu u=%lu) inter+ovh=%.1fms comp=%.2fms(%lu) rst(c=%lu i=%lu m=%lu h=%lu) irq=%.2fms(%lu/f) dma=%.2fms(%lu/f) | gen=%.2fms(%lu/f) oshle=%.2fms(%lu/f) slowW=%lu/f indMiss=%lu/f | dlBeg=%.1f rect=%.1f mtx=%.1f\n",
					jitMs, static_cast<unsigned long>(s.jitEntries / s.totalFrames),
					static_cast<unsigned long>(s.exitReasons[0] / s.totalFrames),
					static_cast<unsigned long>(s.exitReasons[1] / s.totalFrames),
					static_cast<unsigned long>(s.exitReasons[2] / s.totalFrames),
					(cpuMs - jitMs > 0.0) ? (cpuMs - jitMs) : 0.0,
					compMs, static_cast<unsigned long>(s.compCount),
					static_cast<unsigned long>(s.resetReasons[0]),
					static_cast<unsigned long>(s.resetReasons[1]),
					static_cast<unsigned long>(s.resetReasons[2]),
					static_cast<unsigned long>(s.resetReasons[3]),
					irqMs, static_cast<unsigned long>(s.irqCount / s.totalFrames),
					dmaMs, static_cast<unsigned long>(s.dmaCount / s.totalFrames),
					genMs, static_cast<unsigned long>(s.genCalls / s.totalFrames),
					oshleMs, static_cast<unsigned long>(s.oshleCalls / s.totalFrames),
					static_cast<unsigned long>(s.slowWriteCalls / s.totalFrames),
					static_cast<unsigned long>(s.indMissCalls / s.totalFrames),
					begRend, rctRend, mtxRend);
				// Print top 4 generic ops by ticks
				fprintf(log, "  topGen:");
				for (u32 k = 0; k < 4; ++k)
				{
					u32 best = SWindow::kMaxSlots;
					for (u32 i = 0; i < SWindow::kMaxSlots; ++i)
					{
						if (s.genSlots[i].calls > 0 && (best == SWindow::kMaxSlots || s.genSlots[i].ticks > s.genSlots[best].ticks))
							best = i;
					}
					if (best == SWindow::kMaxSlots) break;
					const u32 key = s.genSlots[best].key;
					const double ms = (static_cast<double>(s.genSlots[best].ticks) / ticksPerMs) * invFrames;
					fprintf(log, " [op=%lu sub=0x%lx: %lu/f %.2fms]",
						static_cast<unsigned long>(key >> 12),
						static_cast<unsigned long>(key & 0xFFFu),
						static_cast<unsigned long>(s.genSlots[best].calls / s.totalFrames), ms);
					s.genSlots[best].calls = 0;
				}
				fprintf(log, "\n  topOshle:");
				for (u32 k = 0; k < 4; ++k)
				{
					u32 best = SWindow::kMaxSlots;
					for (u32 i = 0; i < SWindow::kMaxSlots; ++i)
					{
						if (s.oshleSlots[i].calls > 0 && (best == SWindow::kMaxSlots || s.oshleSlots[i].ticks > s.oshleSlots[best].ticks))
							best = i;
					}
					if (best == SWindow::kMaxSlots) break;
					const double ms = (static_cast<double>(s.oshleSlots[best].ticks) / ticksPerMs) * invFrames;
					fprintf(log, " [%p: %lu/f %.2fms]",
						s.oshleSlots[best].fn,
						static_cast<unsigned long>(s.oshleSlots[best].calls / s.totalFrames), ms);
					s.oshleSlots[best].calls = 0;
				}
				fprintf(log, "\n");
#endif
				fclose(log);
			}
		}

		Reset();
	}
}

#endif
