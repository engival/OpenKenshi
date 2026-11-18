#include "audio/music.hpp"
#include "audio/wwbank.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

namespace audio
{
	using namespace hirc;

	uint32_t wwiseHash(const std::string& name)
	{
		uint32_t h = 2166136261u;
		for (char ch : name)
		{
			h *= 16777619u;
			h ^= uint8_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
		}
		return h;
	}

	// Persistent per playlist item: Step containers continue where the last play left off, and avoid-repeat looks back across plays.
	struct MusicPlayer::Walker
	{
		std::deque<size_t> history;
		size_t seqPos = 0;
	};

	struct MusicPlayer::Instance
	{
		uint32_t top = 0;	// the node the event played
		uint32_t child = 0;	// what it resolved to (playlist or segment; 0 = silence)
		struct Frame
		{
			const Item* item;
			int iterDone = 0, inIter = 0, perIter = 1, leafLeft = 1;
			std::vector<size_t> played;	// this iteration (shuffle)
		};
		std::vector<Frame> stack;
		bool started = false, ended = false;
		struct Seg { uint32_t id; uint64_t start, exit; std::vector<VoiceId> voices; };
		std::deque<Seg> segs;	// the playing one, then any scheduled ahead
		uint64_t firstAt = 0;	// start of the next fresh play (event or switch transition)
		float firstFade = 0;
		Curve firstCurve = kLinear;
	};

	MusicPlayer::MusicPlayer(Mixer& mixer, const Hierarchy& h, uint32_t seed) : mix_(mixer), h_(h), rng_(seed) {}
	MusicPlayer::~MusicPlayer() = default;

	uint64_t MusicPlayer::frames(double ms) const { return uint64_t(std::max(0.0, ms) * mix_.sampleRate() / 1000.0 + 0.5); }

	float MusicPlayer::gainDb(uint32_t node) const
	{
		float db = 0;
		for (int depth = 0; node && depth < 16; depth++)
		{
			const Node* n = h_.node(node);
			if (!n) break;
			db += n->volumeDb;
			node = n->parent;
		}
		return db;
	}

	uint32_t MusicPlayer::resolve(uint32_t node) const
	{
		for (int depth = 0; depth < 8; depth++)
		{
			auto it = h_.switches.find(node);
			if (it == h_.switches.end()) return node;
			const Switch& sw = it->second;
			const TreeNode* t = &sw.tree;
			for (size_t level = 0; level < sw.groups.size(); level++)
			{
				const auto& values = sw.groupIsState[level] ? states_ : switches_;
				auto v = values.find(sw.groups[level]);
				uint32_t want = v == values.end() ? 0 : v->second;
				const TreeNode* hit = nullptr;
				for (const TreeNode& c : t->children)
				{
					if (c.key == want) { hit = &c; break; }
				}
				if (!hit)
				{
					for (const TreeNode& c : t->children)
					{
						if (c.key == 0) { hit = &c; break; }	// the "*" fallback
					}
				}
				if (!hit) return 0;
				t = hit;
			}
			node = t->audio;
		}
		return 0;
	}

