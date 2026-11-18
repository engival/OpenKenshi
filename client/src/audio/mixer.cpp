#include "audio/mixer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sokol_audio.h>

namespace audio
{
	// AkInterpolation::InterpolateNoCheck as wwiser documents it (generator/render/bnode_rtpc.py): every curve is from + (to - from) * S(t). Wwise
	// evaluates sin/cos with short polynomials; the functions they approximate are used here.
	float curveShape(Curve c, float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		switch (c)
		{
			case kLog3: return 1 - std::pow(1 - t, 3.0f);
			case kSine: return std::sin(t * float(M_PI) * 0.5f);
			case kLog1: return t * (3 - t) * 0.5f;
			case kInvSCurve: return t <= 0.5f ? 0.5f * std::sin(t * float(M_PI)) : 1 - 0.5f * std::sin((1 - t) * float(M_PI));
			case kSCurve: return 0.5f - 0.5f * std::cos(t * float(M_PI));
			case kExp1: return t * (t + 1) * 0.5f;
			case kSineRecip: return 1 - std::cos(t * float(M_PI) * 0.5f);
			case kExp3: return t * t * t;
			case kConstant: return 1;
			default: return t;
		}
	}

	struct Mixer::Voice
	{
		VoiceId id = 0;
		WemDecoder dec;
		PlayParams p;
		int ch = 1;
		// Decoded source frames, interleaved. src frame 0 is a zero frame so the interpolator has a left neighbour at the start.
		std::vector<float> src;
		size_t frames = 1;
		double pos = 1;
		bool eof = false, done = false;
		// Gain ramp on the mixer clock: `from` before rampStart, curve to `to` over rampLen frames, then `to`.
		float from = 1, to = 1;
		uint64_t rampStart = 0, rampLen = 0;
		Curve curve = kLinear;
		bool stopping = false;
		float lp[2] = { 0, 0 };	// low-pass state per output channel

		float gainAt(uint64_t t) const
		{
			if (t < rampStart) return from;
			if (t >= rampStart + rampLen) return to;
			return from + (to - from) * curveShape(curve, float(t - rampStart) / float(rampLen));
		}
	};

	static void logCb(const char* tag, uint32_t level, uint32_t item, const char* msg, uint32_t line, const char* file, void*)
	{
		fprintf(stderr, "[%s][%s][id:%u] %s:%u: %s\n", tag ? tag : "saudio", level == 0 ? "panic" : level == 1 ? "error" : level == 2 ? "warn" : "info", item,
			file ? file : "", line, msg ? msg : "");
	}

