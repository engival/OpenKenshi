// Audio test CLI: lists and decodes the media embedded in Wwise banks (src/audio/), no graphics, no playback device.
//	kaudio_dump list BANK...                 media id, bytes, channels, rate, samples, loop
//	kaudio_dump check BANK...                decode every media file; frames vs. header sample count, peak, errors
//	kaudio_dump wav BANK MEDIA_ID OUT.wav    decode one media file to 16-bit PCM (for listening tests only; nothing ships converted)
//	kaudio_dump hirc BANK...                 decode the HIRC objects of all banks into one hierarchy; counts and unresolved refs
//	kaudio_dump tree NAMES BANK...                 event -> action -> sound-structure tree; ids labelled from a names file
//	kaudio_dump music AUDIO_DIR BIOME OUT.wav|- MINUTES [night=S] [freq=MIN] [seed=N]   the game's biome music, offline, with the segment log
//	kaudio_dump amb AUDIO_DIR ZONE OUT.wav|- SECONDS [T:key[=value]]... [seed=N]   the game's zone ambience, offline, with a timeline of changes
//	kaudio_dump mix BANK OUT.wav SECONDS ID[@PAN]...   render looping voices through the mixer at 48 kHz, offline
//	kaudio_dump play BANK SECONDS ID[@PAN]...          the same on the default output device
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "audio/sound.hpp"
#include "audio/wwbank.hpp"
#include "audio/wwhirc.hpp"
#include "audio/wwvorbis.hpp"
#include <algorithm>
#include <chrono>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int usage()
{
	fprintf(stderr, "usage: kaudio_dump list BANK... | check BANK... | wav BANK MEDIA_ID OUT.wav | mix BANK OUT.wav SECONDS ID[@PAN]... | play BANK SECONDS ID[@PAN]...\n");
	return 2;
}

static bool loadBank(audio::Bank& bank, const char* path)
{
	if (bank.load(path)) return true;
	fprintf(stderr, "%s\n", bank.error().c_str());
	return false;
}

static int list(int argc, char** argv)
{
	for (int a = 0; a < argc; a++)
	{
		audio::Bank bank;
		if (!loadBank(bank, argv[a])) return 1;
		printf("%s: version %u id %u, %zu media\n", argv[a], bank.version, bank.id, bank.media.size());
		for (const audio::Media& m : bank.media)
		{
			audio::WemDecoder dec;
			if (!dec.open(bank.mediaData(m), m.size)) { printf("  %10u %9u  ERROR %s\n", m.id, m.size, dec.error().c_str()); continue; }
			const audio::WemInfo& in = dec.info();
			printf("  %10u %9u  %dch %5u Hz %9u samples %7.2f s", m.id, m.size, in.channels, in.sampleRate, in.sampleCount, double(in.sampleCount) / in.sampleRate);
			if (in.loopEnd) printf("  loop %u..%u", in.loopStart, in.loopEnd);
			printf("\n");
		}
	}
	return 0;
}

static int check(int argc, char** argv)
{
	size_t total = 0, failed = 0;
	std::vector<float> buf;
	for (int a = 0; a < argc; a++)
	{
		audio::Bank bank;
		if (!loadBank(bank, argv[a])) return 1;
		size_t bankFailed = 0;
		double seconds = 0;
		for (const audio::Media& m : bank.media)
		{
			total++;
			audio::WemDecoder dec;
			if (!dec.open(bank.mediaData(m), m.size))
			{
				printf("  FAIL %u: %s\n", m.id, dec.error().c_str());
				bankFailed++;
				continue;
			}
			const audio::WemInfo& in = dec.info();
			buf.resize(4096 * size_t(in.channels));
			size_t frames = 0, n;
			float peak = 0;
			bool finite = true;
			while ((n = dec.read(buf.data(), 4096)))
			{
				for (size_t i = 0; i < n * size_t(in.channels); i++)
				{
					if (!std::isfinite(buf[i])) finite = false;
					peak = std::max(peak, std::fabs(buf[i]));
				}
				frames += n;
			}
			if (frames != in.sampleCount || !finite || peak > 4.0f)
			{
				printf("  FAIL %u: %zu of %u frames, peak %.3f%s%s\n", m.id, frames, in.sampleCount, peak, finite ? "" : ", non-finite", dec.error().empty() ? "" : (", " + dec.error()).c_str());
				bankFailed++;
				continue;
			}
			seconds += double(frames) / in.sampleRate;
		}
		failed += bankFailed;
		printf("%s: %zu media, %zu failed, %.1f s decoded\n", argv[a], bank.media.size(), bankFailed, seconds);
	}
	printf("total %zu media, %zu failed\n", total, failed);
	return failed ? 1 : 0;
}

