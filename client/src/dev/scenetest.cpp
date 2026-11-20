// The scene test (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7): client/scenes.txt, its loading and applying, the
// --scene-test run (a shot per scene with NAME.json: the load wait, the settle settle, the GPU and pass means, the counts) and the per-scene
// entries the debug panel and the `set scene` script op use. Moved off main.cpp; everything it needs of the game arrives through dev::Context.
// Built only with KCLIENT_DEV_TOOLS.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "kutil/executable.hpp"
#include "app/logicthread.hpp"
#include "characters/characters.hpp"
#include "app/config.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "dev/scenehost.hpp"
#include "render/drawstats.hpp"
#include "world/foliage.hpp"
#include "render/framestats.hpp"
#include "world/grass.hpp"
#include "render/gputimer.hpp"
#include "render/hdr.hpp"
#include "kworld/heightmap.hpp"
#include "render/particles.hpp"
#include "render/readback.hpp"
#include "assets/resources.hpp"	// lower()
#include "bridge/simview.hpp"
#include "render/sky.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "platform/windowgate.hpp"
#include "world/worldbuildings.hpp"

namespace dev
{
namespace
{

// The time fed to animations is fixed so the shots do not depend on how many frames a scene took to load.
constexpr double kSceneTestTime = 10.0;
constexpr int kSceneMaxFrames = 1500;	// loads still pending after this many frames = load failure
constexpr float kFovY = 45.0f * 3.14159265f / 180.0f;	// main.cpp's kFovY, the world camera's vertical field of view (the JSON's fov_y_deg / fov_x_deg)

// A named camera bookmark (client/scenes.txt, format in README.md).
struct Scene
{
	std::string name, weather = "default", ref;
	float fogDist = 0;		// fogdist=UNITS: force the global fog to density 1 / UNITS (a live reference whose weather fog distance is not reproduced), 0 = the weather's own
	double wind = -1;	// wind=DEG: the wind blows toward this yaw (character yaw, 0 = +Z, 90 = +X) for wind-aimed parts; -1 = the live wind
	float clouds = -1;		// clouds=D: force the cloud density 0..1 (a capture taken while the game's clouds had not reached the record's density; kenshi-k16.2.67), -1 = the weather's own; not with weather=auto
	float rain = -1, dust = -1;	// rain=A / dust=A: force a particle strength 0..1 (rain replaces the weather's own rain; dust adds a Sand-Stream around the camera), -1 = the weather decides
	std::string keys;		// R / F presses (building floor up / down) sent once everything has loaded, before the shot
	std::vector<std::string> chars;	// char=SPEC (repeatable): --character specs placed from the scene camera
	std::vector<std::string> sets;	// set=KEY=VALUE (repeatable): options for this scene only (cfg::set at its start, put back at the next scene's)
	std::vector<std::string> blood;	// blood=SPEC (repeatable): --blood specs placed from the scene camera
	float build = -1;		// build=P: construction progress 0..1 of the building nearest the look point (else the camera), set once loaded
	double pos[3] = {}, yaw = 0, pitch = -30, time = 12;
	int day = 1;
	bool hasLook = false, hasAlt = false;	// hasAlt: pos is x,z and the height above ground comes from alt
	double look[3] = {}, alt = 0;
};

struct SceneTest
{
	std::vector<Scene> list;
	size_t idx = 0;
	int since = -1;		// frames since the current scene was applied (-1 = not applied yet)
	int sgBase = 0;
	bool keysDone = false;	// the scene's keys= presses were sent
	bool waiting = false;	// a done the test advances on (the floor step, the shot) is not back yet: no frame counts until it is (kenshi-k16.2.205.12.24)
	double gpuSum[GpuTimer::kSections] = {};	// per-section GPU ms summed over the measured frames (frames >= 2 after the jump)
	int gpuN = 0;
#if KCLIENT_PROFILE
	// per-pass submit counters summed over the same frames (kenshi-k16.2.101.1: draws, instances, triangles, pipeline and binding changes)
	std::uint64_t passSum[GpuTimer::kSections][5] = {};
	void passAdd(const drawstats::Pass& p, int k)
	{
		passSum[k][0] += p.draws; passSum[k][1] += p.instances; passSum[k][2] += p.triangles; passSum[k][3] += p.pipelines; passSum[k][4] += p.bindings;
	}
	void passClear() { for (auto& row : passSum) { for (std::uint64_t& v : row) { v = 0; } } }
#endif
	std::chrono::steady_clock::time_point t0;
};

// The state (this file alone sees it).
struct SceneState
{
	std::string test;	// --scene-test OUTDIR
	std::string filter;	// --scenes NAME,NAME
	std::string file;	// --scene-file FILE (default: client/scenes.txt next to the build dir)
	bool hud = false;	// --scene-hud: scene shots keep the debug text and minimap (default: the world only; the numbers are in NAME.json)
	int frames = 3;		// --scene-frames N: frames after the jump before a shot (more = steadier gpu_* means)
	std::vector<Scene> scenes;
	SceneTest st;
	std::vector<std::string> saved;	// KEY=VALUE of the options the current scene's set= changed
};
SceneState& sc() { static SceneState s; return s; }

bool load_scenes(const std::string& path, std::vector<Scene>& out)
{
	std::ifstream in(path);
	if (!in) { return false; }
	std::string line;
	for (int ln = 1; std::getline(in, line); ln++)
	{
		line = line.substr(0, line.find('#'));
		std::vector<std::string> f;
		std::istringstream ss(line);
		for (std::string w; std::getline(ss, w, '\t');)
		{
			w.erase(0, w.find_first_not_of(" \r"));
			w.erase(w.find_last_not_of(" \r") + 1);
			if (!w.empty()) { f.push_back(w); }
		}
		if (f.empty()) { continue; }
		Scene s;
		s.name = f[0];
		for (size_t i = 1; i < f.size(); i++)
		{
			const size_t eq = f[i].find('=');
			const std::string k = f[i].substr(0, eq), v = eq == std::string::npos ? "" : f[i].substr(eq + 1);
			int h, m;
			bool ok = true;
			if (k == "pos") { s.hasAlt = sscanf(v.c_str(), "%lf,%lf,%lf", &s.pos[0], &s.pos[1], &s.pos[2]) == 2; ok = s.hasAlt || (sscanf(v.c_str(), "%lf,%lf,%lf", &s.pos[0], &s.pos[1], &s.pos[2]) == 3); }
			else if (k == "alt") { s.alt = atof(v.c_str()); }
			else if (k == "look") { ok = sscanf(v.c_str(), "%lf,%lf,%lf", &s.look[0], &s.look[1], &s.look[2]) == 3; s.hasLook = ok; }
			else if (k == "yaw") { s.yaw = atof(v.c_str()); }
			else if (k == "pitch") { s.pitch = atof(v.c_str()); }
			else if (k == "day") { s.day = atoi(v.c_str()); }
			else if (k == "time") { s.time = sscanf(v.c_str(), "%d:%d", &h, &m) == 2 ? h + m / 60.0 : atof(v.c_str()); }
			else if (k == "weather") { s.weather = v; }
			else if (k == "fogdist") { s.fogDist = (float)atof(v.c_str()); }
			else if (k == "rain") { s.rain = (float)atof(v.c_str()); }
			else if (k == "wind") { s.wind = atof(v.c_str()); ok = s.wind >= 0 && s.wind < 360; }
			else if (k == "dust") { s.dust = (float)atof(v.c_str()); }
			else if (k == "clouds") { s.clouds = (float)atof(v.c_str()); ok = s.clouds >= 0 && s.clouds <= 1; }
			else if (k == "ref")
			{
				// a bare name resolves against $KCLIENT_SCENE_REFS (public checkouts have none: no reference, not an error)
				const char* dir = getenv("KCLIENT_SCENE_REFS");
				s.ref = v.find('/') != std::string::npos ? v : dir && *dir ? std::string(dir) + "/" + v : "";
				if (s.ref.empty() || !std::filesystem::exists(s.ref)) { printf("scene %s: no reference (%s)\n", s.name.c_str(), v.c_str()); s.ref.clear(); }
			}
			else if (k == "keys") { s.keys = v; ok = v.find_first_not_of("RF") == std::string::npos; }
			else if (k == "char") { s.chars.push_back(v); }
			else if (k == "set") { s.sets.push_back(v); }
			else if (k == "blood") { s.blood.push_back(v); }
			else if (k == "build") { s.build = (float)atof(v.c_str()); ok = s.build >= 0 && s.build <= 1; }
			else { ok = false; }
			if (!ok) { fprintf(stderr, "%s:%d: bad field '%s'\n", path.c_str(), ln, f[i].c_str()); return false; }
		}
		if (s.clouds >= 0 && lower(s.weather) == "auto") { fprintf(stderr, "%s:%d: clouds= needs a named weather (the regional blend is recomputed every frame)\n", path.c_str(), ln); return false; }
		out.push_back(s);
	}
	return true;
}

// Default scenes file: client/scenes.txt, found from the executable (client/build/kclient).
std::string default_scene_file()
{
	std::error_code ec;
	return (kutil::executablePath(ec).parent_path().parent_path() / "scenes.txt").string();
}

void apply_scene(const Context& cx, const Scene& s)
{
	for (int k = 0; k < 3; k++) { cx.pos[k] = s.pos[k]; }
	if (s.hasAlt) { cx.pos[1] = cx.hm->height(s.pos[0], s.pos[1]) + s.alt; cx.pos[2] = s.pos[1]; }
	if (s.hasLook)
	{
		const double dx = s.look[0] - s.pos[0], dy = s.look[1] - s.pos[1], dz = s.look[2] - s.pos[2];
		cx.yaw = std::atan2(dx, -dz);
		cx.pitch = std::atan2(dy, std::hypot(dx, dz));
	}
	else
	{
		cx.yaw = s.yaw * 3.14159265358979 / 180.0;
		cx.pitch = s.pitch * 3.14159265358979 / 180.0;
	}
	cx.walk = false;
	cx.sky.clock.setHours(s.time);
	cx.sky.clock.day = s.day - 1;
	cx.sceneName = s.name;
	// set=KEY=VALUE: the previous scene's options go back, then this scene's apply (every other scene runs on the options it always did)
	SceneState& t = sc();
	for (const std::string& kv : t.saved) { cfg::set(kv); }
	t.saved.clear();
	for (const std::string& kv : s.sets)
	{
		const std::string key = kv.substr(0, kv.find('='));
		for (const cfg::Info& i : cfg::list()) { if (i.key == key) { t.saved.push_back(key + "=" + i.value); break; } }
		if (!cfg::set(kv)) { fprintf(stderr, "kclient: scene %s: bad set=%s\n", s.name.c_str(), kv.c_str()); }
	}
	cx.setWeather(s.weather);
	cx.sky.fogRhoForce = s.fogDist > 0 ? 1.0f / s.fogDist : 0.0f;
	cx.rainForce = s.rain;
	cx.windForce = s.wind;
	cx.dustForce = s.dust;
	cx.cloudsForce = s.clouds;
	cx.particles.reset(1);	// the same seed and pre-warm for every scene shot
	if (cx.charsReady && !cx.sim.active())	// with a sim the characters are its (clearing them left SimView's indices dangling: Ctrl+1 crashed, user 2026-11-05)
	{
		cx.chars.clear();
		cx.placeCharacters(s.chars, cx.pos[0], cx.pos[2], cx.yaw);
	}
	cx.blood.clear();
	cx.placeBlood(s.blood, cx.pos[0], cx.pos[2], cx.yaw);
}

}	// namespace

bool sceneArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff)
{
	SceneState& s = sc();
	const std::string a = argv[i];
	const bool more = i + 1 < argc;
	if (a == "--scene-test" && more) { s.test = argv[++i]; windowOff.push_back("--scene-test " + s.test); return true; }
	if (a == "--scene-hud") { s.hud = true; return true; }
	if (a == "--scene-frames" && more) { s.frames = atoi(argv[++i]); return true; }
	if (a == "--scenes" && more) { s.filter = argv[++i]; windowOff.push_back("--scenes " + s.filter); return true; }
	if (a == "--scene-file" && more) { s.file = argv[++i]; windowOff.push_back("--scene-file " + s.file); return true; }
	return false;
}

