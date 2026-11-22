// FrontendAudio: Zero-CPU-overhead UI sound effects and looping ambient audio
// for the DaedalusX3D 3DS frontend.
//
// Runs entirely on the Nintendo 3DS TeakLite DSP (ndsp) hardware channels 1-3:
//   - Channel 0 is reserved exclusively for N64 game audio (AudioOutput.cpp).
//   - Channel 1 plays a hardware-looping ambient soundscape (waveBuf.looping = true):
//     submitted once to the DSP, 0% ARM11 CPU usage thereafter (no callbacks/threads).
//   - Channels 2-3 provide 2-voice hardware-mixed polyphony for pre-rendered UI SFX.
//
// All Linear RAM buffers are freed via End() before emulation starts so gameplay
// incurs 0 bytes of RAM overhead, 0 DSP channel usage, and 0 CPU overhead.
#ifndef FRONTEND_AUDIO_H
#define FRONTEND_AUDIO_H

namespace FrontendAudio
{
	enum ESfxId
	{
		SFX_MOVE = 0,
		SFX_SELECT,
		SFX_BACK,
		SFX_TOGGLE,
		SFX_LAUNCH,
		SFX_ERROR,
		SFX_COUNT
	};

	// Prepares hardware DSP channels and pre-renders (or loads optional SD overrides for)
	// UI SFX and optional seamless ambient loop into Linear RAM.
	// Safe to call repeatedly (refreshes ambient volume if already active).
	void Begin(bool withAmbient, float ambientGain = 0.22f);

	// Triggers a pre-rendered UI sound effect directly on a hardware DSP channel.
	// Non-blocking, zero CPU mixing.
	void PlaySfx(ESfxId id);

	// Adjusts the ambient channel hardware gain (0.0f .. 1.0f). Zero CPU cost.
	void SetAmbientGain(float gain);

	// Stops all frontend DSP channels, releases 100% of Linear RAM buffers, and
	// releases ndsp if FrontendAudio initialized it (leaving game AudioOutput untouched).
	void End();
}

#endif // FRONTEND_AUDIO_H