static void put16(FILE* f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put32(FILE* f, uint32_t v) { put16(f, uint16_t(v)); put16(f, uint16_t(v >> 16)); }

static int wav(const char* bankPath, const char* idText, const char* outPath)
{
	audio::Bank bank;
	if (!loadBank(bank, bankPath)) return 1;
	uint32_t size;
	const uint8_t* wem = bank.mediaData(uint32_t(strtoul(idText, nullptr, 10)), size);
	if (!wem) { fprintf(stderr, "no media %s in %s\n", idText, bankPath); return 1; }
	audio::WemDecoder dec;
	if (!dec.open(wem, size)) { fprintf(stderr, "%s\n", dec.error().c_str()); return 1; }
	const audio::WemInfo& in = dec.info();
	FILE* f = fopen(outPath, "wb");
	if (!f) { perror(outPath); return 1; }
	uint32_t ch = uint32_t(in.channels), bytes = in.sampleCount * ch * 2;
	fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16); put16(f, 1); put16(f, uint16_t(ch)); put32(f, in.sampleRate); put32(f, in.sampleRate * ch * 2); put16(f, uint16_t(ch * 2)); put16(f, 16);
	fwrite("data", 1, 4, f); put32(f, bytes);
	std::vector<float> buf(4096 * ch);
	size_t n;
	while ((n = dec.read(buf.data(), 4096)))
	{
		for (size_t i = 0; i < n * ch; i++) put16(f, uint16_t(int16_t(std::lround(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f))));
	}
	fclose(f);
	printf("%s: %u ch, %u Hz, %u samples\n", outPath, ch, in.sampleRate, in.sampleCount);
	return 0;
}

