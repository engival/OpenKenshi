#pragma once
#include "audio/wwvorbis.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Stereo float mixer. Voices decode their wem on the fly (WemDecoder), resample to the output rate times their playback rate (4-point Hermite),
// loop on the smpl points, run a one-pole low-pass, and carry gain ramps with Wwise fade curves; buses group them for the option sliders. Starts,
// gain changes and stops can be scheduled on the mixer clock (output frames rendered), so music segments chain sample-accurately. render() is the
// whole mix: the sokol_audio callback calls it (startDevice), tests call it directly to render offline. Voices are opened on the caller's
// thread; the lock is only held to add/change/mix them.
namespace audio
{
	enum Bus : uint8_t { kMusic, kAmbience, kSfx, kVoice, kBusCount };

	// Wwise AkCurveInterpolation order (the value stored in banks).
	enum Curve : uint8_t { kLog3, kSine, kLog1, kInvSCurve, kLinear, kSCurve, kExp1, kSineRecip, kExp3, kConstant };
	// Fade shape at t in [0, 1] for a fade-in (a fade-out runs it from 1 to 0 over the same shape).
	float curveShape(Curve c, float t);

	struct PlayParams
	{
		float gain = 1, pan = 0;	// pan -1 left .. 1 right
		float rate = 1;	// playback rate (pitch): 2 = an octave up
		float lpf = 0;	// Wwise LPF amount, 0 (open) .. 100
		Bus bus = kSfx;
		uint32_t wwBus = 0;	// the Wwise output bus (Init.bnk) the node routes to: its bus volume ramps (the pause duck) apply; 0 = none
		bool loop = false;	// smpl loop points when present, else the whole file
		float fadeIn = 0;	// seconds
		Curve fadeCurve = kLinear;
		uint64_t startAt = 0;	// mixer clock frame to start on (0 or past: at once)
	};

	using VoiceId = uint32_t;	// 0 = none

	class Mixer
	{
	public:
		explicit Mixer(int sampleRate = 44100);
		~Mixer();

		// Opens the default output device through sokol_audio and mixes from its callback. The output rate becomes the device's.
		bool startDevice();
		void stopDevice();
		int sampleRate() const { return rate_; }
		uint64_t clock() const;	// frames rendered so far

		// The wem bytes must outlive the voice (they belong to a loaded Bank). 0 on failure (error()). info: the wem's header, if wanted.
		VoiceId play(const uint8_t* wem, size_t size, const PlayParams& p, WemInfo* info = nullptr);
		// Gain ramp from the current gain, starting at clock frame `at` (0: now). stop() ramps to 0 and then removes the voice.
		void setGain(VoiceId id, float gain, float fadeSeconds = 0, Curve c = kLinear, uint64_t at = 0);
		void stop(VoiceId id, float fadeSeconds = 0, Curve c = kLinear, uint64_t at = 0);
		void setPan(VoiceId id, float pan);
		void setRate(VoiceId id, float rate);
		void setLpf(VoiceId id, float lpf);
		bool playing(VoiceId id) const;
		size_t voices() const;
		void setBusGain(Bus b, float gain);
		// Wwise buses (Init.bnk): the tree, then SetBusVolume (absolute dB) / ResetBusVolume (0 dB) ramps on the mixer clock. A voice gets the
		// sum of the dB of its bus and every ancestor (the pause duck: SFX Bus -96 dB, so its child buses too).
		void setWwBusParent(uint32_t bus, uint32_t parent);
		void setWwBusVolume(uint32_t bus, float db, float fadeSeconds, Curve c);
		float wwBusDb(uint32_t bus) const;	// now, summed up the tree
		void setMasterGain(float gain);
		const std::string& error() const { return error_; }

		// Mixes `frames` stereo frames into out (overwrites) and advances the clock.
		void render(float* out, int frames);

	private:
		struct Voice;
		void ramp(Voice& v, float gain, float fadeSeconds, Curve c, uint64_t at, bool stop);
		void fill(Voice& v, size_t need);
		void mixVoice(Voice& v, float* out, int frames, float bus);
		struct BusRamp { float from = 0, to = 0; uint64_t start = 0, len = 0; Curve curve = kLinear; float at(uint64_t t) const; };
		float wwBusDbLocked(uint32_t bus) const;

		mutable std::mutex lock_;
		std::vector<std::unique_ptr<Voice>> voices_;
		float bus_[kBusCount] = { 1, 1, 1, 1 }, master_ = 1;
		std::unordered_map<uint32_t, uint32_t> wwParent_;	// Wwise bus -> its parent bus
		std::unordered_map<uint32_t, BusRamp> wwBus_;	// Wwise buses with a volume set
		int rate_;
		uint64_t clock_ = 0;
		VoiceId nextId_ = 1;
		bool device_ = false;
		std::string error_;
	};
}