bool sceneTesting() { return !sc().test.empty(); }

bool scenesNeedCharacters()
{
	for (const Scene& s : sc().scenes) { if (!s.chars.empty()) { return true; } }
	return false;
}

std::vector<std::string> sceneNames()
{
	std::vector<std::string> names;
	for (const Scene& s : sc().scenes) { names.push_back(s.name); }
	return names;
}

bool sceneSetName(const Context& cx, const std::string& name)
{
	SceneState& s = sc();
	const auto it = std::find_if(s.scenes.begin(), s.scenes.end(), [&](const Scene& sc) { return sc.name == name; });
	if (it == s.scenes.end() || cx.sim.active()) { return false; }
	apply_scene(cx, *it);
	return true;
}

double sceneSimTime(double animTime) { return sc().test.empty() ? animTime : kSceneTestTime; }

void sceneSetup(bool debug)
{
	SceneState& s = sc();
	if (!debug)	// the scenes are a development feature: a release build loads none
	{
		if (!s.test.empty() || !s.filter.empty() || !s.file.empty())
		{
			fprintf(stderr, "kclient: --scene-test, --scenes and --scene-file need --debug (the scenes are a development feature)\n");
			exit(2);
		}
		return;
	}
	if (s.file.empty()) { s.file = default_scene_file(); }
	if (!load_scenes(s.file, s.scenes) && (!s.test.empty() || std::filesystem::exists(s.file)))
	{
		fprintf(stderr, "kclient: cannot read scenes file %s\n", s.file.c_str());
		exit(2);
	}
	if (s.test.empty()) { return; }
	std::filesystem::create_directories(s.test);
	std::stringstream names(s.filter);
	std::vector<std::string> want;
	for (std::string n; std::getline(names, n, ',');) { want.push_back(n); }
	for (const Scene& scene : s.scenes)
	{
		if (want.empty() || std::find(want.begin(), want.end(), scene.name) != want.end()) { s.st.list.push_back(scene); }
	}
	for (const std::string& n : want)
	{
		if (std::none_of(s.scenes.begin(), s.scenes.end(), [&](const Scene& scene) { return scene.name == n; })) { fprintf(stderr, "kclient: no scene '%s' in %s\n", n.c_str(), s.file.c_str()); exit(2); }
	}
	if (s.st.list.empty()) { fprintf(stderr, "kclient: no scenes to test\n"); exit(2); }
}