static int hircCheck(int argc, char** argv)
{
	std::vector<std::unique_ptr<audio::Bank>> banks;
	audio::hirc::Hierarchy h;
	for (int a = 0; a < argc; a++)
	{
		banks.push_back(std::make_unique<audio::Bank>());
		if (!loadBank(*banks.back(), argv[a])) return 1;
		size_t ev = h.events.size(), ac = h.actions.size(), sg = h.segments.size(), tr = h.tracks.size(), rs = h.ranseqs.size(), sw = h.switches.size(), sk = h.skipped;
		size_t so = h.sounds.size(), co = h.containers.size(), sc = h.switchCntrs.size(), la = h.layerCntrs.size(), mx = h.mixers.size();
		if (!h.add(*banks.back())) { printf("%s: FAIL %s\n", argv[a], h.error().c_str()); continue; }
		printf("%-28s +%zu events +%zu actions +%zu segments +%zu tracks +%zu ranseq +%zu switches | +%zu sounds +%zu ranseq-cntr +%zu switch-cntr +%zu layer-cntr"
			" +%zu mixers, %zu skipped\n", argv[a], h.events.size() - ev, h.actions.size() - ac, h.segments.size() - sg, h.tracks.size() - tr, h.ranseqs.size() - rs,
			h.switches.size() - sw, h.sounds.size() - so, h.containers.size() - co, h.switchCntrs.size() - sc, h.layerCntrs.size() - la, h.mixers.size() - mx, h.skipped - sk);
	}
	size_t unresolved = 0, streamed = 0;
	for (auto& [id, s] : h.sounds)
	{
		if (s.stream != 0) streamed++;
		else if (s.plugin == audio::hirc::kPluginSilence) continue;
		else if (!h.media.count(s.source)) { unresolved++; fprintf(stderr, "UNRES sound %u media %u plugin %08x\n", id, s.source, s.plugin); }
	}
	auto check = [&](const audio::hirc::Object& o) { for (uint32_t c : o.children) if (!h.object(c)) { unresolved++; fprintf(stderr, "UNRES child %u of %u\n", c, o.id); } };
	for (auto& [id, o] : h.containers) check(o);
	for (auto& [id, o] : h.switchCntrs) check(o);
	for (auto& [id, o] : h.layerCntrs) check(o);
	printf("sound structure: %zu sounds (%zu not embedded), %zu ranseq-cntr, %zu switch-cntr, %zu layer-cntr, %zu mixers; %zu unresolved child/media refs\n",
		h.sounds.size(), streamed, h.containers.size(), h.switchCntrs.size(), h.layerCntrs.size(), h.mixers.size(), unresolved);
	// Attenuation objects (HIRC 0x0E): how many, with a cone, the curveToUse slots in use, and every node's attenuation id resolved.
	{
		size_t cones = 0, badRef = 0, used[7] = {};
		float maxDist = 0;
		for (auto& [id, a] : h.attenuations)
		{
			cones += a.cone;
			for (int k = 0; k < 7; k++) used[k] += a.curveToUse[k] != 0xFF;
			maxDist = std::max(maxDist, a.maxDistance());
		}
		auto ref = [&](const audio::hirc::Object& o) { if (o.p.attenuation && !h.attenuations.count(o.p.attenuation)) { badRef++; fprintf(stderr, "UNRES attenuation %u of %u\n", o.p.attenuation, o.id); } };
		for (auto& [id, o] : h.sounds) ref(o);
		for (auto& [id, o] : h.containers) ref(o);
		for (auto& [id, o] : h.switchCntrs) ref(o);
		for (auto& [id, o] : h.layerCntrs) ref(o);
		for (auto& [id, o] : h.mixers) ref(o);
		printf("attenuation: %zu objects, %zu with a cone, curveToUse slots in use [volume %zu, aux game %zu, aux user %zu, lpf %zu, 4: %zu, 5: %zu, 6: %zu], farthest point %g u, %zu unresolved refs\n",
			h.attenuations.size(), cones, used[0], used[1], used[2], used[3], used[4], used[5], used[6], maxDist, badRef);
		for (auto& [id, a] : h.attenuations)
		{
			if (!getenv("KAUDIO_ATT")) break;
			printf("  att %u: cone %d, curves", id, (int)a.cone);
			for (const auto& c : a.curves) { printf(" [scaling %u, %zu points, %g..%g]", c.scaling, c.points.size(), c.points.empty() ? 0.0f : c.points.front().x, c.points.empty() ? 0.0f : c.points.back().x); }
			printf(", dB at 20/150/250/1000: %.1f %.1f %.1f %.1f\n", a.volumeDb(20), a.volumeDb(150), a.volumeDb(250), a.volumeDb(1000));
		}
	}
	size_t silent = 0, missing = 0;
	for (auto& [id, s] : h.segments)
	{
		if (s.children.empty()) silent++;
		for (uint32_t t : s.children)
		{
			auto it = h.tracks.find(t);
			if (it == h.tracks.end()) { missing++; continue; }
			for (uint32_t m : it->second.sources)
			{
				if (!h.media.count(m)) missing++;
			}
		}
	}
	printf("total: %zu events, %zu segments (%zu silent), %zu tracks, %zu ranseq, %zu switches; %zu unresolved track/media refs\n", h.events.size(), h.segments.size(), silent,
		h.tracks.size(), h.ranseqs.size(), h.switches.size(), missing);
	return 0;
}

