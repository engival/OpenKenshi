#include "audio/sound.hpp"
#include "audio/music.hpp"
#include "audio/wwbank.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

namespace audio
{
	using namespace hirc;

	namespace
	{
		constexpr uint64_t kNever = UINT64_MAX;
		constexpr double kLookAheadMs = 500;
		constexpr float kSmoothS = 0.05f;	// gain / parameter changes ramp this long, against zipper noise
		enum Kind : uint8_t { kSound, kContainer, kSwitch, kLayer };
	}

	// A value ramp on the mixer clock (action offsets, play fades, crossfades).
	struct SoundPlayer::Ramp
	{
		float from = 0, to = 0;
		uint64_t start = 0, len = 0;
		Curve curve = kLinear;

		float at(uint64_t t) const
		{
			if (len == 0 || t >= start + len) return t >= start ? to : from;
			if (t <= start) return from;
			return from + (to - from) * curveShape(curve, float(t - start) / float(len));
		}
		void go(float target, uint64_t now, uint64_t frames, Curve c)
		{
			from = at(now);
			to = target;
			start = now;
			len = frames;
			curve = c;
		}
	};

	struct SoundPlayer::Mods { Ramp db, cents, lpf; };

	// Playlist position and recent picks of one container.
	struct SoundPlayer::Walker
	{
		std::deque<uint32_t> recent;
		std::vector<uint32_t> bag;	// shuffle: what is left of this round
		size_t seq = 0;
		int dir = 1;
	};

	struct SoundPlayer::Run
	{
		uint32_t node = 0;
		Kind kind = kSound;
		Run* parent = nullptr;
		std::vector<std::unique_ptr<Run>> kids;
		uint64_t start = 0, end = kNever;	// sounds: the span they play; containers: kNever
		Ramp xf { 1, 1 };	// crossfade / switch fade factor (linear)
		bool finished = false;	// starts no more children
		bool stopping = false;
		// Sound
		VoiceId voice = 0;
		float pan = 0;
		Eval rolled;	// ranged props along the chain, rolled when the voice starts
		// Container
		int64_t left = 0;	// children still to start, -1 infinite
		uint64_t nextAt = kNever;
		std::unique_ptr<Walker> local;	// when the container resets at each play
		// Switch
		uint32_t value = 0;
	};

	struct SoundPlayer::Instance
	{
		std::unique_ptr<Run> root;
		Bus bus = kSfx;
		Ramp fade { 1, 1 };
		bool stopping = false;
		uint64_t stopEnd = 0;
		uint32_t object = 0;	// the world object it was posted on (0: none)
		double pos[3] = {};	// where that object last was: the sound follows it, and stays there when it is removed
	};

	SoundPlayer::SoundPlayer(Mixer& mixer, const Hierarchy& h, uint32_t seed) : mix_(mixer), h_(h), rng_(seed) {}
	SoundPlayer::~SoundPlayer()
	{
		for (auto& in : instances_) stopRun(*in->root, 0, kLinear);
	}

	uint64_t SoundPlayer::frames(double ms) const { return uint64_t(std::max(0.0, ms) * mix_.sampleRate() / 1000.0); }
	float SoundPlayer::rtpc(uint32_t id) const { auto it = rtpcs_.find(id); return it == rtpcs_.end() ? 0 : it->second; }
	void SoundPlayer::setRtpc(uint32_t id, float value) { rtpcs_[id] = value; }
	void SoundPlayer::setState(uint32_t group, uint32_t value) { states_[group] = value; }
	void SoundPlayer::setSwitch(uint32_t group, uint32_t value, uint32_t object)
	{
		if (object) { if (auto e = emitters_.find(object); e != emitters_.end()) e->second.switches[group] = value; }
		else switches_[group] = value;
	}
	void SoundPlayer::setObject(uint32_t object, const double pos[3])
	{
		Emitter& e = emitters_[object];
		for (int k = 0; k < 3; k++) e.pos[k] = pos[k];
	}
	float SoundPlayer::objectDistance(uint32_t object) const
	{
		auto e = emitters_.find(object);
		if (e == emitters_.end()) return -1;
		double d2 = 0;
		for (int k = 0; k < 3; k++) d2 += (e->second.pos[k] - listenerPos_[k]) * (e->second.pos[k] - listenerPos_[k]);
		return float(std::sqrt(d2));
	}
	void SoundPlayer::removeObject(uint32_t object) { emitters_.erase(object); }
	void SoundPlayer::setListener(const double pos[3], const double fwd[3], const double right[3])
	{
		for (int k = 0; k < 3; k++) { listenerPos_[k] = pos[k]; listenerFwd_[k] = fwd[k]; listenerRight_[k] = right[k]; }
	}