void sceneTestPre(const Context& cx)
{
	SceneTest& st = sc().st;
	if (st.since >= 0 || st.idx >= st.list.size()) { return; }
	apply_scene(cx, st.list[st.idx]);
	cx.hideUi = !sc().hud;
	logicthread::renderIdle();	// resetTurns writes what the draw reads (overlap: render is idle at the packet boundary; free elsewhere; kenshi-k16.2.205.12.108)
	cx.buildings.resetFloors();
	cx.buildings.resetTurns();
	cx.buildings.clearConstruction();
	st.keysDone = false;
	st.since = 0;
	st.gpuN = 0;
	for (double& v : st.gpuSum) { v = 0; }
#if KCLIENT_PROFILE
	st.passClear();
#endif
	st.sgBase = cx.sgErrors;
	st.t0 = std::chrono::steady_clock::now();
}

// Once nothing is loading, writes NAME.png and NAME.json and moves to the next scene.
// After the frame is built (logic_tail; kenshi-k16.2.205.12.23): the decision is logic's (nothing loading: the pending counts are written by
// the logic half's update and prepare calls only), and what the draw produced is read once the frame's last pass has ended, as readback
// requests: the GPU and pass counters (readback::after), the floor step that must not show before the next frame, the shot and its JSON.
void sceneTestPost(const Context& cx)
{
	SceneTest& st = sc().st;
	if (st.since < 0 || st.waiting) { return; }	// waiting: the step or the shot this test asked for has not had its done (readback::runDone) yet
	const bool loading = cx.worldLoading(nullptr);
	const bool timeout = ++st.since >= kSceneMaxFrames;
	if (st.since >= 2)
	{
		readback::after([](bool) {	// this frame's stats slot, as the draw left it
		SceneTest& st = sc().st;
		for (int k = 0; k < GpuTimer::kSections; k++)
		{
			st.gpuSum[k] += framestats::stats().gpuMs[k];
#if KCLIENT_PROFILE
			st.passAdd(framestats::stats().passes[k], k);
#endif
		}
		st.gpuN++;
		});
	}
	if ((loading || st.since < sc().frames) && !timeout) { return; }
	const Scene& scene = st.list[st.idx];
	if (!st.keysDone && (!scene.keys.empty() || scene.build >= 0))	// the floor focus exists only after a frame at the scene's position: step now, then give the change frames to load and render
	{
		st.keysDone = true;
		st.since = 0;
		st.waiting = true;
		readback::after([&cx, scene](bool) {	// after this frame's draw: the frame being built shows the floors as they were
		sc().st.waiting = false;
		for (const char k : scene.keys) { cx.buildings.stepFloor(k == 'R' ? 1 : -1); }
		if (scene.build >= 0)
		{
			const int b = cx.buildings.nearestBuilding(scene.hasLook ? scene.look[0] : cx.pos[0], scene.hasLook ? scene.look[2] : cx.pos[2]);
			if (b >= 0) { cx.buildings.setConstruction((uint32_t)b, scene.build); printf("scene %s: build=%.2f on %s\n", scene.name.c_str(), scene.build, cx.buildings.describe((uint32_t)b).c_str()); }
			else { fprintf(stderr, "scene %s: build=: no building within %.0f\n", scene.name.c_str(), WorldBuildings::kFocusRange); }
		}
		});
		return;
	}
	const std::string base = sc().test + "/" + scene.name;
	st.waiting = true;
	cx.requestHdrRead();	// the JSON's hdr_scene_lum / hdr_adapted, read on the render side
	readback::screenshot(base + ".png", [&cx, base, scene, loading](bool) {
	SceneTest& st = sc().st;
	st.waiting = false;
	const WorldBuildings::Stats bs = cx.buildings.stats();	// the draw's counts (drawn, draws): read after the frame's last pass
	const Foliage::Stats fs = cx.foliage.stats();
	const Grass::Stats gs = cx.grass.stats();
	std::vector<Terrain::WaterChunk> water;
	cx.terrain.waterChunks(Water::kLevel, water);
	float hb[4] = {};
	if (!cx.hideUi) { cx.hudBox(hb); }
	const int sgErrors = cx.sgErrors - st.sgBase;
	const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st.t0).count();
	// section means over the measured frames (the last frame's value when none were measured)
	const auto gpuMean = [](int k) { const SceneTest& t = sc().st; return t.gpuN ? t.gpuSum[k] / t.gpuN : framestats::stats().gpuMs[k]; };	// no capture: inside a readback done
	double gpuTotal = 0;
	for (int k = 0; k < GpuTimer::kSections; k++) { gpuTotal += gpuMean(k); }
	char json[4096];
	snprintf(json, sizeof json,
		"{\n"
		"\t\"scene\": \"%s\",\n"
		"\t\"weather\": \"%s\",\n"
		"\t\"clouds\": %.3f,\n"
		"\t\"camera\": [%.1f, %.1f, %.1f],\n\t\"yaw_deg\": %.2f,\n\t\"pitch_deg\": %.2f,\n\t\"day\": %d,\n\t\"time\": %.4f,\n\t\"sun_altitude_deg\": %.2f,\n\t\"sun_dir\": [%.5f, %.5f, %.5f],\n\t\"fov_y_deg\": %.2f,\n\t\"fov_x_deg\": %.2f,\n\t\"viewport\": [%d, %d],\n"
		"\t\"buildings_drawn\": %zu,\n\t\"buildings_resident\": %zu,\n"
		"\t\"features_drawn\": %zu,\n\t\"interiors_drawn\": %zu,\n"
		"\t\"foliage_drawn\": %zu,\n\t\"foliage_resident\": %zu,\n\t\"foliage_cells\": %zu,\n"
		"\t\"water_chunks_drawn\": %zu,\n"
		"\t\"terrain_chunks_drawn\": %d,\n\t\"terrain_chunks_resident\": %zu,\n"
		"\t\"cpu_ms\": %.2f,\n"
		"\t\"shadow_cpu_ms\": %.2f,\n"
		"\t\"foliage_draws\": %zu,\n"
		"\t\"grass_drawn\": %zu,\n\t\"grass_resident\": %zu,\n\t\"grass_draws\": %zu,\n"
		"\t\"gpu_ms\": %.2f,\n"
		"\t\"gpu_shadow_ms\": %.2f,\n\t\"gpu_sky_ms\": %.2f,\n\t\"gpu_terrain_ms\": %.2f,\n\t\"gpu_buildings_ms\": %.2f,\n\t\"gpu_foliage_ms\": %.2f,\n\t\"gpu_grass_ms\": %.2f,\n\t\"gpu_water_ms\": %.2f,\n\t\"gpu_overlay_ms\": %.2f,\n\t\"gpu_exposure_ms\": %.2f,\n\t\"gpu_fog_ms\": %.2f,\n\t\"gpu_shafts_ms\": %.2f,\n\t\"gpu_particles_ms\": %.2f,\n\t\"gpu_ao_ms\": %.2f,\n\t\"gpu_post_ms\": %.2f,\n\t\"gpu_reflection_ms\": %.2f,\n"
#if KCLIENT_PROFILE
		"\t\"gpu_characters_ms\": %.2f,\n\t\"gpu_character_shadows_ms\": %.2f,\n\t\"gpu_items_ms\": %.2f,\n\t\"gpu_ui_ms\": %.2f,\n"
#endif
		"\t\"fog_volumes_drawn\": %d,\n\t\"particles_drawn\": %d,\n\t\"hdr_scene_lum\": %.4f,\n\t\"hdr_adapted\": %.4f,\n\t\"exposure\": %.4f,\n"
		"\t\"load_wait_ms\": %.0f,\n"
		"\t\"load_frames\": %d,\n"
		"\t\"load_failed\": %s,\n"
		"\t\"meshes_failed\": %zu,\n\t\"textures_failed\": %zu,\n"
		"\t\"sg_errors\": %d,\n"
		"\t\"hud_box\": [%.0f, %.0f, %.0f, %.0f]\n"
		"}\n",
		scene.name.c_str(), cx.sky.weather.name.c_str(), cx.sky.weather.cloudsDensity, cx.pos[0], cx.pos[1], cx.pos[2], cx.yaw * 180 / 3.14159265358979, cx.pitch * 180 / 3.14159265358979,
		cx.sky.clock.day + 1, cx.sky.clock.hours, std::asin(std::fmin(1.0, std::fmax(-1.0, cx.sky.sunDir()[1]))) * 180 / 3.14159265358979,
		cx.sky.sunDir()[0], cx.sky.sunDir()[1], cx.sky.sunDir()[2], kFovY * 180 / 3.14159265, 2 * std::atan(std::tan(kFovY * 0.5) * view::widthf() / view::heightf()) * 180 / 3.14159265358979,
		view::width(), view::height(), bs.instancesDrawn - bs.featuresDrawn, bs.meshesReady, bs.featuresDrawn, bs.interiorsDrawn,
		fs.instancesDrawn, fs.instancesLoaded, fs.cellsLoaded, water.size(), cx.terrain.drawn(), cx.terrain.resident(),
		framestats::stats().cpu.lastMs, framestats::stats().shadowMs, fs.draws, gs.bladesDrawn, gs.bladesLoaded, gs.draws, gpuTotal, gpuMean(GpuTimer::Shadow), gpuMean(GpuTimer::Sky), gpuMean(GpuTimer::Terrain), gpuMean(GpuTimer::Buildings), gpuMean(GpuTimer::Foliage), gpuMean(GpuTimer::Grass), gpuMean(GpuTimer::Water), gpuMean(GpuTimer::Overlay), gpuMean(GpuTimer::Exposure), gpuMean(GpuTimer::Fog), gpuMean(GpuTimer::Shafts), gpuMean(GpuTimer::Particles), gpuMean(GpuTimer::Ao), gpuMean(GpuTimer::Post), gpuMean(GpuTimer::Reflection),
#if KCLIENT_PROFILE
		gpuMean(GpuTimer::Characters), gpuMean(GpuTimer::CharacterShadows), gpuMean(GpuTimer::Items), gpuMean(GpuTimer::Ui),
#endif
		framestats::stats().fogDrawn, cx.particlesDrawn, cx.hdrSceneLum, cx.hdrAdapted, cx.hdrAdapted > 0 ? 0.55 / cx.hdrAdapted : 0.0, waitMs, st.since, loading ? "true" : "false", bs.meshesFailed, bs.texturesFailed, sgErrors, hb[0], hb[1], hb[2], hb[3]);
	std::string jsonOut = json;
	if (cx.settledChars)	// kenshi-k16.17.7.18: only for scenes that settle their characters, so every other scene's JSON stays as it was
	{
		const Characters::IdleStats is = cx.chars.idleStats();
		char extra[200];
		snprintf(extra, sizeof extra, ",\n\t\"foot_idle\": { \"engaged\": %d, \"feet_ground_delta\": %.3f, \"slope_w\": %.3f }", is.engaged, is.feetDelta, is.slopeW);
		jsonOut.insert(jsonOut.rfind("\n}"), extra);
	}
	#if KCLIENT_PROFILE
	{	// the per-pass submit counters, means over the measured frames (kenshi-k16.2.101.1): draws, instances, triangles, pipeline and binding changes
		std::string prof = ",\n\t\"passes\": {";
		bool first = true;
		for (int k = 0; k < GpuTimer::kSections; k++)
		{
			const std::uint64_t* c = st.passSum[k];
			if (c[0] == 0 && c[3] == 0 && c[4] == 0) { continue; }
			const double n = st.gpuN ? (double)st.gpuN : 1.0;
			char one[220];
			snprintf(one, sizeof one, "%s\n\t\t\"%s\": { \"draws\": %.1f, \"instances\": %.1f, \"triangles\": %.0f, \"pipelines\": %.1f, \"bindings\": %.1f }",
				first ? "" : ",", GpuTimer::name(k), c[0] / n, c[1] / n, c[2] / n, c[3] / n, c[4] / n);
			prof += one;
			first = false;
		}
		if (!first) { prof += "\n\t}"; } else { prof = ",\n\t\"passes\": {}"; }
		jsonOut.insert(jsonOut.rfind("\n}"), prof);
	}
	#endif
	std::ofstream(base + ".json") << jsonOut;
	printf("scene %s: %.0f ms load wait, %d frames, sg errors %d%s\n", scene.name.c_str(), waitMs, st.since, sgErrors, loading ? ", LOAD FAILED" : "");
	if (sgErrors > 0 || loading) { cx.exitCode = 1; }
	st.since = -1;
	if (++st.idx >= st.list.size()) { cx.requestQuit(); }
	});
}

}	// namespace dev