// Event -> action -> sound-structure tree with the parameters the players use. Ids are labelled from a names file (one name per line,
// docs/reference/audio/names.txt) where the hash matches.
static int soundTree(const char* namesPath, int argc, char** argv)
{
	namespace H = audio::hirc;
	std::unordered_map<uint32_t, std::string> names;
	if (FILE* f = fopen(namesPath, "r"))
	{
		char line[256];
		while (fgets(line, sizeof line, f))
		{
			std::string n(line);
			while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
			if (!n.empty()) names[audio::wwiseHash(n)] = n;
		}
		fclose(f);
	}
	auto name = [&](uint32_t id) { auto it = names.find(id); return it == names.end() ? std::to_string(id) : it->second; };
	std::vector<std::unique_ptr<audio::Bank>> banks;
	H::Hierarchy h;
	for (int a = 0; a < argc; a++)
	{
		banks.push_back(std::make_unique<audio::Bank>());
		if (!loadBank(*banks.back(), argv[a])) return 1;
		if (!h.add(*banks.back())) { printf("%s: FAIL %s\n", argv[a], h.error().c_str()); return 1; }
	}
	auto params = [&](const H::Params& p)
	{
		std::string s;
		char b[96];
		for (const H::Prop& q : p.props)
		{
			if (q.id == H::kPropLoop) snprintf(b, sizeof b, " loop=%d", int32_t(q.raw));
			else { float v; memcpy(&v, &q.raw, 4); snprintf(b, sizeof b, " p%02x=%g", q.id, v); }
			s += b;
		}
		for (const H::RangedProp& q : p.ranged) { snprintf(b, sizeof b, " r%02x=[%g,%g]", q.id, q.min, q.max); s += b; }
		if (p.posOverride) { snprintf(b, sizeof b, " %s", p.is3d ? "3d" : "2d"); s += b; }
		if (p.attenuation) { snprintf(b, sizeof b, " att=%u", p.attenuation); s += b; }
		for (const H::Rtpc& r : p.rtpcs)
		{
			snprintf(b, sizeof b, " rtpc(%s->param%u:", name(r.id).c_str(), r.param);
			s += b;
			for (const H::Point& q : r.points) { snprintf(b, sizeof b, " %g,%g", q.x, q.y); s += b; }
			s += ")";
		}
		for (uint32_t g : p.stateGroups) s += " states(" + name(g) + ")";
		return s;
	};
	std::function<void(uint32_t, int)> show = [&](uint32_t id, int depth)
	{
		std::string pad(size_t(depth) * 2, ' ');
		const H::Object* o = h.object(id);
		if (!o) { printf("%s%s (not a sound object%s)\n", pad.c_str(), name(id).c_str(), h.node(id) ? ": music" : ""); return; }
		if (depth > 12) return;
		if (auto it = h.sounds.find(id); it != h.sounds.end())
		{
			const H::Sound& s = it->second;
			if (s.plugin == H::kPluginSilence) printf("%sSound %u silence %gs [-%g,+%g]%s\n", pad.c_str(), id, s.silenceS, s.silenceMinusS, s.silencePlusS, params(s.p).c_str());
			else printf("%sSound %u media %u%s\n", pad.c_str(), id, s.source, params(s.p).c_str());
			return;
		}
		if (auto it = h.containers.find(id); it != h.containers.end())
		{
			const H::Container& c = it->second;
			printf("%sRanSeq %u %s%s loops=%u[%u,%u] trans=%u %gms[%g,%g] avoid=%u%s%s%s playlist=%zu%s\n", pad.c_str(), id, c.mode ? "sequence" : "random",
				c.randomMode ? " shuffle" : "", c.loopCount, c.loopModMin, c.loopModMax, c.transition, c.transitionMs, c.transitionModMinMs, c.transitionModMaxMs, c.avoidRepeat,
				c.continuous ? " continuous" : " step", c.resetEachPlay ? " reset" : "", c.global ? " global" : "", c.playlist.size(), params(c.p).c_str());
			for (const H::Container::Entry& e : c.playlist) { printf("%s  w=%d", pad.c_str(), e.weight); show(e.id, 0); }
			for (uint32_t k : c.children)
			{
				if (std::none_of(c.playlist.begin(), c.playlist.end(), [&](const H::Container::Entry& e) { return e.id == k; })) { printf("%s  (not in playlist)", pad.c_str()); show(k, 0); }
			}
			return;
		}
		if (auto it = h.switchCntrs.find(id); it != h.switchCntrs.end())
		{
			const H::SwitchCntr& s = it->second;
			printf("%sSwitch %u %s %s default=%s%s%s\n", pad.c_str(), id, s.groupType ? "state" : "switch", name(s.group).c_str(), name(s.defaultValue).c_str(),
				s.continuousValidation ? " continuous" : "", params(s.p).c_str());
			for (const H::SwitchCntr::NodeParams& n : s.nodeParams)
			{
				printf("%s  node %u firstOnly=%d continue=%d onSwitch=%u fadeOut=%d fadeIn=%d\n", pad.c_str(), n.node, n.firstOnly, n.continuePlayback, n.onSwitch, n.fadeOutMs, n.fadeInMs);
			}
			for (const H::SwitchCntr::Package& k : s.packages)
			{
				printf("%s  [%s]\n", pad.c_str(), name(k.value).c_str());
				for (uint32_t c : k.nodes) show(c, depth + 2);
			}
			return;
		}
		if (auto it = h.layerCntrs.find(id); it != h.layerCntrs.end())
		{
			const H::LayerCntr& l = it->second;
			printf("%sLayer %u%s\n", pad.c_str(), id, params(l.p).c_str());
			for (const H::Layer& y : l.layers)
			{
				printf("%s  layer %u crossfade=%s", pad.c_str(), y.id, y.crossfadeRtpc ? name(y.crossfadeRtpc).c_str() : "-");
				for (const H::Rtpc& r : y.rtpcs)
				{
					printf(" rtpc(%s->param%u:", name(r.id).c_str(), r.param);
					for (const H::Point& q : r.points) printf(" %g,%g", q.x, q.y);
					printf(")");
				}
				for (const H::Layer::Assoc& a : y.assocs)
				{
					printf(" child %u:", a.child);
					for (const H::Point& q : a.curve) printf(" %g,%g", q.x, q.y);
				}
				printf("\n");
			}
			for (uint32_t c : l.children) show(c, depth + 1);
			return;
		}
		printf("%sActorMixer %u%s\n", pad.c_str(), id, params(o->p).c_str());
	};
	std::vector<uint32_t> evs;
	for (auto& [id, e] : h.events) evs.push_back(id);
	std::sort(evs.begin(), evs.end(), [&](uint32_t a, uint32_t b) { return name(a) < name(b); });
	for (uint32_t ev : evs)
	{
		printf("Event %s\n", name(ev).c_str());
		for (uint32_t aid : h.events[ev].actions)
		{
			auto it = h.actions.find(aid);
			if (it == h.actions.end()) { printf("  action %u missing\n", aid); continue; }
			const H::Action& a = it->second;
			printf("  action 0x%04x target=%s fade=%dms delay=%dms curve=%u", a.type, name(a.target).c_str(), a.fadeMs, a.delayMs, a.curve);
			if (a.cls() == 0x12 || a.cls() == 0x19) printf(" %s=%s", name(a.group).c_str(), name(a.value).c_str());
			if (a.cls() >= 0x08 && a.cls() <= 0x0F) printf(" meaning=%u value=%g [%g,%g]", a.propMeaning, a.propBase, a.propMin, a.propMax);
			printf("\n");
			if (a.cls() == 0x04 && h.object(a.target))
			{
				// Parent chain first (actor-mixers carry shared volume / rtpcs).
				for (uint32_t p = h.object(a.target)->p.parent; p; )
				{
					const H::Object* po = h.object(p);
					if (!po) break;
					printf("    ^ parent %u%s\n", p, params(po->p).c_str());
					p = po->p.parent;
				}
				show(a.target, 2);
			}
		}
	}
	return 0;
}