	uint32_t SoundPlayer::syncValue(uint8_t groupType, uint32_t group, uint32_t def, uint32_t object) const
	{
		if (groupType != 1 && object)	// a switch set on the object wins over the global one
		{
			if (auto e = emitters_.find(object); e != emitters_.end())
			{
				if (auto it = e->second.switches.find(group); it != e->second.switches.end()) return it->second;
			}
		}
		const auto& m = groupType == 1 ? states_ : switches_;
		auto it = m.find(group);
		return it == m.end() ? def : it->second;
	}

	// Distance, side and attenuation of a voice on a world object: the node's positioning (nearest override up its chain) must be 3D; the
	// attenuation object gives the volume (dB) and low-pass amount at the distance listener -> object (Wwise: the same distance in game units;
	// the curves end at the gate's distance), and the pan is the sine of the object's azimuth against the listener's right vector (a stereo
	// pan law, UNSURE: Wwise pans 2-channel output from speaker angles).
	SoundPlayer::Spatial SoundPlayer::spatial(const Run& r, const Instance& in) const
	{
		Spatial s;
		if (!in.object) return s;
		const Hierarchy::Positioning po = h_.positioning(r.node);
		if (!po.is3d) return s;
		s.is3d = true;
		double d[3], dist2 = 0, side = 0;
		for (int k = 0; k < 3; k++) { d[k] = in.pos[k] - listenerPos_[k]; dist2 += d[k] * d[k]; side += d[k] * listenerRight_[k]; }
		s.distance = float(std::sqrt(dist2));
		s.pan = s.distance > 1.0f ? float(std::clamp(side / std::sqrt(dist2), -1.0, 1.0)) : 0.0f;
		if (auto a = h_.attenuations.find(po.attenuation); a != h_.attenuations.end())
		{
			s.db = a->second.volumeDb(s.distance);
			s.lpf = a->second.lpf(s.distance);
		}
		return s;
	}

	bool SoundPlayer::postEvent(uint32_t eventId, Bus bus, uint32_t object, float gate)
	{
		auto ev = h_.events.find(eventId);
		if (ev == h_.events.end()) return false;
		if (object)
		{
			auto em = emitters_.find(object);
			if (em == emitters_.end()) return false;
			if (gate > 0)
			{
				double d2 = 0;
				for (int k = 0; k < 3; k++) d2 += (em->second.pos[k] - listenerPos_[k]) * (em->second.pos[k] - listenerPos_[k]);
				if (d2 > double(gate) * gate) return false;
			}
		}
		uint64_t now = mix_.clock();
		for (uint32_t aid : ev->second.actions)
		{
			auto it = h_.actions.find(aid);
			if (it == h_.actions.end()) continue;
			const Action& a = it->second;
			switch (a.cls())
			{
				case 0x04: play(a.target, bus, float(a.fadeMs) / 1000, a.curve, now + frames(a.delayMs), object); break;
				case 0x01: stop(a.target, float(a.fadeMs) / 1000, a.curve); break;
				case 0x12: setState(a.group, a.value); break;
				case 0x19: setSwitch(a.group, a.value, object); break;
				case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0E: case 0x0F: setMod(a, now); break;
				// Set / Reset Bus Volume (Pause_Game: SFX Bus -96 dB and the ambience bus -9 dB over 1 s; Resume_Game resets both over 3 s).
				// Absolute (meaning 1) only, every one in the banks; an offset is logged and skipped.
				case 0x0C:
					if (a.propMeaning == 1) { mix_.setWwBusVolume(a.target, a.propBase, float(a.fadeMs) / 1000, a.curve); }
					else { fprintf(stderr, "sound: bus %u volume meaning %u not applied\n", a.target, a.propMeaning); }
					break;
				case 0x0D: mix_.setWwBusVolume(a.target, 0, float(a.fadeMs) / 1000, a.curve); break;
				default: break;	// bypass FX, pause / resume, mute, ...: not used for what we play
			}
		}
		return true;
	}

