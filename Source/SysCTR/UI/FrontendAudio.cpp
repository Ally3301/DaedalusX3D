// FrontendAudio: Zero-CPU-overhead UI sound effects and looping ambient audio
// powered strictly by the Nintendo 3DS TeakLite DSP (ndsp hardware channels 1-3).
#include "stdafx.h"
#include "FrontendAudio.h"

#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "System/Paths.h"

// Owned by SysCTR/HLEAudio/AudioOutput.cpp (uses hardware channel 0 only).
extern bool audioOpen;

namespace
{
	constexpr int   kAmbientCh       = 1;
	constexpr int   kSfxChBase       = 2;
	constexpr int   kSfxVoices       = 2; // Channels 2 and 3
	constexpr float kTwoPi           = 6.28318530717958647692f;
	constexpr u32   kSfxRate         = 22050;
	constexpr u32   kAmbientRate     = 16000;
	constexpr u32   kMaxAmbientBytes = 1024 * 1024; // 1 MiB cap for optional SD wav
	constexpr u32   kMaxSfxBytes     = 128 * 1024;  // 128 KiB cap for optional SD wav
	constexpr u32   kMinLinearForAmb = 384 * 1024;  // Keep headroom on Old 3DS

	struct AudioClip
	{
		s16 * pcm;
		u32   nsamples;   // Number of sample frames (per channel)
		u32   byteSize;
		u32   sampleRate;
		u16   format;     // NDSP_FORMAT_MONO_PCM16 or NDSP_FORMAT_STEREO_PCM16
	};

	bool        s_active        = false;
	bool        s_ownsNdsp      = false;
	bool        s_ambientActive = false;
	int         s_nextVoice     = 0;
	float       s_ambientGain   = 0.22f;

	AudioClip   s_ambient = {};
	AudioClip   s_sfx[FrontendAudio::SFX_COUNT] = {};

	ndspWaveBuf s_ambientWbuf = {};
	ndspWaveBuf s_sfxVoiceBuf[kSfxVoices] = {};

	inline s16 ClampS16(float x)
	{
		if (x > 1.0f) x = 1.0f;
		if (x < -1.0f) x = -1.0f;
		return static_cast<s16>(x * 32767.0f);
	}

	inline float SmoothAttack(float t, float attackSec)
	{
		if (t >= attackSec || attackSec <= 0.0f)
			return 1.0f;
		return 0.5f - 0.5f * cosf((t / attackSec) * (kTwoPi * 0.5f));
	}

	void FreeClip(AudioClip & clip)
	{
		if (clip.pcm != nullptr)
		{
			linearFree(clip.pcm);
			clip.pcm = nullptr;
		}
		clip.nsamples   = 0;
		clip.byteSize   = 0;
		clip.sampleRate = 0;
		clip.format     = 0;
	}

	bool AllocClip(AudioClip & clip, u32 nsamples, u32 sampleRate, bool stereo)
	{
		FreeClip(clip);
		const u32 frameBytes = stereo ? 4u : 2u;
		const u32 totalBytes = nsamples * frameBytes;
		if (totalBytes == 0 || linearSpaceFree() < totalBytes + 32768u)
			return false;

		s16 * buf = static_cast<s16 *>(linearAlloc(totalBytes));
		if (buf == nullptr)
			return false;

		clip.pcm        = buf;
		clip.nsamples   = nsamples;
		clip.byteSize   = totalBytes;
		clip.sampleRate = sampleRate;
		clip.format     = stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
		return true;
	}