// GameAmbience offline: ZONE for SECONDS, with a timeline of changes "T:key[=value]" (T in seconds): zone=NAME, day, night, height=H (camera
// above ground), rain=V, wind=V, inside, inside_big, outside, pause, resume (Pause_Game / Resume_Game from SFX_General_Bank: the bus duck, with the
// Init.bnk bus tree). OUT "-" = no wav. Prints each voice started and a per-10 s level.
static int ambRun(int argc, char** argv)
{
	const char* dir = argv[0];
	const char* outPath = argv[2];
	double seconds = atof(argv[3]);
	uint32_t seed = 1;
	struct Change { double t; std::string key, value; };
	std::vector<Change> changes { { 0, "zone", argv[1] } };
	for (int i = 4; i < argc; i++)
	{
		if (!strncmp(argv[i], "seed=", 5)) { seed = uint32_t(atoi(argv[i] + 5)); continue; }
		const char* colon = strchr(argv[i], ':');
		if (!colon) { fprintf(stderr, "bad change %s (want T:key[=value])\n", argv[i]); return 2; }
		std::string kv(colon + 1);
		size_t eq = kv.find('=');
		changes.push_back({ atof(argv[i]), kv.substr(0, eq), eq == std::string::npos ? "" : kv.substr(eq + 1) });
	}
	std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.t < b.t; });

	audio::BankSet banks(dir);
	for (const char* b : { "Init", "SFX_General_Bank" })
	{
		if (!banks.load(b)) { fprintf(stderr, "%s\n", banks.error().c_str()); return 1; }
	}
	audio::Mixer mix(48000);
	for (const auto& [bus, parent] : banks.hierarchy().buses) { mix.setWwBusParent(bus, parent); }
	audio::GameAmbience amb(banks, mix, seed);
	audio::SoundPlayer ui(mix, banks.hierarchy(), seed);
	FILE* f = strcmp(outPath, "-") ? fopen(outPath, "wb") : nullptr;
	uint32_t total = uint32_t(seconds * mix.sampleRate());
	if (f)
	{
		fwrite("RIFF", 1, 4, f); put32(f, 36 + total * 4); fwrite("WAVEfmt ", 1, 8, f);
		put32(f, 16); put16(f, 1); put16(f, 2); put32(f, uint32_t(mix.sampleRate())); put32(f, uint32_t(mix.sampleRate()) * 4); put16(f, 4); put16(f, 16);
		fwrite("data", 1, 4, f); put32(f, total * 4);
	}
	std::vector<float> buf(1024 * 2);
	size_t next = 0, shown = 0;
	double sq = 0, peak = 0;
	uint32_t window = 0;
	for (uint32_t done = 0; done < total;)
	{
		double now = double(done) / mix.sampleRate();
		for (; next < changes.size() && changes[next].t <= now; next++)
		{
			const Change& c = changes[next];
			printf("%8.1f s  %s%s%s\n", now, c.key.c_str(), c.value.empty() ? "" : "=", c.value.c_str());
			float v = float(atof(c.value.c_str()));
			if (c.key == "zone")
			{
				audio::GameAmbience::Zone z = audio::GameAmbience::zoneByName(c.value);
				if (z == audio::GameAmbience::kNoZone && c.value != "None") { fprintf(stderr, "unknown zone %s\n", c.value.c_str()); return 2; }
				amb.setZone(z);
			}
			else if (c.key == "day") amb.setDay(true);
			else if (c.key == "night") amb.setDay(false);
			else if (c.key == "height") amb.setCameraHeight(v);
			else if (c.key == "rain") amb.setRain(v);
			else if (c.key == "wind") amb.setWind(v);
			else if (c.key == "inside") amb.setIndoors(true);
			else if (c.key == "inside_big") amb.setIndoors(true, true);
			else if (c.key == "outside") amb.setIndoors(false);
			else if (c.key == "pause" || c.key == "resume") ui.postEvent(audio::wwiseHash(c.key == "pause" ? "Pause_Game" : "Resume_Game"), audio::kSfx);
			else { fprintf(stderr, "unknown key %s\n", c.key.c_str()); return 2; }
		}
		amb.update();
		ui.update();
		const auto& hist = amb.player()->history;
		for (; shown < hist.size(); shown++)
		{
			printf("%8.1f s    sound %10u  %6.1f dB  pan %+.2f  rate %.3f\n", double(hist[shown].start) / mix.sampleRate(), hist[shown].sound, hist[shown].gainDb, hist[shown].pan,
				hist[shown].rate);
		}
		int n = int(std::min<uint32_t>(1024, total - done));
		mix.render(buf.data(), n);
		for (int i = 0; i < n * 2; i++)
		{
			sq += double(buf[i]) * buf[i];
			peak = std::max(peak, double(std::fabs(buf[i])));
		}
		window += uint32_t(n);
		if (f)
		{
			for (int i = 0; i < n * 2; i++) put16(f, uint16_t(int16_t(std::lround(buf[i] * 32767.0f))));
		}
		done += uint32_t(n);
		if (window >= uint32_t(10 * mix.sampleRate()) || done == total)
		{
			double rms = std::sqrt(sq / (2.0 * window));
			printf("%8.1f s  level rms %6.1f dBFS peak %6.1f dBFS, %zu instances, %zu voices\n", double(done) / mix.sampleRate(), 20 * std::log10(std::max(rms, 1e-9)),
				20 * std::log10(std::max(peak, 1e-9)), amb.player()->instances(), mix.voices());
			sq = peak = 0;
			window = 0;
		}
	}
	if (f) fclose(f);
	printf("%s", amb.player()->describe().c_str());
	return 0;
}