	// Set (even class) / Reset (odd class) Pitch 0x08, Volume 0x0A, LPF 0x0E on a node: an offset that every voice below it picks up.
	void SoundPlayer::setMod(const Action& a, uint64_t now)
	{
		auto& m = mods_[a.target];
		if (!m) m = std::make_unique<Mods>();
		uint8_t cls = a.cls();
		bool reset = cls & 1;
		float v = reset ? 0 : a.propBase + std::uniform_real_distribution<float>(std::min(a.propMin, a.propMax), std::max(a.propMin, a.propMax))(rng_);
		Ramp& r = cls <= 0x09 ? m->cents : cls <= 0x0B ? m->db : m->lpf;
		r.go(v, now, frames(a.fadeMs), a.curve);
	}

	void SoundPlayer::play(uint32_t node, Bus bus, float fadeS, Curve c, uint64_t at, uint32_t object)
	{
		if (!h_.object(node) || h_.mixers.count(node)) return;
		auto in = std::make_unique<Instance>();
		in->bus = bus;
		in->object = object;
		if (auto e = emitters_.find(object); e != emitters_.end()) { for (int k = 0; k < 3; k++) in->pos[k] = e->second.pos[k]; }
		if (fadeS > 0)
		{
			in->fade = Ramp { 0, 0 };
			in->fade.go(1, at, uint64_t(fadeS * float(mix_.sampleRate())), c);
		}
		in->root = start(node, nullptr, *in, at);
		if (in->root) instances_.push_back(std::move(in));
	}

	void SoundPlayer::stopRun(Run& r, float fadeS, Curve c)
	{
		r.stopping = r.finished = true;
		if (r.voice) mix_.stop(r.voice, fadeS, c);
		for (auto& k : r.kids) stopRun(*k, fadeS, c);
	}

	// Stops every instance whose played node is the target or lies below it (Leave_<zone> stops the zone's actor-mixer), and runs of the
	// target inside other instances.
	void SoundPlayer::stop(uint32_t target, float fadeS, Curve c)
	{
		uint64_t now = mix_.clock();
		for (auto& in : instances_)
		{
			if (in->stopping) continue;
			bool hit = false;
			for (uint32_t id = in->root->node; id && !hit; )
			{
				hit = id == target;
				const Object* o = h_.object(id);
				id = o ? o->p.parent : 0;
			}
			if (hit)
			{
				in->stopping = true;
				in->stopEnd = now + uint64_t(fadeS * float(mix_.sampleRate()));
				stopRun(*in->root, fadeS, c);
				continue;
			}
			std::vector<Run*> todo { in->root.get() };
			while (!todo.empty())
			{
				Run* r = todo.back();
				todo.pop_back();
				if (r->node == target && !r->stopping) { stopRun(*r, fadeS, c); continue; }
				for (auto& k : r->kids) todo.push_back(k.get());
			}
		}
	}