	uint32_t MusicPlayer::nextSegment(Instance& in)
	{
		if (!in.child) return 0;
		auto rs = h_.ranseqs.find(in.child);
		if (rs == h_.ranseqs.end())
		{
			if (in.started || !h_.segments.count(in.child)) return 0;
			in.started = true;
			return in.child;
		}
		if (!in.started)
		{
			in.started = true;
			in.stack.push_back({ &rs->second.root, 0, 0, 1, 1, {} });
			Instance::Frame& f = in.stack.back();
			f.perIter = f.item->type == kStepSequence || f.item->type == kStepRandom ? 1 : int(f.item->children.size());
		}
		for (int guard = 0; guard < 10000 && !in.stack.empty(); guard++)
		{
			Instance::Frame& f = in.stack.back();
			const Item& it = *f.item;
			if (it.children.empty())
			{
				if (it.type != kLeaf || !f.leafLeft) { in.stack.pop_back(); continue; }
				f.leafLeft--;
				return it.segment;
			}
			if (f.inIter >= f.perIter)
			{
				f.iterDone++;
				f.inIter = 0;
				f.played.clear();
				if (it.loop && f.iterDone >= it.loop) { in.stack.pop_back(); continue; }
			}
			size_t n = it.children.size(), pick = 0;
			Walker& w = *walkers_.try_emplace(&it, std::make_unique<Walker>()).first->second;
			if (it.type == kContinuousSequence) pick = size_t(f.inIter) % n;
			else if (it.type == kStepSequence) pick = w.seqPos++ % n;
			else
			{
				std::vector<size_t> cands;
				size_t avoid = std::min<size_t>(it.avoidRepeat, n - 1);
				for (size_t i = 0; i < n; i++)
				{
					if (it.shuffle && it.type == kContinuousRandom && std::find(f.played.begin(), f.played.end(), i) != f.played.end()) continue;
					if (std::find(w.history.end() - std::ptrdiff_t(std::min(avoid, w.history.size())), w.history.end(), i) != w.history.end()) continue;
					cands.push_back(i);
				}
				if (cands.empty())
				{
					for (size_t i = 0; i < n; i++) cands.push_back(i);
				}
				double total = 0;
				for (size_t i : cands) total += it.useWeight ? it.children[i].weight : 1.0;
				double r = std::uniform_real_distribution<double>(0, total)(rng_);
				pick = cands.back();
				for (size_t i : cands)
				{
					r -= it.useWeight ? it.children[i].weight : 1.0;
					if (r < 0) { pick = i; break; }
				}
				w.history.push_back(pick);
				if (w.history.size() > 32) w.history.pop_front();
			}
			f.inIter++;
			f.played.push_back(pick);
			const Item& c = it.children[pick];
			Instance::Frame nf { &c, 0, 0, 1, 1, {} };
			nf.perIter = c.type == kStepSequence || c.type == kStepRandom ? 1 : int(c.children.size());
			nf.leafLeft = c.loop ? c.loop : INT_MAX;
			in.stack.push_back(std::move(nf));
		}
		return 0;
	}

	void MusicPlayer::schedule(Instance& in, uint32_t segId, uint64_t start, float fadeS, Curve c)
	{
		const Segment& seg = h_.segments.at(segId);
		Instance::Seg s { segId, start, start + frames(seg.exitMs), {} };
		float db = gainDb(segId);
		for (uint32_t tid : seg.children)
		{
			auto t = h_.tracks.find(tid);
			if (t == h_.tracks.end()) continue;
			for (const Clip& clip : t->second.clips)
			{
				auto bank = h_.media.find(clip.sourceId);
				uint32_t size;
				const uint8_t* wem = bank == h_.media.end() ? nullptr : bank->second->mediaData(clip.sourceId, size);
				if (!wem) { fprintf(stderr, "music: media %u of segment %u is not loaded\n", clip.sourceId, segId); continue; }
				PlayParams p;
				p.bus = kMusic;
				p.gain = std::pow(10.0f, (db + t->second.volumeDb) / 20.0f);
				p.startAt = start + frames(clip.playAtMs + clip.beginTrimMs);
				p.fadeIn = fadeS;
				p.fadeCurve = c;
				if (VoiceId v = mix_.play(wem, size, p)) s.voices.push_back(v);
				else fprintf(stderr, "music: %s\n", mix_.error().c_str());
			}
		}
		history.push_back({ start, in.top, segId });
		in.segs.push_back(std::move(s));
	}

	void MusicPlayer::play(uint32_t node, float fadeS, Curve c, uint64_t at)
	{
		if (!h_.node(node)) return;	// an event shared with a sound bank (Create_or_Load_Game plays a Sound too): only its music actions are ours
		for (auto& in : instances_)
		{
			// UNSURE: Wwise may start a second instance; the game re-posts every 4-7 min and one Step pick is at most ~100 s, so overlap is rare.
			if (in->top == node && !in->ended) return;
		}
		auto in = std::make_unique<Instance>();
		in->top = node;
		in->child = resolve(node);
		in->firstAt = at;
		in->firstFade = fadeS;
		in->firstCurve = c;
		instances_.push_back(std::move(in));
	}

	void MusicPlayer::stop(uint32_t node, float fadeS, Curve c)
	{
		for (auto& in : instances_)
		{
			if (in->top != node) continue;
			for (auto& s : in->segs)
			{
				for (VoiceId v : s.voices) mix_.stop(v, fadeS, c);
			}
			in->segs.clear();
			in->ended = true;
		}
	}