// GameMusic offline: biome music for MINUTES with the game's re-trigger timer; optional night switch and frequency. OUT "-" = no wav.
static int musicRun(int argc, char** argv)
{
	const char* dir = argv[0];
	std::string biome = argv[1];
	const char* outPath = argv[2];
	double minutes = atof(argv[3]), nightAt = -1;
	float freq = 5;
	uint32_t seed = 1;
	for (int i = 4; i < argc; i++)
	{
		if (!strncmp(argv[i], "night=", 6)) nightAt = atof(argv[i] + 6);
		else if (!strncmp(argv[i], "freq=", 5)) freq = float(atof(argv[i] + 5));
		else if (!strncmp(argv[i], "seed=", 5)) seed = uint32_t(atoi(argv[i] + 5));
	}
	static const char* const names[] = { "Dunes", "Savannah", "Swamplands", "Tarsands" };
	int b = 0;
	while (b < 4 && biome != names[b]) b++;
	if (b == 4) { fprintf(stderr, "biome must be Dunes, Savannah, Swamplands or Tarsands\n"); return 2; }

	audio::BankSet banks(dir);
	audio::Mixer mix(48000);
	audio::GameMusic music(banks, mix, seed);
	music.setFrequencyMinutes(freq);
	music.setBiome(audio::GameMusic::Biome(b));
	FILE* f = strcmp(outPath, "-") ? fopen(outPath, "wb") : nullptr;
	uint32_t total = uint32_t(minutes * 60 * mix.sampleRate());
	if (f)
	{
		fwrite("RIFF", 1, 4, f); put32(f, 36 + total * 4); fwrite("WAVEfmt ", 1, 8, f);
		put32(f, 16); put16(f, 1); put16(f, 2); put32(f, uint32_t(mix.sampleRate())); put32(f, uint32_t(mix.sampleRate()) * 4); put16(f, 4); put16(f, 16);
		fwrite("data", 1, 4, f); put32(f, total * 4);
	}
	std::vector<float> buf(1024 * 2);
	double audible = 0, peak = 0;
	bool night = false;
	for (uint32_t done = 0; done < total;)
	{
		if (nightAt >= 0 && !night && done >= nightAt * mix.sampleRate()) { night = true; music.setDay(false); printf("%8.1f s  -> Night\n", double(done) / mix.sampleRate()); }
		music.update();
		int n = int(std::min<uint32_t>(1024, total - done));
		mix.render(buf.data(), n);
		float blockPeak = 0;
		for (int i = 0; i < n * 2; i++) blockPeak = std::max(blockPeak, std::fabs(buf[i]));
		if (blockPeak > 1e-4f) audible += double(n) / mix.sampleRate();
		peak = std::max(peak, double(blockPeak));
		if (f)
		{
			for (int i = 0; i < n * 2; i++) put16(f, uint16_t(int16_t(std::lround(buf[i] * 32767.0f))));
		}
		done += uint32_t(n);
	}
	if (f) fclose(f);
	const audio::hirc::Hierarchy& h = banks.hierarchy();
	for (const auto& p : music.player()->history)
	{
		const auto& seg = h.segments.at(p.segment);
		printf("%8.1f s  node %10u  segment %10u  %6.1f s  %s\n", double(p.start) / mix.sampleRate(), p.node, p.segment, seg.durationMs / 1000,
			seg.children.empty() ? "silent" : "music");
	}
	printf("%zu segments, audible %.0f of %.0f s, peak %.3f\n", music.player()->history.size(), audible, minutes * 60, peak);
	return 0;
}