	std::unique_ptr<SoundPlayer::Run> SoundPlayer::start(uint32_t node, Run* parent, Instance& in, uint64_t at)
	{
		auto r = std::make_unique<Run>();
		r->node = node;
		r->parent = parent;
		r->start = at;
		if (auto it = h_.sounds.find(node); it != h_.sounds.end())
		{
			const Sound& s = it->second;
			r->kind = kSound;
			r->finished = true;
			int rate = mix_.sampleRate();
			if (s.plugin == kPluginSilence)
			{
				float d = s.silenceS + std::uniform_real_distribution<float>(-s.silenceMinusS, s.silencePlusS)(rng_);
				r->end = at + uint64_t(std::max(0.0f, d) * float(rate));
				return r;
			}
			auto m = h_.media.find(s.source);
			uint32_t size = 0;
			const uint8_t* wem = m == h_.media.end() ? nullptr : m->second->mediaData(s.source, size);
			if (!wem) { r->end = at; return r; }
			// Ranged props (volume, pitch, LPF) of the sound and everything above it, rolled for this voice.
			for (uint32_t id = node; id; )
			{
				const Object* o = h_.object(id);
				if (!o) break;
				for (const RangedProp& q : o->p.ranged)
				{
					float v = std::uniform_real_distribution<float>(std::min(q.min, q.max), std::max(q.min, q.max))(rng_);
					if (q.id == kPropVolume) r->rolled.db += v;
					else if (q.id == kPropPitch) r->rolled.cents += v;
					else if (q.id == kPropLpf) r->rolled.lpf += v;
				}
				id = o->p.parent;
			}
			// 3D sounds have no emitter position here; one-shots picked by a container get a random pan instead (interpretation, AUDIO_ENGINE.md).
			bool is3d = false;
			for (uint32_t id = node; id; )
			{
				const Object* o = h_.object(id);
				if (!o) break;
				if (o->p.posOverride) { is3d = o->p.is3d; break; }
				id = o->p.parent;
			}
			const Spatial sp = spatial(*r, in);
			if (sp.is3d) r->pan = sp.pan;	// on a world object: the side it stands on
			else if (is3d && parent && parent->kind == kContainer) r->pan = std::uniform_real_distribution<float>(-0.7f, 0.7f)(rng_);
			Eval e = eval(*r, in, at);
			int32_t loops = s.p.i(kPropLoop, 1);
			PlayParams pp;
			pp.gain = std::pow(10.0f, e.db / 20) * e.lin;
			pp.pan = r->pan;
			pp.rate = std::pow(2.0f, e.cents / 1200);
			pp.lpf = e.lpf;
			pp.bus = in.bus;
			for (uint32_t id = node; id && !pp.wwBus; )	// the output bus: the nearest node up the chain that routes to one
			{
				const Object* o = h_.object(id);
				if (!o) break;
				pp.wwBus = o->p.bus;
				id = o->p.parent;
			}
			pp.loop = loops != 1;
			pp.startAt = at;
			WemInfo info;
			r->voice = mix_.play(wem, size, pp, &info);
			if (!r->voice) { fprintf(stderr, "sound %u: %s\n", node, mix_.error().c_str()); r->end = at; return r; }
			uint64_t once = uint64_t(double(info.sampleCount) / info.sampleRate * rate / pp.rate);
			if (loops == 0) r->end = kNever;
			else
			{
				r->end = at + once * uint64_t(std::max(1, loops));
				if (loops > 1) mix_.stop(r->voice, 0, kLinear, r->end);
			}
			history.push_back({ at, node, r->voice, e.db, pp.pan, pp.rate, in.object, sp.distance });
			return r;
		}
		if (auto it = h_.containers.find(node); it != h_.containers.end())
		{
			const Container& c = it->second;
			r->kind = kContainer;
			if (c.playlist.empty()) { r->finished = true; return r; }
			int64_t loops = c.loopCount;
			if (c.loopModMax > 0) loops += std::uniform_int_distribution<int>(-int(c.loopModMin), int(c.loopModMax))(rng_);
			r->left = !c.continuous ? 1 : c.loopCount == 0 ? -1 : std::max<int64_t>(1, loops) * int64_t(c.playlist.size());
			if (c.resetEachPlay) r->local = std::make_unique<Walker>();
			startNext(*r, in, at);
			return r;
		}
		if (auto it = h_.switchCntrs.find(node); it != h_.switchCntrs.end())
		{
			const SwitchCntr& s = it->second;
			r->kind = kSwitch;
			r->finished = true;
			r->value = syncValue(s.groupType, s.group, s.defaultValue, in.object);
			for (const SwitchCntr::Package& p : s.packages)
			{
				if (p.value != r->value) continue;
				for (uint32_t k : p.nodes)
				{
					if (auto kid = start(k, r.get(), in, at)) r->kids.push_back(std::move(kid));
				}
			}
			return r;
		}
		if (auto it = h_.layerCntrs.find(node); it != h_.layerCntrs.end())
		{
			r->kind = kLayer;
			r->finished = true;
			for (uint32_t k : it->second.children)
			{
				if (auto kid = start(k, r.get(), in, at)) r->kids.push_back(std::move(kid));
			}
			return r;
		}
		return nullptr;
	}

