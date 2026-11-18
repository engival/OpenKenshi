#include "audio/audiohook.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "audio/sound.hpp"
#include "app/config.hpp"
#include "kworld/gamedata.hpp"
#include "render/sky.hpp"

// The game's per-update audio inputs (AUDIO_EXE.md section 4, FUN_1406b0140 / FUN_1409e8f70; AUDIO_ENGINE.md "Exe facts"):
//  - the area at the listener: BIOME_GROUP `music` (0 Dunes, 1 Savannah, 2 Swamplands, 3 Tarsands, 4 None) and `ambience` (0 Ashlands .. 9 Tarsands,
//    alphabetical, = GameAmbience::Zone). The area is the areasmap.tga cell (pixel colour = BIOME_GROUP.index, TERRAIN_FORMAT.md), read at the camera
//    here; the game reads it at its +0x58 node (not identified).
//  - Camera_Zoom from the camera height above the ground; day = hours in (sunrise, sunset) of GLOBAL CONSTANTS (MINE: FUN_14066cb20's bounds are not
//    matched to the record fields).
//  - Rain_Intensity = WEATHER rain intensity x strength, Intensity = wind intensity x wind speed x 0.01 (14167c310). The client has no weather strength
//    (SkyWeather is the record's, strength 1, wind speed max).
namespace
{
	constexpr int kN = 256;
	constexpr double kWorld = 294912.0;

	struct Areas
	{
		std::vector<uint8_t> music, ambience;	// kN * kN; row 0 = north (z = -147456), column 0 = x = -147456
		std::string error;
	};