// Starts each MEDIA_ID (looping) on the mixer: "id" or "id@pan".
static bool startVoices(audio::Mixer& mix, const audio::Bank& bank, int n, char** ids)
{
	for (int i = 0; i < n; i++)
	{
		audio::PlayParams p;
		p.loop = true;
		p.fadeIn = 0.5f;
		if (const char* at = strchr(ids[i], '@')) p.pan = strtof(at + 1, nullptr);
		uint32_t size;
		const uint8_t* wem = bank.mediaData(uint32_t(strtoul(ids[i], nullptr, 10)), size);
		if (!wem) { fprintf(stderr, "no media %s\n", ids[i]); return false; }
		if (!mix.play(wem, size, p)) { fprintf(stderr, "%s\n", mix.error().c_str()); return false; }
	}
	return true;
}

static int mixWav(const char* bankPath, const char* outPath, double seconds, int n, char** ids)
{
	audio::Bank bank;
	if (!loadBank(bank, bankPath)) return 1;
	audio::Mixer mix(48000);
	if (!startVoices(mix, bank, n, ids)) return 1;
	FILE* f = fopen(outPath, "wb");
	if (!f) { perror(outPath); return 1; }
	uint32_t frames = uint32_t(seconds * mix.sampleRate()), bytes = frames * 4;
	fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16); put16(f, 1); put16(f, 2); put32(f, uint32_t(mix.sampleRate())); put32(f, uint32_t(mix.sampleRate()) * 4); put16(f, 4); put16(f, 16);
	fwrite("data", 1, 4, f); put32(f, bytes);
	std::vector<float> buf(1024 * 2);
	for (uint32_t done = 0; done < frames;)
	{
		int n = int(std::min<uint32_t>(1024, frames - done));
		mix.render(buf.data(), n);
		for (int i = 0; i < n * 2; i++) put16(f, uint16_t(int16_t(std::lround(buf[i] * 32767.0f))));
		done += uint32_t(n);
	}
	fclose(f);
	printf("%s: %u frames at %d Hz, %zu voices left\n", outPath, frames, mix.sampleRate(), mix.voices());
	return 0;
}