	uint32_t SoundPlayer::pick(const Container& c, Walker& w)
	{
		size_t n = c.playlist.size();
		if (c.mode == Container::kSequence)
		{
			if (w.seq >= n) w.seq = 0;
			uint32_t id = c.playlist[w.seq].id;
			if (!c.restartBackward) w.seq = (w.seq + 1) % n;
			else
			{
				if (n > 1 && ((w.dir > 0 && w.seq + 1 >= n) || (w.dir < 0 && w.seq == 0))) w.dir = -w.dir;
				w.seq = n > 1 ? size_t(int64_t(w.seq) + w.dir) : 0;
			}
			return id;
		}
		size_t avoid = std::min<size_t>(c.avoidRepeat, n - 1);
		auto recent = [&](uint32_t id) { return std::find(w.recent.begin(), w.recent.end(), id) != w.recent.end(); };
		uint32_t id = c.playlist[0].id;
		if (c.randomMode == 1)	// shuffle: each item once per round
		{
			if (w.bag.empty()) for (const Container::Entry& e : c.playlist) w.bag.push_back(e.id);
			std::vector<size_t> ok;
			for (size_t k = 0; k < w.bag.size(); k++) if (!recent(w.bag[k])) ok.push_back(k);
			size_t k = ok.empty() ? std::uniform_int_distribution<size_t>(0, w.bag.size() - 1)(rng_) : ok[std::uniform_int_distribution<size_t>(0, ok.size() - 1)(rng_)];
			id = w.bag[k];
			w.bag.erase(w.bag.begin() + std::ptrdiff_t(k));
		} else {
			double total = 0;
			for (const Container::Entry& e : c.playlist) if (!recent(e.id)) total += std::max(0, e.weight);
			double x = std::uniform_real_distribution<double>(0, total)(rng_);
			for (const Container::Entry& e : c.playlist)
			{
				if (recent(e.id) || e.weight <= 0) continue;
				id = e.id;
				if ((x -= e.weight) < 0) break;
			}
		}
		if (avoid)
		{
			w.recent.push_back(id);
			while (w.recent.size() > avoid) w.recent.pop_front();
		}
		return id;
	}