	// Optional uncompressed PCM16 WAV override loader from SD card.
	bool TryLoadWavOverride(const char * filename, AudioClip & out, u32 maxBytes)
	{
		char path[256];
		snprintf(path, sizeof(path), "%sAudio/%s", DAEDALUS_CTR_PATH(""), filename);
		FILE * f = fopen(path, "rb");
		if (f == nullptr)
			return false;

		u8 hdr[12];
		if (fread(hdr, 1, 12, f) != 12 ||
		    memcmp(hdr, "RIFF", 4) != 0 ||
		    memcmp(hdr + 8, "WAVE", 4) != 0)
		{
			fclose(f);
			return false;
		}

		bool hasFmt = false;
		u16  audioFmt = 0;
		u16  channels = 0;
		u32  sampleRate = 0;
		u16  bitsPerSample = 0;

		while (true)
		{
			u8 chunkHdr[8];
			if (fread(chunkHdr, 1, 8, f) != 8)
				break;

			const u32 chunkSize =
				static_cast<u32>(chunkHdr[4]) |
				(static_cast<u32>(chunkHdr[5]) << 8) |
				(static_cast<u32>(chunkHdr[6]) << 16) |
				(static_cast<u32>(chunkHdr[7]) << 24);

			if (memcmp(chunkHdr, "fmt ", 4) == 0 && chunkSize >= 16)
			{
				u8 fmt[16];
				if (fread(fmt, 1, 16, f) != 16)
					break;
				audioFmt      = static_cast<u16>(fmt[0] | (fmt[1] << 8));
				channels      = static_cast<u16>(fmt[2] | (fmt[3] << 8));
				sampleRate    = static_cast<u32>(fmt[4]) |
				                (static_cast<u32>(fmt[5]) << 8) |
				                (static_cast<u32>(fmt[6]) << 16) |
				                (static_cast<u32>(fmt[7]) << 24);
				bitsPerSample = static_cast<u16>(fmt[14] | (fmt[15] << 8));
				hasFmt = true;
				if (chunkSize > 16)
					fseek(f, static_cast<long>(chunkSize - 16), SEEK_CUR);
			}
			else if (memcmp(chunkHdr, "data", 4) == 0 && hasFmt)
			{
				if (audioFmt == 1 &&
				    (channels == 1 || channels == 2) &&
				    bitsPerSample == 16 &&
				    sampleRate >= 8000 && sampleRate <= 48000 &&
				    chunkSize >= 4 && chunkSize <= maxBytes)
				{
					const u32 frameBytes = static_cast<u32>(channels) * 2u;
					const u32 nsamples = chunkSize / frameBytes;
					if (AllocClip(out, nsamples, sampleRate, channels == 2))
					{
						if (fread(out.pcm, 1, out.byteSize, f) == out.byteSize)
						{
							DSP_FlushDataCache(out.pcm, out.byteSize);
							fclose(f);
							return true;
						}
						FreeClip(out);
					}
				}
				break;
			}
			else
			{
				// Skip unknown chunk (word-aligned)
				fseek(f, static_cast<long>(chunkSize + (chunkSize & 1u)), SEEK_CUR);
			}
		}

		fclose(f);
		return false;
	}

	// Seamless 8.0s gentle 3DS-inspired ambient music (Fmaj7 <-> Cmaj7 pad + soft motif)
	// at 16 kHz mono PCM16 (250 KiB). All pad frequencies are exact multiples of 1/8.0s
	// (0.125 Hz) and melody decay tails wrap seamlessly around 8.0s -> 100% click-free DSP loop.
	bool BuildAmbientClip(AudioClip & out)
	{
		if (TryLoadWavOverride("ambient.wav", out, kMaxAmbientBytes))
			return true;

		if (linearSpaceFree() < kMinLinearForAmb)
			return false;

		constexpr float kDurSec = 8.0f;
		const u32 nsamples = static_cast<u32>(kAmbientRate * kDurSec);
		if (!AllocClip(out, nsamples, kAmbientRate, false))
			return false;

		// Warm open voicings quantized to exact 0.125 Hz multiples for seamless looping:
		// Chord A (Fmaj7): F3, C4, E4, A4
		// Chord B (Cmaj7): C3, G3, B3, E4
		static const float kChordA[4] = { 174.625f, 261.625f, 329.625f, 440.000f };
		static const float kChordB[4] = { 130.750f, 196.000f, 247.000f, 329.625f };
		static const float kPadAmp[4] = {   0.26f,    0.20f,    0.16f,    0.10f  };

		// Delicate floating bell/electric-piano notes (wrapped around 8.0s)
		struct MotifNote { float start; float freq; float amp; };
		static const MotifNote kMotif[6] = {
			{ 0.50f, 659.25f, 0.13f }, // E5
			{ 1.75f, 523.25f, 0.12f }, // C5
			{ 3.00f, 440.00f, 0.11f }, // A4
			{ 4.50f, 392.00f, 0.12f }, // G4
			{ 5.75f, 493.88f, 0.11f }, // B4
			{ 6.75f, 587.33f, 0.10f }, // D5
		};

		const float invRate = 1.0f / static_cast<float>(kAmbientRate);
		for (u32 i = 0; i < nsamples; ++i)
		{
			const float t = static_cast<float>(i) * invRate;

			// Smooth raised-cosine crossfade between Fmaj7 (t=0,8) and Cmaj7 (t=4)
			const float wA = 0.5f + 0.5f * cosf(kTwoPi * (t / kDurSec));
			const float wB = 1.0f - wA;

			float padA = 0.0f;
			float padB = 0.0f;
			for (int v = 0; v < 4; ++v)
			{
				padA += kPadAmp[v] * sinf(kTwoPi * kChordA[v] * t);
				padB += kPadAmp[v] * sinf(kTwoPi * kChordB[v] * t);
			}
			// Gentle breathing swell
			const float swell = 0.90f + 0.10f * sinf(kTwoPi * (t / kDurSec));
			float sample = (padA * wA + padB * wB) * swell * 0.55f;

			// Soft melodic notes with wrapped decay tails for seamless loop boundary
			for (int m = 0; m < 6; ++m)
			{
				float dt = t - kMotif[m].start;
				if (dt < 0.0f) dt += kDurSec;
				if (dt < 3.6f)
				{
					const float env = SmoothAttack(dt, 0.030f) * expf(-dt * 1.45f);
					// Pure warm fundamental + tiny soft octave shimmer that fades fast
					const float ph = kTwoPi * kMotif[m].freq * dt;
					const float shimmer = 0.08f * expf(-dt * 5.0f) * sinf(ph * 2.0f);
					sample += kMotif[m].amp * env * (sinf(ph) + shimmer);
				}
			}

			out.pcm[i] = ClampS16(sample * 0.72f);
		}

		DSP_FlushDataCache(out.pcm, out.byteSize);
		return true;
	}