	static void streamCb(float* buffer, int frames, int channels, void* user)
	{
		Mixer* m = static_cast<Mixer*>(user);
		if (channels == 2) { m->render(buffer, frames); return; }
		thread_local std::vector<float> stereo;
		stereo.resize(size_t(frames) * 2);
		m->render(stereo.data(), frames);
		for (int i = 0; i < frames; i++)
		{
			for (int c = 0; c < channels; c++) buffer[i * channels + c] = channels == 1 ? 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]) : c < 2 ? stereo[i * 2 + c] : 0.0f;
		}
	}

	Mixer::Mixer(int sampleRate) : rate_(sampleRate) {}
	Mixer::~Mixer() { stopDevice(); }

	bool Mixer::startDevice()
	{
		if (device_) return true;
		saudio_desc d {};
		d.sample_rate = rate_;
		d.num_channels = 2;
		d.stream_userdata_cb = streamCb;
		d.user_data = this;
		d.logger.func = logCb;
		saudio_setup(&d);
		if (!saudio_isvalid()) { error_ = "no audio output device (sokol_audio setup failed)"; saudio_shutdown(); return false; }
		{
			std::lock_guard<std::mutex> g(lock_);
			rate_ = saudio_sample_rate();
		}
		device_ = true;
		return true;
	}

	void Mixer::stopDevice()
	{
		if (!device_) return;
		saudio_shutdown();
		device_ = false;
	}

	uint64_t Mixer::clock() const
	{
		std::lock_guard<std::mutex> g(lock_);
		return clock_;
	}

	VoiceId Mixer::play(const uint8_t* wem, size_t size, const PlayParams& p, WemInfo* info)
	{
		auto v = std::make_unique<Voice>();
		if (!v->dec.open(wem, size)) { error_ = v->dec.error(); return 0; }
		if (info) *info = v->dec.info();
		v->p = p;
		v->ch = v->dec.info().channels;
		v->src.assign(size_t(v->ch), 0.0f);
		std::lock_guard<std::mutex> g(lock_);
		v->p.startAt = std::max(p.startAt, clock_);
		v->to = p.gain;
		if (p.fadeIn > 0)
		{
			v->from = 0;
			v->rampStart = v->p.startAt;
			v->rampLen = uint64_t(p.fadeIn * float(rate_));
			v->curve = p.fadeCurve;
		} else {
			v->from = p.gain;
		}
		v->id = nextId_++;
		if (!nextId_) nextId_ = 1;
		VoiceId id = v->id;
		voices_.push_back(std::move(v));
		return id;
	}

	void Mixer::ramp(Voice& v, float gain, float fadeSeconds, Curve c, uint64_t at, bool stop)
	{
		uint64_t start = std::max(at, clock_);
		v.from = v.gainAt(start);
		v.to = gain;
		v.rampStart = start;
		v.rampLen = uint64_t(std::max(0.0f, fadeSeconds) * float(rate_));
		v.curve = c;
		v.stopping = stop;
	}

	void Mixer::setGain(VoiceId id, float gain, float fadeSeconds, Curve c, uint64_t at)
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			if (v->id == id && !v->stopping) ramp(*v, gain, fadeSeconds, c, at, false);
		}
	}

	void Mixer::stop(VoiceId id, float fadeSeconds, Curve c, uint64_t at)
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			if (v->id == id && !v->stopping) ramp(*v, 0, fadeSeconds, c, at, true);
		}
	}

	void Mixer::setPan(VoiceId id, float pan)
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			if (v->id == id) v->p.pan = std::clamp(pan, -1.0f, 1.0f);
		}
	}

	void Mixer::setRate(VoiceId id, float rate)
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			if (v->id == id) v->p.rate = std::clamp(rate, 0.0625f, 16.0f);
		}
	}

	void Mixer::setLpf(VoiceId id, float lpf)
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			if (v->id == id) v->p.lpf = std::clamp(lpf, 0.0f, 100.0f);
		}
	}

	bool Mixer::playing(VoiceId id) const
	{
		std::lock_guard<std::mutex> g(lock_);
		for (const auto& v : voices_)
		{
			if (v->id == id) return !v->done;
		}
		return false;
	}

	size_t Mixer::voices() const
	{
		std::lock_guard<std::mutex> g(lock_);
		return voices_.size();
	}

	void Mixer::setBusGain(Bus b, float gain) { std::lock_guard<std::mutex> g(lock_); bus_[b] = gain; }
	void Mixer::setMasterGain(float gain) { std::lock_guard<std::mutex> g(lock_); master_ = gain; }

	// Interpolated in dB (UNSURE: Wwise's bus volume transition space is not traced; -96 dB over 1 s in dB is a fast drop, then silence).
	float Mixer::BusRamp::at(uint64_t t) const
	{
		if (t < start) return from;
		if (t >= start + len) return to;
		return from + (to - from) * curveShape(curve, float(t - start) / float(len));
	}

	void Mixer::setWwBusParent(uint32_t bus, uint32_t parent) { std::lock_guard<std::mutex> g(lock_); wwParent_[bus] = parent; }

	void Mixer::setWwBusVolume(uint32_t bus, float db, float fadeSeconds, Curve c)
	{
		std::lock_guard<std::mutex> g(lock_);
		BusRamp& r = wwBus_[bus];
		r = { r.at(clock_), db, clock_, uint64_t(std::max(0.0f, fadeSeconds) * float(rate_)), c };
	}

	float Mixer::wwBusDb(uint32_t bus) const { std::lock_guard<std::mutex> g(lock_); return wwBusDbLocked(bus); }

	float Mixer::wwBusDbLocked(uint32_t bus) const
	{
		float db = 0;
		for (int depth = 0; bus && depth < 16; depth++)
		{
			if (const auto it = wwBus_.find(bus); it != wwBus_.end()) { db += it->second.at(clock_); }
			const auto p = wwParent_.find(bus);
			bus = p == wwParent_.end() ? 0 : p->second;
		}
		return db;
	}

	// Makes src hold at least `need` frames (unless the stream ended), dropping what the interpolator no longer needs.
	void Mixer::fill(Voice& v, size_t need)
	{
		size_t keep = size_t(v.pos) - 1;
		if (keep > 0)
		{
			v.src.erase(v.src.begin(), v.src.begin() + std::ptrdiff_t(keep * size_t(v.ch)));
			v.frames -= keep;
			v.pos -= double(keep);
		}
		need -= std::min(need, keep);
		const WemInfo& in = v.dec.info();
		int emptyWraps = 0;
		while (v.frames < need && !v.eof)
		{
			uint32_t end = v.p.loop && in.loopEnd ? in.loopEnd : in.sampleCount;
			size_t chunk = std::min<size_t>(2048, end > v.dec.position() ? end - v.dec.position() : 0);
			size_t n = 0;
			if (chunk)
			{
				v.src.resize((v.frames + chunk) * size_t(v.ch));
				n = v.dec.read(v.src.data() + v.frames * size_t(v.ch), chunk);
				v.frames += n;
				v.src.resize(v.frames * size_t(v.ch));
			}
			if (n) { emptyWraps = 0; continue; }
			if (!v.p.loop || ++emptyWraps > 1) { v.eof = true; break; }
			v.dec.seek(in.loopEnd ? in.loopStart : 0);
		}
	}

	void Mixer::mixVoice(Voice& v, float* out, int frames, float bus)
	{
		int first = v.p.startAt > clock_ ? int(std::min<uint64_t>(uint64_t(frames), v.p.startAt - clock_)) : 0;
		if (first >= frames) return;
		double step = double(v.dec.info().sampleRate) / rate_ * double(std::clamp(v.p.rate, 0.0625f, 16.0f));
		// LPF amount -> cutoff, exponential from 20 kHz (0) to 50 Hz (100). UNSURE: Wwise's own table is not known.
		bool lpf = v.p.lpf > 0.5f;
		float a = lpf ? 1 - std::exp(-2 * float(M_PI) * 20000.0f * std::pow(0.0025f, std::min(v.p.lpf, 100.0f) / 100) / float(rate_)) : 1;
		fill(v, size_t(v.pos + step * (frames - first)) + 3);
		const float* s = v.src.data();
		int ch = v.ch;
		auto at = [&](size_t k, int c) { return k < v.frames ? s[k * size_t(ch) + size_t(c)] : 0.0f; };
		float pan = v.p.pan, gl, gr;
		if (ch == 1)
		{
			// Equal power, unity at centre.
			float th = (pan + 1) * 0.25f * float(M_PI);
			gl = std::cos(th) * float(M_SQRT2);
			gr = std::sin(th) * float(M_SQRT2);
		} else {
			gl = std::min(1.0f, 1 - pan);
			gr = std::min(1.0f, 1 + pan);
		}
		for (int i = first; i < frames; i++)
		{
			size_t i0 = size_t(v.pos);
			if (v.eof && i0 >= v.frames) { v.done = true; break; }
			uint64_t t = clock_ + uint64_t(i);
			if (v.stopping && t >= v.rampStart + v.rampLen) { v.done = true; break; }
			float x = float(v.pos - double(i0));
			float y[2];
			for (int c = 0; c < 2; c++)
			{
				int sc = std::min(c, ch - 1);
				float ym = at(i0 - 1, sc), y0 = at(i0, sc), y1 = at(i0 + 1, sc), y2 = at(i0 + 2, sc);
				float c1 = 0.5f * (y1 - ym), c2 = ym - 2.5f * y0 + 2 * y1 - 0.5f * y2, c3 = 0.5f * (y2 - ym) + 1.5f * (y0 - y1);
				y[c] = ((c3 * x + c2) * x + c1) * x + y0;
			}
			if (lpf)
			{
				for (int c = 0; c < 2; c++) y[c] = v.lp[c] += a * (y[c] - v.lp[c]);
			}
			float g = v.gainAt(t) * bus;
			out[i * 2] += y[0] * gl * g;
			out[i * 2 + 1] += y[1] * gr * g;
			v.pos += step;
		}
	}

	void Mixer::render(float* out, int frames)
	{
		std::fill(out, out + size_t(frames) * 2, 0.0f);
		std::lock_guard<std::mutex> g(lock_);
		for (auto& v : voices_)
		{
			// Wwise bus volume: once per render block (~20 ms), enough for the 1 s / 3 s pause ramps
			const float ww = v->p.wwBus && !wwBus_.empty() ? std::pow(10.0f, std::max(-96.0f, wwBusDbLocked(v->p.wwBus)) / 20) : 1.0f;
			if (!v->done) mixVoice(*v, out, frames, bus_[v->p.bus] * master_ * ww);
		}
		voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const std::unique_ptr<Voice>& v) { return v->done; }), voices_.end());
		clock_ += uint64_t(frames);
		for (int i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
	}
}