	Areas loadAreas(const std::string& dataDir)
	{
		Areas a;
		const std::string path = dataDir + "/newland/land/areasmap.tga";
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) { a.error = "areasmap.tga not found: " + path; return a; }
		unsigned char h[18];
		std::vector<unsigned char> px;
		bool ok = fread(h, 1, 18, f) == 18 && h[2] == 2 && h[16] == 24 && (h[12] | h[13] << 8) == kN && (h[14] | h[15] << 8) == kN && !fseek(f, h[0], SEEK_CUR);
		if (ok)
		{
			px.resize((size_t)kN * kN * 3);
			ok = fread(px.data(), 1, px.size(), f) == px.size();
		}
		fclose(f);
		if (!ok) { a.error = "areasmap.tga is not the expected 256x256 uncompressed 24-bit TGA"; return a; }
		const bool topFirst = (h[17] & 0x20) != 0;
		std::map<uint32_t, std::pair<uint8_t, uint8_t>> byColour;	// -> music, ambience
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		for (const kdata::Record* g : gd.byType(kworld::itemTypeByName("BIOME_GROUP")))
		{
			byColour[(uint32_t)g->intOr("index", 0) & 0xffffff] = { (uint8_t)g->intOr("music", audio::GameMusic::kNone), (uint8_t)g->intOr("ambience", audio::GameAmbience::kNoZone) };
		}
		a.music.assign((size_t)kN * kN, audio::GameMusic::kNone);
		a.ambience.assign((size_t)kN * kN, audio::GameAmbience::kNoZone);
		for (int row = 0; row < kN; row++)
		{
			const int src = topFirst ? row : kN - 1 - row;
			for (int col = 0; col < kN; col++)
			{
				const unsigned char* p = &px[((size_t)src * kN + col) * 3];	// B, G, R
				const auto it = byColour.find((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
				if (it == byColour.end()) { continue; }
				a.music[(size_t)row * kN + col] = it->second.first;
				a.ambience[(size_t)row * kN + col] = it->second.second;
			}
		}
		return a;
	}

	// Volumes, percent (config.hpp; no options screen yet). environment = zone ambience and weather; effects = sound effects and voice.
	const cfg::Int kMaster(cfg::Group{ "Volume" }, "audio.master", 100, 0, 100, "Master volume, percent");
	const cfg::Int kMusic(cfg::Group{ "Volume" }, "audio.music", 100, 0, 100, "Music volume, percent (0 also stops new music)");
	const cfg::Int kEnvironment(cfg::Group{ "Volume" }, "audio.environment", 100, 0, 100, "Zone ambience and weather volume, percent");
	const cfg::Int kEffects(cfg::Group{ "Volume" }, "audio.effects", 100, 0, 100, "Sound effects and voice volume, percent");
	// The per-event world lines: every post's distance / voices, a gated post, every switch set (one footstep = 3-4 lines; a walking squad is thousands a minute).
	// Off: only the first "not in the loaded banks" / "object not registered" per (event, reason) is logged (kenshi-sb3.1).
	const cfg::Bool kAudioTrace(cfg::Group{ "Logging" }, "debug.audio_trace", false, "Debug: log every world sound post (footsteps, impacts: distance, gated, voices) and every object switch to stderr; also the client's 'notify:' lines for animation sound events");

	float gainOf(float percent) { return percent * percent * 1e-4f; }	// squared: closer to equal loudness steps than linear amplitude

	struct Inputs
	{
		double pos[3] = {}, fwd[3] = { 0, 0, 1 }, right[3] = { 1, 0, 0 };
		double ground = 0, hours = 12;
		float rain = 0, wind = 0;
	};

	struct Hook
	{
		std::string dataDir;
		std::thread thread;
		std::mutex m;
		std::condition_variable cv;
		bool quit = false, have = false;
		Inputs in;
		std::vector<UiPost> uiEvents;	// audiohook_event() / audiohook_ui() calls the thread has not run yet
		struct ObjectCmd { enum Kind { kRemove, kSwitch, kPost } kind; uint32_t object; std::string a, b; float gate; };
		std::map<uint32_t, std::array<double, 3>> objects;	// world sound objects: the latest position of each
		struct Indoor { bool inside = false, big = false; std::string material; float integrity = -1; } indoor;
		std::vector<ObjectCmd> objectCmds;	// object switches, posts and removals the thread has not run yet, in order

		void run();
	};

	std::unique_ptr<Hook> g_hook;
	std::vector<UiPost> g_early;	// audiohook_event() / audiohook_ui() calls before audiohook_init: run once the thread starts

	void Hook::run()
	{
		Areas areas;
		SkyConstants k;
		try
		{
			areas = loadAreas(dataDir);
			k = SkyConstants::load(dataDir);
		}
		catch (const std::exception& e)
		{
			areas.error = std::string("game data: ") + e.what();
		}
		if (!areas.error.empty()) { fprintf(stderr, "audio: %s; no music or ambience\n", areas.error.c_str()); return; }
		audio::BankSet banks(dataDir + "/audio");
		// The boot pass (AUDIO_EXE.md section 4, kinspect audio_load_pause.md 1a): the UI sounds and the menu music come from SFX_General_Bank and
		// MT_Music_Bank, loaded with the ambience banks; VO_Bank (v112, 443 media: VO_Get_Hit, VO_Creature_Take_Damage / Die / Victory and the Alert / Passive pair, the
		// VO_Races and Creature_VO switches) is in the game's boot list too (audio_load_pause.md 1a, kenshi-h2v.29.24); Boot_Up_Game (posted by main.cpp under the title) starts the menu music. Init holds the
		// bus tree (the pause duck's SFX Bus and its children).
		for (const char* b : { "Init", "SFX_General_Bank", "MT_Music_Bank", "VO_Bank" })
		{
			if (!banks.load(b)) { fprintf(stderr, "audio: %s\n", banks.error().c_str()); }
		}
		audio::Mixer mix(48000);
		for (const auto& [bus, parent] : banks.hierarchy().buses) { mix.setWwBusParent(bus, parent); }
		// KAUDIO_WAV=FILE: render in real time to a 48 kHz stereo float WAV instead of the device (headless tests).
		const char* wavPath = getenv("KAUDIO_WAV");
		std::thread tap;
		std::atomic<bool> tapQuit = false;
		if (wavPath && *wavPath)
		{
			FILE* wf = fopen(wavPath, "wb");
			if (!wf) { fprintf(stderr, "audio: cannot write %s\n", wavPath); return; }
			tap = std::thread([&mix, &tapQuit, wf]
			{
				const unsigned char hdr[44] = { 'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0xff, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 2, 0,
					0x80, 0xbb, 0, 0, 0, 0xdc, 5, 0, 8, 0, 32, 0, 'd', 'a', 't', 'a', 0xff, 0xff, 0xff, 0xff };
				fwrite(hdr, 1, sizeof hdr, wf);
				std::vector<float> buf(2 * 960);
				auto next = std::chrono::steady_clock::now();
				while (!tapQuit)
				{
					mix.render(buf.data(), 960);
					fwrite(buf.data(), sizeof(float), buf.size(), wf);
					next += std::chrono::milliseconds(20);
					std::this_thread::sleep_until(next);
				}
				const long n = ftell(wf) - 44;
				const uint32_t sizes[2] = { (uint32_t)(n + 36), (uint32_t)n };
				fseek(wf, 4, SEEK_SET); fwrite(&sizes[0], 4, 1, wf);
				fseek(wf, 40, SEEK_SET); fwrite(&sizes[1], 4, 1, wf);
				fclose(wf);
			});
		}
		else if (!mix.startDevice()) { fprintf(stderr, "audio: %s\n", mix.error().c_str()); return; }
		audio::GameMusic music(banks, mix, (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
		audio::GameAmbience amb(banks, mix, (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() * 7919u);
		audio::SoundPlayer world(mix, banks.hierarchy(), (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() * 15485863u);	// world sound objects, on kSfx
		audio::SoundPlayer uiSound(mix, banks.hierarchy(), (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() * 104729u);	// the UI events, on kSfx
		fprintf(stderr, "audio: %d Hz\n", mix.sampleRate());
		// A UI event (audiohook_event / audiohook_ui): run it on the sound player and the music player, each ignoring the nodes it has no part for, and
		// log what happened -- the event, the switch before it, each action, and every voice or music segment it started -- so a headless run can be
		// checked from stderr.
		std::set<std::string> logged;	// (event, reason) pairs already logged once
		auto once = [&](const std::string& key) { return logged.insert(key).second; };
		auto uiEvent = [&](const UiPost& post)
		{
			const uint32_t id = post.id;
			const audio::hirc::Hierarchy& h = banks.hierarchy();
			const auto ev = h.events.find(id);
			if (ev == h.events.end()) { if (once(post.name + "|banks")) { fprintf(stderr, "audio: event %s (%u): not in the loaded banks\n", post.name.c_str(), id); } return; }
			if (post.group)	// the GUI object's switch, before the post (14070d230 and the others set it the same way)
			{
				uiSound.setSwitch(post.group, post.value);
				fprintf(stderr, "audio:   switch %s (%u) = %s (%u)\n", post.groupName.c_str(), post.group, post.valueName.c_str(), post.value);
			}
			const size_t sounds0 = uiSound.history.size(), music0 = music.player()->history.size();
			uiSound.postEvent(id, audio::kSfx);
			music.player()->postEvent(id);
			uiSound.update();
			fprintf(stderr, "audio: event %s (%u): %zu actions\n", post.name.c_str(), id, ev->second.actions.size());
			for (uint32_t aid : ev->second.actions)
			{
				const auto it = h.actions.find(aid);
				if (it == h.actions.end()) { continue; }
				const audio::hirc::Action& a = it->second;
				switch (a.cls())
				{
					case 0x04: fprintf(stderr, "audio:   play %u (%s)%s\n", a.target, h.object(a.target) ? "sound" : "music", a.fadeMs ? ", fade in" : ""); break;
					case 0x01: fprintf(stderr, "audio:   stop %u, %d ms transition\n", a.target, a.fadeMs); break;
					case 0x0C: fprintf(stderr, "audio:   bus %u to %.1f dB over %d ms\n", a.target, a.propBase, a.fadeMs); break;
					case 0x0D: fprintf(stderr, "audio:   bus %u reset over %d ms\n", a.target, a.fadeMs); break;
					case 0x12: fprintf(stderr, "audio:   state %u = %u\n", a.group, a.value); break;
					default: fprintf(stderr, "audio:   action class 0x%02x, target %u\n", a.cls(), a.target); break;
				}
			}
			for (size_t k = sounds0; k < uiSound.history.size(); k++)
			{
				const auto& p = uiSound.history[k];
				fprintf(stderr, "audio:   sound %u: voice %u at %.2f s, %.1f dB, pan %+.2f, rate %.3f\n", p.sound, p.voice, double(p.start) / mix.sampleRate(), p.gainDb, p.pan, p.rate);
			}
			for (size_t k = music0; k < music.player()->history.size(); k++)
			{
				const auto& p = music.player()->history[k];
				fprintf(stderr, "audio:   music %u: segment %u at %.2f s\n", p.node, p.segment, double(p.start) / mix.sampleRate());
			}
		};
		// A world post (audiohook_object_post): the gate and the distance first, then every voice it started, logged for headless checks.
		auto worldPost = [&](uint32_t object, const std::string& name, float gate)
		{
			const uint32_t id = audio::wwiseHash(name);
			const float dist = world.objectDistance(object);
			const bool trace = kAudioTrace.get();
			if (dist < 0) { if (trace || once(name + "|unregistered")) { fprintf(stderr, "audio: object %u event %s: object not registered\n", object, name.c_str()); } return; }
			if (!banks.hierarchy().events.count(id)) { if (trace || once(name + "|banks")) { fprintf(stderr, "audio: object %u event %s (%u): not in the loaded banks\n", object, name.c_str(), id); } return; }
			const size_t n0 = world.history.size();
			if (!world.postEvent(id, audio::kSfx, object, gate))
			{
				if (trace) { fprintf(stderr, "audio: object %u event %s (%u): gated, %.1f u > %.0f u\n", object, name.c_str(), id, dist, gate); }
				return;
			}
			world.update();
			if (trace) { fprintf(stderr, "audio: object %u event %s (%u): %.1f u, %zu voices\n", object, name.c_str(), id, dist, world.history.size() - n0); }
			for (size_t k = n0; trace && k < world.history.size(); k++)
			{
				const auto& p = world.history[k];
				fprintf(stderr, "audio:   world sound %u: voice %u at %.2f s, %.1f dB, pan %+.2f, rate %.3f, %.1f u\n", p.sound, p.voice, double(p.start) / mix.sampleRate(), p.gainDb, p.pan, p.rate, p.distance);
			}
		};
		uint64_t cfgGen = 0;
		int vol[4] = { -1, -1, -1, -1 };
		auto applyVolumes = [&]
		{
			if (cfg::generation() == cfgGen && vol[0] >= 0) { return; }
			cfgGen = cfg::generation();
			const int v[4] = { kMaster.get(), kMusic.get(), kEnvironment.get(), kEffects.get() };
			if (std::equal(v, v + 4, vol)) { return; }
			std::copy(v, v + 4, vol);
			mix.setMasterGain(gainOf(v[0]));
			music.setVolume(gainOf(v[1]));	// 0 also stops new music triggers, as the game's music volume does
			amb.setVolume(gainOf(v[2]));
			mix.setBusGain(audio::kSfx, gainOf(v[3]));
			mix.setBusGain(audio::kVoice, gainOf(v[3]));
			fprintf(stderr, "audio: volumes master %d music %d environment %d effects %d\n", v[0], v[1], v[2], v[3]);
		};
		applyVolumes();
		auto lastBiome = audio::GameMusic::kNone;
		auto lastZone = audio::GameAmbience::kNoZone;
		bool lastDay = false;
		bool lastInside = false;
		std::unique_lock<std::mutex> lk(m);
		while (!quit)
		{
			cv.wait_for(lk, std::chrono::milliseconds(50));
			if (quit) { break; }
			std::vector<UiPost> events;
			events.swap(uiEvents);
			if (!events.empty())
			{
				lk.unlock();
				for (const UiPost& e : events) { uiEvent(e); }
				lk.lock();
			}
			if (!have) { continue; }
			const Inputs i = in;
			lk.unlock();
			applyVolumes();	// cheap unless an option changed
			uiSound.update();
			// World objects: positions first (a post in the same batch needs its object), then the switches, posts and removals in order.
			std::vector<ObjectCmd> cmds;
			std::map<uint32_t, std::array<double, 3>> positions;
			Hook::Indoor ind;
			{
				std::lock_guard<std::mutex> g(m);
				cmds.swap(objectCmds);
				positions = objects;
				ind = indoor;
			}
			world.setListener(i.pos, i.fwd, i.right);
			if (ind.inside != lastInside) { fprintf(stderr, "audio: %s%s Indoors=%s Building_Integrity=%.0f\n", ind.inside ? (ind.big ? "Enter_Building_Big" : "Enter_Building") : "Leave_Building", "", ind.material.c_str(), ind.integrity); lastInside = ind.inside; }
			amb.setIndoors(ind.inside, ind.big, ind.material, ind.integrity);
			for (const auto& [id, p] : positions) { world.setObject(id, p.data()); }
			for (const ObjectCmd& c : cmds)
			{
				if (c.kind == ObjectCmd::kRemove) { world.removeObject(c.object); }
				else if (c.kind == ObjectCmd::kSwitch)
				{
					world.setSwitch(audio::wwiseHash(c.a), audio::wwiseHash(c.b), c.object);
					if (kAudioTrace.get()) { fprintf(stderr, "audio: object %u switch %s = %s\n", c.object, c.a.c_str(), c.b.c_str()); }
				}
				else { worldPost(c.object, c.a, c.gate); }
			}
			world.update();
			world.history.clear();
			const int col = (int)((i.pos[0] + kWorld / 2) / kWorld * kN), row = (int)((i.pos[2] + kWorld / 2) / kWorld * kN);
			const bool inside = col >= 0 && col < kN && row >= 0 && row < kN;
			const size_t cell = inside ? (size_t)row * kN + col : 0;
			const bool day = i.hours > k.sunrise && i.hours < k.sunset;
			const auto biome = inside ? (audio::GameMusic::Biome)areas.music[cell] : audio::GameMusic::kNone;
			const auto zone = inside && areas.ambience[cell] < audio::GameAmbience::kNoZone ? (audio::GameAmbience::Zone)areas.ambience[cell] : audio::GameAmbience::kNoZone;
			if (biome != lastBiome || zone != lastZone || day != lastDay)
			{
				static const char* const kBiomes[] = { "Dunes", "Savannah", "Swamplands", "Tarsands", "None" };
				fprintf(stderr, "audio: music %s, ambience %s, %s\n", kBiomes[biome], zone == audio::GameAmbience::kNoZone ? "none" : audio::GameAmbience::zoneName(zone), day ? "day" : "night");
				lastBiome = biome; lastZone = zone; lastDay = day;
			}
			music.setDay(day);	// day first: the first Enter_ / Play then already has the right half
			music.setBiome(biome);
			amb.setDay(day);
			amb.setZone(zone);
			amb.setCameraHeight((float)(i.pos[1] - i.ground));
			amb.setRain(i.rain);
			amb.setWind(i.wind);
			music.update();
			amb.update();
			lk.lock();
		}
		lk.unlock();
		if (tap.joinable())
		{
			tapQuit = true;
			tap.join();
		}
		else { mix.stopDevice(); }
	}
}

void audiohook_init(const std::string& dataDir)
{
	if (g_hook) { return; }
	const char* off = getenv("KCLIENT_NOAUDIO");
	if (off && *off && *off != '0') { fprintf(stderr, "audio: off (KCLIENT_NOAUDIO)\n"); return; }
	const char* xdg = getenv("XDG_CONFIG_HOME");
	const char* home = getenv("HOME");
	const std::string old = (xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.config") + "/kclient/audio.cfg";
	std::error_code ec;
	if (std::filesystem::exists(old, ec)) { fprintf(stderr, "audio: %s is no longer read: set audio.master / music / environment / effects in %s, then delete it\n", old.c_str(), cfg::path().c_str()); }
	g_hook = std::make_unique<Hook>();
	g_hook->dataDir = dataDir;
	g_hook->uiEvents.swap(g_early);
	g_hook->thread = std::thread([] { g_hook->run(); });
}

void audiohook_update(const double camPos[3], const double camFwd[3], const double camRight[3], double groundY, const SkyWeather& weather, double hours)
{
	if (!g_hook) { return; }
	std::lock_guard<std::mutex> g(g_hook->m);
	Inputs& i = g_hook->in;
	for (int k = 0; k < 3; k++) { i.pos[k] = camPos[k]; i.fwd[k] = camFwd[k]; i.right[k] = camRight[k]; }
	i.ground = groundY;
	i.hours = hours;
	i.rain = weather.rain * 100.0f;
	i.wind = weather.windIntensity * weather.windSpeed * 0.01f;
	g_hook->have = true;
}

void audiohook_event(const std::string& name)
{
	audiohook_ui({ audio::wwiseHash(name), 0, 0, name, {}, {} });
}

void audiohook_ui(const UiPost& post)
{
	if (!g_hook)
	{
		if (g_early.size() < 64) { g_early.push_back(post); }
		return;
	}
	{
		std::lock_guard<std::mutex> g(g_hook->m);
		if (g_hook->uiEvents.size() >= 64) { g_hook->uiEvents.erase(g_hook->uiEvents.begin()); }	// the thread never got to them: keep the newest
		g_hook->uiEvents.push_back(post);
	}
	g_hook->cv.notify_all();
}

namespace
{
	void objectCmd(Hook::ObjectCmd c)
	{
		if (!g_hook || !c.object) { return; }
		{
			std::lock_guard<std::mutex> g(g_hook->m);
			if (g_hook->objectCmds.size() >= 256) { g_hook->objectCmds.erase(g_hook->objectCmds.begin()); }	// the thread never got to them: keep the newest
			g_hook->objectCmds.push_back(std::move(c));
		}
		g_hook->cv.notify_all();
	}
}

void audiohook_object(uint32_t object, const double pos[3])
{
	if (!g_hook || !object) { return; }
	std::lock_guard<std::mutex> g(g_hook->m);
	g_hook->objects[object] = { pos[0], pos[1], pos[2] };
}

void audiohook_object_remove(uint32_t object)
{
	if (!g_hook || !object) { return; }
	{
		std::lock_guard<std::mutex> g(g_hook->m);
		g_hook->objects.erase(object);
	}
	objectCmd({ Hook::ObjectCmd::kRemove, object, {}, {}, 0 });
}

bool audiohook_trace() { return kAudioTrace.get(); }

void audiohook_object_switch(uint32_t object, const std::string& group, const std::string& value)
{
	objectCmd({ Hook::ObjectCmd::kSwitch, object, group, value, 0 });
}

void audiohook_indoors(bool inside, bool big, const std::string& material, float integrity)
{
	if (!g_hook) { return; }
	std::lock_guard<std::mutex> g(g_hook->m);
	g_hook->indoor = { inside, big, material, integrity };
}

void audiohook_object_post(uint32_t object, const std::string& event, float gate)
{
	objectCmd({ Hook::ObjectCmd::kPost, object, event, {}, gate });
}

void audiohook_shutdown()
{
	if (!g_hook) { return; }
	{
		std::lock_guard<std::mutex> g(g_hook->m);
		g_hook->quit = true;
	}
	g_hook->cv.notify_all();
	g_hook->thread.join();
	g_hook.reset();
}