	bool MusicPlayer::postEvent(uint32_t eventId)
	{
		auto ev = h_.events.find(eventId);
		if (ev == h_.events.end()) return false;
		uint64_t now = mix_.clock();
		for (uint32_t aid : ev->second.actions)
		{
			auto a = h_.actions.find(aid);
			if (a == h_.actions.end()) continue;
			const Action& act = a->second;
			float fade = float(act.fadeMs) / 1000.0f;
			switch (act.cls())
			{
				case 0x04: case 0x05: play(act.target, fade, act.curve, now + frames(act.delayMs)); break;
				case 0x01: stop(act.target, fade, act.curve); break;
				case 0x12: setState(act.group, act.value); break;
				case 0x19: setSwitch(act.group, act.value); break;
				default: break;	// non-music actions (sound objects) are not handled here
			}
		}
		update();
		return true;
	}

	void MusicPlayer::transition(Instance& in, uint32_t target)
	{
		const Switch& sw = h_.switches.at(in.top);
		const Rule* rule = nullptr;
		auto has = [](const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end() || std::find(v.begin(), v.end(), kAny) != v.end(); };
		for (const Rule& r : sw.rules)
		{
			if (has(r.src, in.child) && has(r.dst, target)) rule = &r;	// UNSURE: the last match wins (MUSIC.md section 7)
		}
		Rule fallback;
		if (!rule) rule = &fallback;
		uint64_t now = mix_.clock(), at = now;
		if (!in.segs.empty())
		{
			const Instance::Seg& cur = in.segs.front();
			const Segment& seg = h_.segments.at(cur.id);
			double beat = 60000.0 / std::max(1.0f, seg.meter.bpm), period = 0;
			if (rule->sync == kNextGrid) period = seg.meter.gridMs;
			else if (rule->sync == kNextBar) period = beat * seg.meter.beatsPerBar;
			else if (rule->sync == kNextBeat) period = beat;
			if (rule->sync == kImmediate) at = now;
			else if (period > 0)
			{
				uint64_t p = frames(period), base = cur.start + frames(seg.meter.gridOffsetMs);
				at = now <= base ? base : base + (now - base + p - 1) / p * p;
				at = std::min(at, cur.exit);
			}
			else at = std::max(now, cur.exit);
		}
		// UNSURE: fade offsets are ignored; both fades start on the sync point.
		float outS = float(rule->srcFade.ms) / 1000.0f;
		for (auto& s : in.segs)
		{
			for (VoiceId v : s.voices)
			{
				if (s.start >= at) mix_.stop(v);
				else mix_.stop(v, outS, rule->srcFade.curve, at);
			}
		}
		in.segs.clear();
		in.child = target;
		in.stack.clear();
		in.started = false;
		in.firstAt = at;
		in.firstFade = float(rule->dstFade.ms) / 1000.0f;
		in.firstCurve = rule->dstFade.curve;
	}

	void MusicPlayer::setState(uint32_t group, uint32_t value)
	{
		uint32_t& cur = states_[group];
		if (cur == value) return;
		cur = value;
		for (auto& in : instances_)
		{
			if (in->ended || !h_.switches.count(in->top)) continue;
			uint32_t target = resolve(in->top);
			if (target != in->child) transition(*in, target);
		}
		update();
	}

	void MusicPlayer::setSwitch(uint32_t group, uint32_t value)
	{
		uint32_t& cur = switches_[group];
		if (cur == value) return;
		cur = value;
		for (auto& in : instances_)
		{
			if (in->ended || !h_.switches.count(in->top)) continue;
			uint32_t target = resolve(in->top);
			if (target != in->child) transition(*in, target);
		}
		update();
	}