static int play(const char* bankPath, double seconds, int n, char** ids)
{
	audio::Bank bank;
	if (!loadBank(bank, bankPath)) return 1;
	audio::Mixer mix;
	if (!mix.startDevice()) { fprintf(stderr, "%s\n", mix.error().c_str()); return 1; }
	if (!startVoices(mix, bank, n, ids)) return 1;
	printf("playing at %d Hz for %.1f s\n", mix.sampleRate(), seconds);
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 3) return usage();
	if (!strcmp(argv[1], "list")) return list(argc - 2, argv + 2);
	if (!strcmp(argv[1], "check")) return check(argc - 2, argv + 2);
	if (!strcmp(argv[1], "hirc")) return hircCheck(argc - 2, argv + 2);
	if (!strcmp(argv[1], "tree") && argc >= 4) return soundTree(argv[2], argc - 3, argv + 3);
	if (!strcmp(argv[1], "music") && argc >= 6) return musicRun(argc - 2, argv + 2);
	if (!strcmp(argv[1], "amb") && argc >= 6) return ambRun(argc - 2, argv + 2);
	if (!strcmp(argv[1], "wav") && argc == 5) return wav(argv[2], argv[3], argv[4]);
	if (!strcmp(argv[1], "mix") && argc >= 6) return mixWav(argv[2], argv[3], atof(argv[4]), argc - 5, argv + 5);
	if (!strcmp(argv[1], "play") && argc >= 5) return play(argv[2], atof(argv[3]), argc - 4, argv + 4);
	return usage();
}