	// Synthesizes crisp, gentle 3DS-inspired UI SFX into Linear RAM (~28 KiB total).
	void BuildSfxClip(FrontendAudio::ESfxId id, AudioClip & out)
	{
		static const char * const kOverrideNames[FrontendAudio::SFX_COUNT] = {
			"move.wav",
			"select.wav",
			"back.wav",
			"toggle.wav",
			"launch.wav",
			"error.wav"
		};

		if (TryLoadWavOverride(kOverrideNames[id], out, kMaxSfxBytes))
			return;

		const float invRate = 1.0f / static_cast<float>(kSfxRate);

		switch (id)
		{
		case FrontendAudio::SFX_MOVE:
		{
			// Crisp glassy 3DS cursor tick (~36 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.036f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			float phase = 0.0f;
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float u = static_cast<float>(i) / static_cast<float>(n);
				const float freq = 820.0f - 240.0f * u;
				phase += kTwoPi * freq * invRate;
				const float env = SmoothAttack(t, 0.002f) * expf(-t * 95.0f) * (1.0f - u);
				const float s = (sinf(phase) + 0.25f * sinf(phase * 2.0f)) * env * 0.48f;
				out.pcm[i] = ClampS16(s);
			}
			break;
		}

		case FrontendAudio::SFX_SELECT:
		{
			// Bright ascending two-note chime C6 -> G6 (~130 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.130f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float tailFade = (i > n - 120) ? (static_cast<float>(n - i) / 120.0f) : 1.0f;
				float s = 0.0f;
				// Note 1: C6 (1046.5 Hz)
				const float e1 = SmoothAttack(t, 0.0025f) * expf(-t * 28.0f);
				s += (sinf(kTwoPi * 1046.5f * t) + 0.20f * sinf(kTwoPi * 2093.0f * t)) * e1 * 0.36f;
				// Note 2: G6 (1568.0 Hz) entering at 32 ms
				if (t >= 0.032f)
				{
					const float t2 = t - 0.032f;
					const float e2 = SmoothAttack(t2, 0.0025f) * expf(-t2 * 24.0f);
					s += (sinf(kTwoPi * 1567.98f * t2) + 0.18f * sinf(kTwoPi * 3135.96f * t2)) * e2 * 0.34f;
				}
				out.pcm[i] = ClampS16(s * tailFade);
			}
			break;
		}