	void MusicPlayer::update()
	{
		uint64_t now = mix_.clock(), ahead = now + frames(500);
		for (auto& inp : instances_)
		{
			Instance& in = *inp;
			if (in.ended) continue;
			if (in.segs.empty())
			{
				if (in.firstAt > ahead) continue;
				uint32_t seg = nextSegment(in);
				if (!seg) { in.ended = true; continue; }
				schedule(in, seg, std::max(in.firstAt, now), in.firstFade, in.firstCurve);
			}
			while (in.segs.back().exit <= ahead)
			{
				uint32_t seg = nextSegment(in);
				if (!seg) { in.ended = true; break; }
				uint64_t entry = frames(h_.segments.at(seg).entryMs), exit = in.segs.back().exit;
				schedule(in, seg, exit > entry ? exit - entry : 0, 0, kLinear);
			}
			while (in.segs.size() > 1 && in.segs.front().exit <= now) in.segs.pop_front();
		}
		instances_.erase(std::remove_if(instances_.begin(), instances_.end(), [&](const std::unique_ptr<Instance>& in)
			{ return in->ended && (in->segs.empty() || in->segs.back().exit <= now); }), instances_.end());
	}

	std::string MusicPlayer::describe() const
	{
		std::string out;
		uint64_t now = mix_.clock();
		char line[160];
		for (const auto& in : instances_)
		{
			const Instance::Seg* cur = nullptr;
			for (const auto& s : in->segs)
			{
				if (s.start <= now) cur = &s;
			}
			double left = cur && cur->exit > now ? double(cur->exit - now) / mix_.sampleRate() : 0;
			snprintf(line, sizeof line, "node %u -> %u: segment %u (%s), %.1f s left%s\n", in->top, in->child, cur ? cur->id : 0,
				cur && !cur->voices.empty() ? "audible" : "silent", left, in->ended ? ", last" : "");
			out += line;
		}
		return out;
	}

	BankSet::BankSet(std::string dir) : dir_(std::move(dir)) {}
	BankSet::~BankSet() = default;

	bool BankSet::load(const std::string& name)
	{
		std::string file = name.size() > 4 && name.compare(name.size() - 4, 4, ".bnk") == 0 ? name : name + ".bnk";
		if (banks_.count(file)) return true;
		auto b = std::make_unique<Bank>();
		if (!b->load(dir_ + "/" + file)) { error_ = b->error(); return false; }
		if (!h_.add(*b)) { error_ = h_.error(); return false; }
		banks_.emplace(file, std::move(b));
		return true;
	}

	bool BankSet::loaded(const std::string& name) const
	{
		return banks_.count(name.size() > 4 && name.compare(name.size() - 4, 4, ".bnk") == 0 ? name : name + ".bnk") != 0;
	}

	static const char* const kBiomeNames[] = { "Dunes", "Savannah", "Swamplands", "Tarsands", "None" };

	GameMusic::GameMusic(BankSet& banks, Mixer& mixer, uint32_t seed) : banks_(banks), mix_(mixer), rng_(seed)
	{
		player_ = std::make_unique<MusicPlayer>(mixer, banks.hierarchy(), seed);
		player_->setState(wwiseHash("Time_in_Game"), wwiseHash("Day"));
	}
	GameMusic::~GameMusic() = default;

	double GameMusic::now() const { return double(mix_.clock()) / mix_.sampleRate(); }

	void GameMusic::setBiome(Biome b)
	{
		if (b == biome_) return;
		biome_ = b;
		if (b == kNone) return;
		std::string name = kBiomeNames[b];
		if (!banks_.load(name + "_Music_Bank")) { fprintf(stderr, "music: %s\n", banks_.error().c_str()); return; }
		player_->setSwitch(wwiseHash("Biomes"), wwiseHash(name));	// no post here: the exe's zone change (1402587a0) only switches; the vf4 timer (update) posts
	}

	void GameMusic::setDay(bool day)
	{
		day_ = day;
		player_->setState(wwiseHash("Time_in_Game"), wwiseHash(day ? "Day" : "Night"));
	}

	void GameMusic::setFrequencyMinutes(float minutes)
	{
		freqMin_ = std::max(0.1f, minutes);
		next_ = std::min(next_, now() + freqMin_ * 60.0);
	}

	void GameMusic::setVolume(float v)
	{
		volume_ = v;
		mix_.setBusGain(kMusic, v);
	}

	void GameMusic::trigger()
	{
		if (volume_ > 0 && biome_ != kNone) player_->postEvent(wwiseHash(std::string("Music_") + kBiomeNames[biome_] + "_Play"));
		next_ = now() + std::uniform_real_distribution<double>(0.8, 1.4)(rng_) * freqMin_ * 60.0;
	}

	void GameMusic::update()
	{
		if (biome_ != kNone && now() >= next_) trigger();
		player_->update();
	}
}
