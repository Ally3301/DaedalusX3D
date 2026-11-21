#include "UISound.h"

#include <cmath>
#include <cstring>

#ifdef DAEDALUS_CTR
#include "stdafx.h"
#include <3ds.h>
#include "SysCTR/HLEAudio/AudioOutput.h"
#endif

namespace UI
{
namespace Sound
{

namespace
{

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

ESfxVolume s_volume = SFX_VOL_MEDIUM;

struct NoteSpec
{
	float startSec;
	float durSec;
	float freqStart;
	float freqEnd;
	float glideSec;
	float attackSec;
	float decayRate;
	float amp;
	float harm2;
	float harm3;
};

struct SoundSpec
{
	uint32_t       durationMs;
	int            noteCount;
	const NoteSpec * notes;
};

// Cohesive Nintendo 3DS-inspired harmonic palette (C major / G major pentatonic).
// Every tone uses phase-continuous frequency glides, raised-cosine attacks,
// faster-decaying upper harmonics (marimba/celesta timbre), and zero-crossing tails.

static const NoteSpec kNotesNavigate[] = {
	{ 0.000f, 0.036f,  880.0f,  740.0f, 0.010f, 0.0020f,  95.0f, 0.58f, 0.22f, 0.06f },
	{ 0.000f, 0.016f, 1480.0f, 1480.0f, 0.001f, 0.0010f, 180.0f, 0.14f, 0.00f, 0.00f },
};

static const NoteSpec kNotesConfirm[] = {
	{ 0.000f, 0.075f, 1046.5f, 1046.5f, 0.001f, 0.0030f,  32.0f, 0.55f, 0.20f, 0.05f }, // C6
	{ 0.038f, 0.090f, 1568.0f, 1568.0f, 0.001f, 0.0030f,  26.0f, 0.62f, 0.18f, 0.04f }, // G6
};

static const NoteSpec kNotesBack[] = {
	{ 0.000f, 0.065f,  784.0f,  784.0f, 0.001f, 0.0030f,  34.0f, 0.52f, 0.16f, 0.03f }, // G5
	{ 0.036f, 0.082f,  587.3f,  587.3f, 0.001f, 0.0040f,  28.0f, 0.48f, 0.12f, 0.02f }, // D5
};

static const NoteSpec kNotesToggleOn[] = {
	{ 0.000f, 0.055f,  659.3f,  659.3f, 0.001f, 0.0020f,  42.0f, 0.50f, 0.20f, 0.04f }, // E5
	{ 0.028f, 0.070f,  987.8f,  987.8f, 0.001f, 0.0020f,  34.0f, 0.58f, 0.18f, 0.05f }, // B5
};

static const NoteSpec kNotesToggleOff[] = {
	{ 0.000f, 0.052f,  987.8f,  987.8f, 0.001f, 0.0020f,  44.0f, 0.46f, 0.15f, 0.03f }, // B5
	{ 0.028f, 0.070f,  659.3f,  659.3f, 0.001f, 0.0030f,  34.0f, 0.48f, 0.12f, 0.02f }, // E5
};

static const NoteSpec kNotesAdjust[] = {
	{ 0.000f, 0.052f,  860.0f,  880.0f, 0.008f, 0.0020f,  58.0f, 0.56f, 0.24f, 0.06f }, // A5
};

static const NoteSpec kNotesPauseOpen[] = {
	{ 0.000f, 0.155f,  523.3f,  523.3f, 0.001f, 0.0040f,  18.0f, 0.36f, 0.18f, 0.04f }, // C5
	{ 0.022f, 0.145f,  659.3f,  659.3f, 0.001f, 0.0040f,  18.0f, 0.36f, 0.16f, 0.04f }, // E5
	{ 0.044f, 0.128f,  784.0f,  784.0f, 0.001f, 0.0040f,  20.0f, 0.38f, 0.15f, 0.03f }, // G5
	{ 0.066f, 0.106f,  987.8f,  987.8f, 0.001f, 0.0040f,  22.0f, 0.34f, 0.12f, 0.02f }, // B5
};

static const NoteSpec kNotesPauseClose[] = {
	{ 0.000f, 0.075f,  784.0f,  784.0f, 0.001f, 0.0030f,  32.0f, 0.42f, 0.16f, 0.03f }, // G5
	{ 0.024f, 0.080f,  659.3f,  659.3f, 0.001f, 0.0030f,  28.0f, 0.42f, 0.14f, 0.03f }, // E5
	{ 0.048f, 0.080f,  523.3f,  523.3f, 0.001f, 0.0030f,  26.0f, 0.46f, 0.12f, 0.02f }, // C5
};

static const NoteSpec kNotesLaunchGame[] = {
	{ 0.000f, 0.180f,  523.3f,  523.3f, 0.001f, 0.0040f,  16.0f, 0.34f, 0.20f, 0.05f }, // C5
	{ 0.042f, 0.185f,  659.3f,  659.3f, 0.001f, 0.0040f,  15.0f, 0.36f, 0.20f, 0.05f }, // E5
	{ 0.084f, 0.180f,  784.0f,  784.0f, 0.001f, 0.0040f,  15.0f, 0.38f, 0.18f, 0.04f }, // G5
	{ 0.126f, 0.152f, 1046.5f, 1046.5f, 0.001f, 0.0030f,  16.0f, 0.46f, 0.18f, 0.05f }, // C6
	{ 0.126f, 0.100f, 2093.0f, 2093.0f, 0.001f, 0.0050f,  28.0f, 0.12f, 0.00f, 0.00f }, // C7 sparkle
};

static const NoteSpec kNotesSaveState[] = {
	{ 0.000f, 0.110f,  698.5f,  698.5f, 0.001f, 0.0030f,  24.0f, 0.40f, 0.18f, 0.04f }, // F5
	{ 0.042f, 0.115f,  880.0f,  880.0f, 0.001f, 0.0030f,  22.0f, 0.44f, 0.18f, 0.04f }, // A5
	{ 0.084f, 0.098f, 1046.5f, 1046.5f, 0.001f, 0.0030f,  22.0f, 0.50f, 0.16f, 0.04f }, // C6
};

static const NoteSpec kNotesLoadState[] = {
	{ 0.000f, 0.115f,  587.3f,  587.3f, 0.001f, 0.0030f,  22.0f, 0.38f, 0.16f, 0.04f }, // D5
	{ 0.040f, 0.120f,  880.0f,  880.0f, 0.001f, 0.0030f,  20.0f, 0.42f, 0.16f, 0.04f }, // A5
	{ 0.080f, 0.102f, 1174.7f, 1174.7f, 0.001f, 0.0030f,  20.0f, 0.44f, 0.14f, 0.03f }, // D6
	{ 0.080f, 0.102f, 1178.7f, 1178.7f, 0.001f, 0.0040f,  22.0f, 0.18f, 0.00f, 0.00f }, // D6+4Hz shimmer
};

static const NoteSpec kNotesError[] = {
	{ 0.000f, 0.062f,  240.0f,  210.0f, 0.025f, 0.0030f,  38.0f, 0.58f, 0.30f, 0.10f },
	{ 0.068f, 0.082f,  205.0f,  175.0f, 0.030f, 0.0030f,  32.0f, 0.55f, 0.28f, 0.08f },
};

static const NoteSpec kNotesStartup[] = {
	{ 0.000f, 0.180f,  784.0f,  784.0f, 0.001f, 0.0050f,  15.0f, 0.30f, 0.15f, 0.03f }, // G5
	{ 0.055f, 0.175f,  987.8f,  987.8f, 0.001f, 0.0050f,  15.0f, 0.32f, 0.14f, 0.03f }, // B5
	{ 0.110f, 0.148f, 1174.7f, 1174.7f, 0.001f, 0.0050f,  16.0f, 0.36f, 0.14f, 0.03f }, // D6
};

#define MAKE_SPEC(ms, arr) { (ms), (int)(sizeof(arr) / sizeof((arr)[0])), (arr) }

static const SoundSpec kSoundSpecs[SFX_COUNT] = {
	MAKE_SPEC( 38u, kNotesNavigate),
	MAKE_SPEC(130u, kNotesConfirm),
	MAKE_SPEC(120u, kNotesBack),
	MAKE_SPEC(100u, kNotesToggleOn),
	MAKE_SPEC(100u, kNotesToggleOff),
	MAKE_SPEC( 55u, kNotesAdjust),
	MAKE_SPEC(175u, kNotesPauseOpen),
	MAKE_SPEC(130u, kNotesPauseClose),
	MAKE_SPEC(280u, kNotesLaunchGame),
	MAKE_SPEC(185u, kNotesSaveState),
	MAKE_SPEC(185u, kNotesLoadState),
	MAKE_SPEC(155u, kNotesError),
	MAKE_SPEC(260u, kNotesStartup),
};

#undef MAKE_SPEC

constexpr int kMaxNotesPerSound = 6;

static inline size_t AlignUp(size_t value, size_t align)
{
	return (value + align - 1u) & ~(align - 1u);
}

static inline float ClampF(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

// ============================================================================
// Pure, host-testable volume, synthesis, and polyphonic voice pool API
// ============================================================================

const char * VolumeName(ESfxVolume level)
{
	switch (level)
	{
	case SFX_VOL_OFF:    return "Off";
	case SFX_VOL_LOW:    return "Low";
	case SFX_VOL_MEDIUM: return "Medium";
	case SFX_VOL_HIGH:   return "High";
	default:             return "Medium";
	}
}

float VolumeGain(ESfxVolume level)
{
	switch (level)
	{
	case SFX_VOL_OFF:    return 0.0f;
	case SFX_VOL_LOW:    return 0.32f;
	case SFX_VOL_MEDIUM: return 0.62f;
	case SFX_VOL_HIGH:   return 0.95f;
	default:             return 0.62f;
	}
}

float ClampPitch(float pitchScale)
{
	return ClampF(pitchScale, 0.5f, 2.0f);
}

void ComputeStereoPan(float gain, float pan, float & outLeft, float & outRight)
{
	const float g = ClampF(gain, 0.0f, 1.0f);
	const float p = ClampF(pan, -1.0f, 1.0f);
	// Constant-power-like gentle pan so UI sounds never collapse on one speaker.
	const float angle = (p + 1.0f) * 0.25f * kPi; // [0, pi/2]
	outLeft  = g * cosf(angle) * 1.41421356f;
	outRight = g * sinf(angle) * 1.41421356f;
	if (outLeft > 1.0f)  outLeft = 1.0f;
	if (outRight > 1.0f) outRight = 1.0f;
}

ESfxVolume GetVolume()
{
	return s_volume;
}

void SetVolume(ESfxVolume level)
{
	if ((int)level < 0 || (int)level >= SFX_VOL_COUNT)
		level = SFX_VOL_MEDIUM;
	s_volume = level;
}

size_t GetSampleCount(ESoundId id)
{
	if ((int)id < 0 || (int)id >= SFX_COUNT)
		return 0u;
	return (size_t)((kSampleRate * kSoundSpecs[id].durationMs) / 1000u);
}

size_t ComputeTotalBankBytes()
{
	size_t total = 0u;
	for (int i = 0; i < SFX_COUNT; ++i)
	{
		const size_t bytes = GetSampleCount((ESoundId)i) * sizeof(int16_t);
		total += AlignUp(bytes, kClipAlignBytes);
	}
	return total;
}

size_t Synthesize(ESoundId id, int16_t * outSamples, size_t maxSamples)
{
	if (!outSamples || (int)id < 0 || (int)id >= SFX_COUNT)
		return 0u;

	const size_t nSamples = GetSampleCount(id);
	if (nSamples == 0u || maxSamples < nSamples)
		return 0u;

	const SoundSpec & spec = kSoundSpecs[id];
	const int noteCount = (spec.noteCount > kMaxNotesPerSound) ? kMaxNotesPerSound : spec.noteCount;

	float phase[kMaxNotesPerSound];
	for (int k = 0; k < kMaxNotesPerSound; ++k)
		phase[k] = 0.0f;

	const float invSr = 1.0f / (float)kSampleRate;
	const size_t tailFadeSamples = (nSamples > 96u) ? 96u : (nSamples / 4u);
	constexpr float kPeakScale = 24576.0f; // Safe headroom for 4-voice DSP mixing

	for (size_t i = 0u; i < nSamples; ++i)
	{
		const float tGlobal = (float)i * invSr;
		float sum = 0.0f;

		for (int k = 0; k < noteCount; ++k)
		{
			const NoteSpec & n = spec.notes[k];
			if (tGlobal < n.startSec)
				continue;
			const float lt = tGlobal - n.startSec;
			if (lt >= n.durSec)
				continue;

			// Instantaneous frequency with smooth linear glide
			float freq = n.freqEnd;
			if (n.glideSec > 0.0f && lt < n.glideSec)
			{
				const float frac = lt / n.glideSec;
				freq = n.freqStart + (n.freqEnd - n.freqStart) * frac;
			}

			// Smooth raised-cosine attack and exponential decay
			float env = expf(-n.decayRate * lt);
			if (n.attackSec > 0.0f && lt < n.attackSec)
			{
				const float a = lt / n.attackSec;
				env *= 0.5f * (1.0f - cosf(kPi * a));
			}

			// Smooth note-end release over last 6 ms to guarantee zero discontinuity
			const float rem = n.durSec - lt;
			constexpr float kRelSec = 0.006f;
			if (rem < kRelSec)
			{
				const float r = rem / kRelSec;
				env *= 0.5f * (1.0f - cosf(kPi * r));
			}

			// Fundamental + faster-decaying warm 2nd and 3rd harmonics
			const float ph = phase[k];
			float wave = sinf(ph);
			if (n.harm2 > 0.0f)
				wave += n.harm2 * sinf(2.0f * ph) * expf(-1.2f * n.decayRate * lt);
			if (n.harm3 > 0.0f)
				wave += n.harm3 * sinf(3.0f * ph) * expf(-2.8f * n.decayRate * lt);

			sum += wave * env * n.amp;

			phase[k] += kTwoPi * freq * invSr;
			if (phase[k] >= kTwoPi)
				phase[k] -= kTwoPi;
		}

		// Guarantee exact click-free zero boundary at start and tail of buffer
		if (i == 0u)
		{
			sum = 0.0f;
		}
		else if (tailFadeSamples > 0u && i >= nSamples - tailFadeSamples)
		{
			const float r = (float)(nSamples - 1u - i) / (float)tailFadeSamples;
			sum *= 0.5f * (1.0f - cosf(kPi * r));
		}

		// Soft saturation limiter prevents clipping while preserving warm tone
		const float limited = tanhf(sum);
		int32_t pcm = (int32_t)(limited * kPeakScale);
		if (pcm > 32767) pcm = 32767;
		if (pcm < -32768) pcm = -32768;
		outSamples[i] = (int16_t)pcm;
	}

	return nSamples;
}

VoicePool::VoicePool()
	: mNextSeq(1u)
	, mLastNavMs(0u)
	, mHasNav(false)
{
	Reset();
}

void VoicePool::Reset()
{
	for (int i = 0; i < kVoiceCount; ++i)
		mVoiceSeq[i] = 0u;
	mNextSeq   = 1u;
	mLastNavMs = 0u;
	mHasNav    = false;
}

bool VoicePool::ShouldSuppress(ESoundId id, uint64_t nowMs) const
{
	if (id == SFX_NAVIGATE && mHasNav)
	{
		if (nowMs >= mLastNavMs && (nowMs - mLastNavMs) < kNavigateMinIntervalMs)
			return true;
	}
	return false;
}

int VoicePool::Allocate(const bool voiceFree[kVoiceCount], ESoundId id, uint64_t nowMs, bool & outStolen)
{
	outStolen = false;
	if (ShouldSuppress(id, nowMs))
		return -1;

	int chosen = -1;
	for (int i = 0; i < kVoiceCount; ++i)
	{
		if (voiceFree[i])
		{
			chosen = i;
			break;
		}
	}

	if (chosen < 0)
	{
		// All voices busy: steal the oldest voice so newer chimes ring clearly.
		outStolen = true;
		chosen = 0;
		uint64_t oldest = mVoiceSeq[0];
		for (int i = 1; i < kVoiceCount; ++i)
		{
			if (mVoiceSeq[i] < oldest)
			{
				oldest = mVoiceSeq[i];
				chosen = i;
			}
		}
	}

	mVoiceSeq[chosen] = mNextSeq++;
	if (id == SFX_NAVIGATE)
	{
		mLastNavMs = nowMs;
		mHasNav    = true;
	}
	return chosen;
}

// ============================================================================
// Nintendo 3DS NDSP Hardware Backend (channels 1..4, zero conflict with ch 0)
// ============================================================================
#ifdef DAEDALUS_CTR

namespace
{

uint8_t *    s_pcmBank = nullptr;
size_t       s_pcmBankBytes = 0u;
int16_t *    s_clipPtr[SFX_COUNT] = {};
size_t       s_clipLen[SFX_COUNT] = {};
ndspWaveBuf  s_chanBuf[kVoiceCount];
VoicePool    s_voicePool;
bool         s_ready = false;
bool         s_initAttempted = false;

static inline uint64_t NowMs()
{
	return (uint64_t)((svcGetSystemTick() * 1000ULL) / SYSCLOCK_ARM11);
}

bool EnsureReady()
{
	if (s_ready)
		return true;
	if (s_initAttempted)
		return false;
	s_initAttempted = true;

	if (!AudioOutput::AcquireNdsp())
		return false;

	const size_t totalBytes = ComputeTotalBankBytes();
	s_pcmBank = static_cast<uint8_t *>(linearAlloc(totalBytes));
	if (!s_pcmBank)
	{
		AudioOutput::ReleaseNdsp();
		return false;
	}
	s_pcmBankBytes = totalBytes;
	std::memset(s_pcmBank, 0, totalBytes);

	size_t offset = 0u;
	for (int i = 0; i < SFX_COUNT; ++i)
	{
		const size_t n = GetSampleCount((ESoundId)i);
		int16_t * dst = reinterpret_cast<int16_t *>(s_pcmBank + offset);
		s_clipPtr[i] = dst;
		s_clipLen[i] = Synthesize((ESoundId)i, dst, n);
		offset += AlignUp(n * sizeof(int16_t), kClipAlignBytes);
	}

	DSP_FlushDataCache(s_pcmBank, static_cast<u32>(s_pcmBankBytes));

	for (int v = 0; v < kVoiceCount; ++v)
	{
		const int ch = kFirstChannel + v;
		std::memset(&s_chanBuf[v], 0, sizeof(s_chanBuf[v]));
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, (float)kSampleRate);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	}

	s_voicePool.Reset();
	s_ready = true;
	return true;
}

void PlayInternal(ESoundId id, float gain, float pitchScale, float pan)
{
	if ((int)id < 0 || (int)id >= SFX_COUNT || gain <= 0.001f)
		return;
	if (!EnsureReady())
		return;
	if (!s_clipPtr[id] || s_clipLen[id] == 0u)
		return;

	bool voiceFree[kVoiceCount];
	for (int v = 0; v < kVoiceCount; ++v)
	{
		const u8 st = s_chanBuf[v].status;
		voiceFree[v] = (st == NDSP_WBUF_FREE || st == NDSP_WBUF_DONE);
	}

	bool stolen = false;
	const int slot = s_voicePool.Allocate(voiceFree, id, NowMs(), stolen);
	if (slot < 0)
		return;

	const int ch = kFirstChannel + slot;
	if (stolen)
		ndspChnWaveBufClear(ch);

	const float clampedPitch = ClampPitch(pitchScale);
	float mix[12] = {};
	ComputeStereoPan(gain, pan, mix[0], mix[1]);

	ndspChnSetRate(ch, (float)kSampleRate * clampedPitch);
	ndspChnSetMix(ch, mix);

	std::memset(&s_chanBuf[slot], 0, sizeof(s_chanBuf[slot]));
	s_chanBuf[slot].data_vaddr = s_clipPtr[id];
	s_chanBuf[slot].nsamples   = static_cast<u32>(s_clipLen[id]);
	s_chanBuf[slot].looping    = false;

	ndspChnWaveBufAdd(ch, &s_chanBuf[slot]);
}

} // namespace

bool Init()
{
	// If user disabled UI SFX in frontend.ini, defer NDSP/RAM allocation until enabled.
	if (s_volume == SFX_VOL_OFF)
		return true;
	return EnsureReady();
}

void Shutdown()
{
	if (s_ready)
	{
		StopAll();
		// Wait briefly for DSP frame to finish clearing channels 1..4 before freeing Linear RAM.
		for (int wait = 0; wait < 20; ++wait)
		{
			bool anyPlaying = false;
			for (int v = 0; v < kVoiceCount; ++v)
			{
				if (ndspChnIsPlaying(kFirstChannel + v))
				{
					anyPlaying = true;
					break;
				}
			}
			if (!anyPlaying)
				break;
			svcSleepThread(1000000LL);
		}
		AudioOutput::ReleaseNdsp();
		s_ready = false;
	}

	if (s_pcmBank != nullptr)
	{
		linearFree(s_pcmBank);
		s_pcmBank = nullptr;
	}
	s_pcmBankBytes = 0u;
	for (int i = 0; i < SFX_COUNT; ++i)
	{
		s_clipPtr[i] = nullptr;
		s_clipLen[i] = 0u;
	}
	s_voicePool.Reset();
	s_initAttempted = false;
}

bool IsReady()
{
	return s_ready;
}

void Play(ESoundId id, float pitchScale, float pan)
{
	if (s_volume == SFX_VOL_OFF)
		return;
	PlayInternal(id, VolumeGain(s_volume), pitchScale, pan);
}

void PlayPreview(ESoundId id, ESfxVolume previewLevel, float pitchScale)
{
	if (previewLevel == SFX_VOL_OFF)
		return;
	PlayInternal(id, VolumeGain(previewLevel), pitchScale, 0.0f);
}

void StopAll()
{
	if (!s_ready)
		return;
	for (int v = 0; v < kVoiceCount; ++v)
	{
		ndspChnWaveBufClear(kFirstChannel + v);
		std::memset(&s_chanBuf[v], 0, sizeof(s_chanBuf[v]));
	}
	s_voicePool.Reset();
}

#endif // DAEDALUS_CTR

} // namespace Sound
} // namespace UI