		case FrontendAudio::SFX_BACK:
		{
			// Soft descending chime G5 -> D5 (~110 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.110f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float tailFade = (i > n - 120) ? (static_cast<float>(n - i) / 120.0f) : 1.0f;
				float s = 0.0f;
				const float e1 = SmoothAttack(t, 0.0025f) * expf(-t * 32.0f);
				s += sinf(kTwoPi * 783.99f * t) * e1 * 0.36f;
				if (t >= 0.028f)
				{
					const float t2 = t - 0.028f;
					const float e2 = SmoothAttack(t2, 0.0025f) * expf(-t2 * 28.0f);
					s += sinf(kTwoPi * 587.33f * t2) * e2 * 0.36f;
				}
				out.pcm[i] = ClampS16(s * tailFade);
			}
			break;
		}

		case FrontendAudio::SFX_TOGGLE:
		{
			// Snappy tactile switch pop (~55 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.055f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			float phase = 0.0f;
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float u = static_cast<float>(i) / static_cast<float>(n);
				const float freq = 620.0f + 360.0f * u;
				phase += kTwoPi * freq * invRate;
				const float env = SmoothAttack(t, 0.002f) * expf(-t * 58.0f) * (1.0f - u);
				const float s = (sinf(phase) + 0.18f * sinf(phase * 2.0f)) * env * 0.45f;
				out.pcm[i] = ClampS16(s);
			}
			break;
		}

		case FrontendAudio::SFX_LAUNCH:
		{
			// Warm 4-note N64/3DS shimmer arpeggio C5-E5-G5-C6 (~260 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.260f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			static const float kNotes[4]  = { 523.25f, 659.25f, 783.99f, 1046.50f };
			static const float kStarts[4] = {   0.000f,  0.035f,  0.070f,   0.105f };
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float tailFade = (i > n - 220) ? (static_cast<float>(n - i) / 220.0f) : 1.0f;
				float s = 0.0f;
				for (int k = 0; k < 4; ++k)
				{
					if (t >= kStarts[k])
					{
						const float tk = t - kStarts[k];
						const float ek = SmoothAttack(tk, 0.003f) * expf(-tk * 14.0f);
						s += (sinf(kTwoPi * kNotes[k] * tk) + 0.22f * sinf(kTwoPi * kNotes[k] * 2.0f * tk)) * ek * 0.24f;
					}
				}
				out.pcm[i] = ClampS16(s * tailFade);
			}
			break;
		}

		case FrontendAudio::SFX_ERROR:
		{
			// Subtle muted double-thud (~120 ms)
			const u32 n = static_cast<u32>(kSfxRate * 0.120f);
			if (!AllocClip(out, n, kSfxRate, false)) return;
			for (u32 i = 0; i < n; ++i)
			{
				const float t = static_cast<float>(i) * invRate;
				const float tailFade = (i > n - 120) ? (static_cast<float>(n - i) / 120.0f) : 1.0f;
				float s = 0.0f;
				const float e1 = SmoothAttack(t, 0.002f) * expf(-t * 48.0f);
				s += sinf(kTwoPi * 240.0f * t) * e1 * 0.42f;
				if (t >= 0.052f)
				{
					const float t2 = t - 0.052f;
					const float e2 = SmoothAttack(t2, 0.002f) * expf(-t2 * 44.0f);
					s += sinf(kTwoPi * 185.0f * t2) * e2 * 0.42f;
				}
				out.pcm[i] = ClampS16(s * tailFade);
			}
			break;
		}

		default:
			return;
		}

		if (out.pcm != nullptr)
			DSP_FlushDataCache(out.pcm, out.byteSize);
	}

	void StartAmbientHardwareLoop(float gain)
	{
		if (!s_active || s_ambient.pcm == nullptr)
			return;

		s_ambientGain = gain;
		if (s_ambientGain < 0.0f) s_ambientGain = 0.0f;
		if (s_ambientGain > 1.0f) s_ambientGain = 1.0f;

		ndspChnReset(kAmbientCh);
		ndspChnSetInterp(kAmbientCh, NDSP_INTERP_LINEAR);
		ndspChnSetRate(kAmbientCh, static_cast<float>(s_ambient.sampleRate));
		ndspChnSetFormat(kAmbientCh, s_ambient.format);

		float mix[12] = {};
		mix[0] = s_ambientGain;
		mix[1] = s_ambientGain;
		ndspChnSetMix(kAmbientCh, mix);

		std::memset(&s_ambientWbuf, 0, sizeof(s_ambientWbuf));
		s_ambientWbuf.data_vaddr = s_ambient.pcm;
		s_ambientWbuf.nsamples   = s_ambient.nsamples;
		s_ambientWbuf.looping    = true; // Hardware DSP infinite loop: 0% ARM11 CPU!

		ndspChnWaveBufAdd(kAmbientCh, &s_ambientWbuf);
		s_ambientActive = true;
	}
} // namespace

