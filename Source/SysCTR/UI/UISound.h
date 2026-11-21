#pragma once

#include <stddef.h>
#include <stdint.h>

namespace UI
{

enum ESoundId
{
	SFX_NAVIGATE = 0, // Subtle tactile marimba/glass pip when moving selection
	SFX_CONFIRM,      // Bright rising 3DS two-note chime (OK / open submenu)
	SFX_BACK,         // Gentle descending two-note chime (Back / Cancel)
	SFX_TOGGLE_ON,    // Crisp upward switch-on pip (enable / favorite / wake)
	SFX_TOGGLE_OFF,   // Soft downward switch-off pip (disable / unfavorite / sleep)
	SFX_ADJUST,       // Clean stepper pip for cycling values, sort, or save slots
	SFX_PAUSE_OPEN,   // Warm glassy Cmaj7 chord bloom when pausing gameplay
	SFX_PAUSE_CLOSE,  // Snappy descending resolve when resuming gameplay
	SFX_LAUNCH_GAME,  // Celebratory rising arpeggio when launching a ROM
	SFX_SAVE_STATE,   // Confident rising triad chime when saving state
	SFX_LOAD_STATE,   // Airy crystalline shimmer chime when loading state
	SFX_ERROR,        // Soft non-harsh low double-bump (empty slot / invalid)
	SFX_STARTUP,      // Subtle welcoming triad on entering ROM browser
	SFX_COUNT
};

enum ESfxVolume
{
	SFX_VOL_OFF = 0,
	SFX_VOL_LOW,
	SFX_VOL_MEDIUM,
	SFX_VOL_HIGH,
	SFX_VOL_COUNT
};

namespace Sound
{
	constexpr uint32_t kSampleRate      = 22050u;
	constexpr int      kVoiceCount      = 4;
	constexpr int      kFirstChannel    = 1; // Channel 0 is reserved for N64 AudioOutput
	constexpr size_t   kClipAlignBytes  = 64u;

	const char * VolumeName(ESfxVolume level);
	float        VolumeGain(ESfxVolume level);
	float        ClampPitch(float pitchScale);
	void         ComputeStereoPan(float gain, float pan, float & outLeft, float & outRight);

	// Global UI SFX volume preference (persisted in frontend.ini as ui_sfx=0..3).
	ESfxVolume GetVolume();
	void       SetVolume(ESfxVolume level);

	// Pure, host-testable procedural PCM16 synthesizer (mono, kSampleRate Hz).
	size_t GetSampleCount(ESoundId id);
	size_t ComputeTotalBankBytes();
	size_t Synthesize(ESoundId id, int16_t * outSamples, size_t maxSamples);

	// Pure, host-testable 4-voice polyphonic voice allocator with navigate debounce.
	class VoicePool
	{
	public:
		static constexpr uint64_t kNavigateMinIntervalMs = 35u;

		VoicePool();
		void Reset();

		bool ShouldSuppress(ESoundId id, uint64_t nowMs) const;
		int  Allocate(const bool voiceFree[kVoiceCount], ESoundId id, uint64_t nowMs, bool & outStolen);

	private:
		uint64_t mVoiceSeq[kVoiceCount];
		uint64_t mNextSeq;
		uint64_t mLastNavMs;
		bool     mHasNav;
	};

#ifdef DAEDALUS_CTR
	// Allocates the single Linear RAM PCM bank, synthesizes all clips, and
	// acquires shared NDSP channels 1..4 (safe alongside N64 channel 0).
	bool Init();
	void Shutdown();
	bool IsReady();

	void Play(ESoundId id, float pitchScale = 1.0f, float pan = 0.0f);
	void PlayPreview(ESoundId id, ESfxVolume previewLevel, float pitchScale = 1.0f);
	void StopAll();
#endif

} // namespace Sound

} // namespace UI