	// Starts the container's next child at `at` and works out when the one after it is due (its transition).
	void SoundPlayer::startNext(Run& r, Instance& in, uint64_t at)
	{
		const Container& c = h_.containers.at(r.node);
		if (r.left == 0) { r.finished = true; return; }
		auto& w = r.local ? r.local : walkers_[r.node];
		if (!w) w = std::make_unique<Walker>();
		std::unique_ptr<Run> kid = start(pick(c, *w), &r, in, at);
		if (r.left > 0) r.left--;
		if (!kid) { r.nextAt = at; return; }
		uint64_t t = frames(c.transitionMs + std::uniform_real_distribution<float>(-c.transitionModMinMs, c.transitionModMaxMs)(rng_));
		bool xfade = c.transition == Container::kCrossfadeAmp || c.transition == Container::kCrossfadePower;
		if (xfade && !r.kids.empty())
		{
			kid->xf = Ramp { 0, 0 };
			kid->xf.go(1, at, t, c.transition == Container::kCrossfadePower ? kSine : kLinear);
			Run& prev = *r.kids.back();
			if (prev.voice) mix_.stop(prev.voice, float(t) / float(mix_.sampleRate()), c.transition == Container::kCrossfadePower ? kSineRecip : kLinear, at);
		}
		if (kid->end == kNever) r.nextAt = c.transition == Container::kTriggerRate && kid->kind == kSound ? at + t : kNever;
		else if (c.transition == Container::kDelay) r.nextAt = kid->end + t;
		else if (xfade) r.nextAt = kid->end > at + t ? kid->end - t : at;
		else if (c.transition == Container::kTriggerRate) r.nextAt = at + t;
		else r.nextAt = kid->end;
		r.kids.push_back(std::move(kid));
		if (r.left == 0) r.finished = true;
	}

	void SoundPlayer::tick(Run& r, Instance& in, uint64_t now)
	{
		for (auto& k : r.kids) tick(*k, in, now);
		auto done = [&](const Run& k) { return k.finished && k.kids.empty() && (k.kind != kSound || now >= k.end || (k.voice && !mix_.playing(k.voice) && now > k.start)); };
		r.kids.erase(std::remove_if(r.kids.begin(), r.kids.end(), [&](const std::unique_ptr<Run>& k) { return done(*k); }), r.kids.end());
		if (r.stopping) return;
		if (r.kind == kContainer && !r.finished)
		{
			if (r.nextAt == kNever && r.kids.empty()) r.nextAt = now;	// a nested container ended
			if (r.nextAt != kNever && r.nextAt <= now + frames(kLookAheadMs)) startNext(r, in, std::max(r.nextAt, now));
		}
		if (r.kind == kSwitch)
		{
			const SwitchCntr& s = h_.switchCntrs.at(r.node);
			uint32_t v = syncValue(s.groupType, s.group, s.defaultValue, in.object);
			if (s.continuousValidation && v != r.value)
			{
				r.value = v;
				const SwitchCntr::Package* pkg = nullptr;
				for (const SwitchCntr::Package& p : s.packages) if (p.value == v) pkg = &p;
				auto params = [&](uint32_t node) -> const SwitchCntr::NodeParams* { for (auto& np : s.nodeParams) if (np.node == node) return &np; return nullptr; };
				std::vector<uint32_t> keep;
				for (auto& k : r.kids)
				{
					const SwitchCntr::NodeParams* np = params(k->node);
					bool stays = pkg && np && np->continuePlayback && std::find(pkg->nodes.begin(), pkg->nodes.end(), k->node) != pkg->nodes.end();
					if (stays) { keep.push_back(k->node); continue; }
					if (!np || np->onSwitch == 1) stopRun(*k, np ? float(np->fadeOutMs) / 1000 : 0, kLinear);
					else k->finished = true;	// play to end
				}
				for (uint32_t node : pkg ? pkg->nodes : std::vector<uint32_t> {})
				{
					if (std::find(keep.begin(), keep.end(), node) != keep.end()) continue;
					auto kid = start(node, &r, in, now);
					if (!kid) continue;
					if (const SwitchCntr::NodeParams* np = params(node); np && np->fadeInMs > 0)
					{
						kid->xf = Ramp { 0, 0 };
						kid->xf.go(1, now, frames(np->fadeInMs), kLinear);
					}
					r.kids.push_back(std::move(kid));
				}
			}
		}
		if (r.voice && now >= r.start)
		{
			Eval e = eval(r, in, now);
			mix_.setGain(r.voice, std::pow(10.0f, e.db / 20) * e.lin, kSmoothS);
			mix_.setRate(r.voice, std::pow(2.0f, e.cents / 1200));
			mix_.setLpf(r.voice, e.lpf);
		}
	}