namespace FrontendAudio
{

void Begin(bool withAmbient, float ambientGain)
{
	if (s_active)
	{
		if (withAmbient)
		{
			if (!s_ambientActive)
			{
				if (s_ambient.pcm == nullptr)
					BuildAmbientClip(s_ambient);
				StartAmbientHardwareLoop(ambientGain);
			}
			else
			{
				SetAmbientGain(ambientGain);
			}
		}
		return;
	}

	// If AudioOutput (Channel 0) already initialized ndsp during gameplay,
	// share the active DSP instance without calling ndspInit()/ndspExit().
	if (!audioOpen)
	{
		if (R_FAILED(ndspInit()))
			return; // Missing dspfirm.cdc or DSP unavailable: degrade silently.
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		s_ownsNdsp = true;
	}
	else
	{
		s_ownsNdsp = false;
	}

	s_active = true;
	s_ambientActive = false;
	s_nextVoice = 0;

	// Prepare SFX hardware voices (Channels 2-3).
	float sfxMix[12] = {};
	sfxMix[0] = 0.65f;
	sfxMix[1] = 0.65f;
	for (int v = 0; v < kSfxVoices; ++v)
	{
		const int ch = kSfxChBase + v;
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, static_cast<float>(kSfxRate));
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
		ndspChnSetMix(ch, sfxMix);
		std::memset(&s_sfxVoiceBuf[v], 0, sizeof(s_sfxVoiceBuf[v]));
	}

	for (int i = 0; i < SFX_COUNT; ++i)
		BuildSfxClip(static_cast<ESfxId>(i), s_sfx[i]);

	if (withAmbient && BuildAmbientClip(s_ambient))
		StartAmbientHardwareLoop(ambientGain);
}

void PlaySfx(ESfxId id)
{
	if (!s_active)
		return;
	const int idx = static_cast<int>(id);
	if (idx < 0 || idx >= SFX_COUNT)
		return;

	const AudioClip & clip = s_sfx[idx];
	if (clip.pcm == nullptr || clip.nsamples == 0)
		return;

	const int voice = s_nextVoice;
	s_nextVoice = (s_nextVoice + 1) % kSfxVoices;
	const int ch = kSfxChBase + voice;

	ndspChnWaveBufClear(ch);
	ndspChnSetRate(ch, static_cast<float>(clip.sampleRate));
	ndspChnSetFormat(ch, clip.format);

	ndspWaveBuf & wbuf = s_sfxVoiceBuf[voice];
	std::memset(&wbuf, 0, sizeof(wbuf));
	wbuf.data_vaddr = clip.pcm;
	wbuf.nsamples   = clip.nsamples;
	wbuf.looping    = false;

	ndspChnWaveBufAdd(ch, &wbuf);
}

void SetAmbientGain(float gain)
{
	if (!s_active || !s_ambientActive)
		return;
	if (gain < 0.0f) gain = 0.0f;
	if (gain > 1.0f) gain = 1.0f;
	s_ambientGain = gain;

	float mix[12] = {};
	mix[0] = s_ambientGain;
	mix[1] = s_ambientGain;
	ndspChnSetMix(kAmbientCh, mix);
}

void End()
{
	if (!s_active)
		return;

	ndspChnWaveBufClear(kAmbientCh);
	ndspChnReset(kAmbientCh);
	for (int v = 0; v < kSfxVoices; ++v)
	{
		const int ch = kSfxChBase + v;
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
	}

	if (s_ownsNdsp)
	{
		ndspExit();
		s_ownsNdsp = false;
	}
	else
	{
		// When sharing ndsp with active N64 game AudioOutput (Channel 0), wait one
		// DSP frame (~5 ms) so any in-flight DMA finishes before freeing Linear RAM.
		svcSleepThread(5000000LL);
	}

	FreeClip(s_ambient);
	for (int i = 0; i < SFX_COUNT; ++i)
		FreeClip(s_sfx[i]);

	s_ambientActive = false;
	s_active = false;
}

} // namespace FrontendAudio