	void SoundPlayer::addParams(const Params& p, uint32_t id, Eval& e, uint64_t t) const
	{
		e.db += p.f(kPropVolume) + p.f(kPropMakeUpGain);
		e.cents += p.f(kPropPitch);
		e.lpf += p.f(kPropLpf);
		for (const Rtpc& c : p.rtpcs)
		{
			float y = c.eval(rtpc(c.id));
			if (c.param == kParamVolume || c.param == kParamMakeUpGain || c.param == kParamBusVolume) e.db += y;
			else if (c.param == kParamPitch) e.cents += y;
			else if (c.param == kParamLpf) e.lpf += y;
		}
		if (auto it = mods_.find(id); it != mods_.end())
		{
			e.db += it->second->db.at(t);
			e.cents += it->second->cents.at(t);
			e.lpf += it->second->lpf.at(t);
		}
	}

	// Volume (dB), pitch (cents) and LPF of a voice at clock t: its node chain up the hierarchy, the layers that associate it, the rolled
	// ranges, and the linear factors (play fade, crossfades, layer crossfade curves).
	SoundPlayer::Eval SoundPlayer::eval(const Run& r, const Instance& in, uint64_t t) const
	{
		Eval e = r.rolled;
		for (uint32_t id = r.node; id; )
		{
			const Object* o = h_.object(id);
			if (!o) break;
			addParams(o->p, id, e, t);
			id = o->p.parent;
		}
		for (const Run* a = &r; a; a = a->parent)
		{
			e.lin *= a->xf.at(t);
			if (!a->parent || a->parent->kind != kLayer) continue;
			for (const Layer& y : h_.layerCntrs.at(a->parent->node).layers)
			{
				for (const Layer::Assoc& as : y.assocs)
				{
					if (as.child != a->node) continue;
					Params lp;
					lp.rtpcs = y.rtpcs;
					addParams(lp, 0, e, t);
					// Crossfade curve over the layer's parameter, read as a linear gain (UNSURE, AUDIO_ENGINE.md).
					if (y.crossfadeRtpc && !as.curve.empty()) e.lin *= std::max(0.0f, evalPoints(as.curve, rtpc(y.crossfadeRtpc)));
				}
			}
		}
		e.lin *= in.fade.at(t);
		if (in.object)
		{
			const Spatial sp = spatial(r, in);
			e.db += sp.db;
			e.lpf += sp.lpf;
		}
		e.lpf = std::clamp(e.lpf, 0.0f, 100.0f);
		return e;
	}

	void SoundPlayer::update()
	{
		uint64_t now = mix_.clock();
		for (auto& in : instances_)
		{
			if (in->object) { if (auto e = emitters_.find(in->object); e != emitters_.end()) { for (int k = 0; k < 3; k++) in->pos[k] = e->second.pos[k]; } }	// follow the object
			tick(*in->root, *in, now);
		}
		instances_.erase(std::remove_if(instances_.begin(), instances_.end(), [&](const std::unique_ptr<Instance>& in)
		{
			const Run& r = *in->root;
			if (in->stopping) return now >= in->stopEnd;
			return r.finished && r.kids.empty() && (r.kind != kSound || now >= r.end);
		}), instances_.end());
	}

	std::string SoundPlayer::describe() const
	{
		std::string out;
		for (const auto& in : instances_)
		{
			size_t voices = 0;
			std::vector<const Run*> todo { in->root.get() };
			while (!todo.empty())
			{
				const Run* r = todo.back();
				todo.pop_back();
				if (r->voice) voices++;
				for (const auto& k : r->kids) todo.push_back(k.get());
			}
			static const char* const kinds[] = { "sound", "ranseq", "switch", "layer" };
			char line[160];
			snprintf(line, sizeof line, "  %s %u: %zu voices%s\n", kinds[in->root->kind], in->root->node, voices, in->stopping ? ", stopping" : "");
			out += line;
		}
		return out;
	}

	static const char* const kZoneNames[] = { "Ashlands", "Boneyard", "Canyons", "Coast", "Desert", "Forest", "Jungle", "Plains", "Swamplands", "Tarsands" };

	const char* GameAmbience::zoneName(Zone z) { return z < kNoZone ? kZoneNames[z] : "None"; }
	GameAmbience::Zone GameAmbience::zoneByName(const std::string& name)
	{
		for (int z = 0; z < kNoZone; z++) if (name == kZoneNames[z]) return Zone(z);
		return kNoZone;
	}

	GameAmbience::GameAmbience(BankSet& banks, Mixer& mixer, uint32_t seed) : banks_(banks), mix_(mixer)
	{
		player_ = std::make_unique<SoundPlayer>(mixer, banks.hierarchy(), seed);
		for (const char* b : { "SFX_Amb_Weather_Bank", "SFX_Amb_Indoors_Bank" })
		{
			if (!banks_.load(b)) fprintf(stderr, "ambience: %s\n", banks_.error().c_str());
		}
	}
	GameAmbience::~GameAmbience() = default;

	bool GameAmbience::post(const std::string& event) { return player_->postEvent(wwiseHash(event), kAmbience); }

	// FUN_140258940: the zone (with day/night in bit 8) changed: Leave_<old zone> when the zone itself differs, then Enter_<zone>_Day/_Night once
	// its bank is in. A day/night flip re-enters the same zone; the new event's Stop actions fade the other half out.
	void GameAmbience::enter()
	{
		if (zone_ == entered_ && day_ == enteredDay_) return;
		if (entered_ != kNoZone && zone_ != entered_) post(std::string("Leave_") + kZoneNames[entered_]);
		entered_ = zone_;
		enteredDay_ = day_;
		if (zone_ == kNoZone) return;
		if (!banks_.load(std::string("SFX_Amb_") + kZoneNames[zone_])) { fprintf(stderr, "ambience: %s\n", banks_.error().c_str()); return; }
		post(std::string("Enter_") + kZoneNames[zone_] + (day_ ? "_Day" : "_Night"));
	}

	void GameAmbience::setZone(Zone z) { zone_ = z; enter(); }
	void GameAmbience::setDay(bool day) { day_ = day; enter(); }

	void GameAmbience::setIndoors(bool inside, bool big, const std::string& material, float integrity)
	{
		if (inside == inside_) return;
		inside_ = inside;
		if (inside)
		{
			if (!material.empty()) { player_->setSwitch(wwiseHash("Indoors"), wwiseHash(material)); }
			if (integrity >= 0) { player_->setRtpc(wwiseHash("Building_Integrity"), integrity); }
		}
		post(inside ? (big ? "Enter_Building_Big" : "Enter_Building") : "Leave_Building");
	}

	void GameAmbience::setCameraHeight(float h) { player_->setRtpc(wwiseHash("Camera_Zoom"), h / 2000 * 100); }

	// FUN_1409e8f70: Rain_Start when rain begins, Rain_End when it stops, Rain_Intensity while it changes; Wind_Start when the wind rises from 0,
	// Wind_Stop when it drops to 0. The bank's event is Weather_Wind_End, so the game's Stop finds nothing; the Intensity curves silence the wind.
	void GameAmbience::setRain(float v)
	{
		if (v <= 0 && rain_ > 0) post("Weather_Rain_End");
		if (v > 0 && rain_ == 0) post("Weather_Rain_Start");
		if (v > 0 && v != rain_) player_->setRtpc(wwiseHash("Rain_Intensity"), v);
		rain_ = v;
	}

	void GameAmbience::setWind(float v)
	{
		if (v == wind_) return;
		player_->setRtpc(wwiseHash("Intensity"), v);
		if (wind_ > 0 && v <= 0) post("Weather_Wind_Stop");
		else if (wind_ <= 0) post("Weather_Wind_Start");
		wind_ = v;
	}

	void GameAmbience::setVolume(float v) { mix_.setBusGain(kAmbience, v); }
	void GameAmbience::update() { player_->update(); }
}
