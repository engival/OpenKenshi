#include <algorithm>
#include "kutil/executable.hpp"
#include "render/lightbake.hpp"
#include "render/drawstats.hpp"
#include "render/framestats.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <unistd.h>
#include <string>
#include <vector>

#include "render/interiorclip.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"	// the frame-buffer calls the dev depth readback needs (animlab / idbuffer do the same)
#endif

#include "world/buildings.hpp"
#include "world/foliage.hpp"
#include "world/grass.hpp"
#include "render/gputimer.hpp"
#include "app/cpustage.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "app/threadsplit.hpp"	// the logic / render split on the stages line (kenshi-k16.2.205.12.84)
#endif
#include "dev/packetdigest.hpp"	// KCLIENT_PACKET_DIGEST / KCLIENT_PACKET_POISON (kenshi-k16.2.205.12.93)
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "assets/sidorder.hpp"
#include "render/fogvolume.hpp"
#include "render/sunshafts.hpp"
#include "render/dust.hpp"
#include "render/blooddecals.hpp"
#include "render/arrows.hpp"
#include "render/particles.hpp"
#include "world/particleworld.hpp"
#include "frame/beamfield.hpp"
#include "characters/physics.hpp"
#include "bridge/ragdolldrive.hpp"
#include "characters/ragdoll.hpp"
#include "render/hdr.hpp"
#include "render/idbuffer.hpp"
#include "render/post.hpp"
#include "render/camera.hpp"
#include "characters/portraits.hpp"
#include "world/doorpose.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/pickmask.hpp"
#endif
#include "knobs/lightterms.hpp"
#include "net/aitask.hpp"
#include "app/githash.hpp"
#include "frame/loadscreen.hpp"
#include "platform/localserver.hpp"
#include "assets/dds.hpp"
#include "assets/ogremesh.hpp"
#include "world/clientnav.hpp"
#include "kworld/towns.hpp"
#include "render/navoverlay.hpp"
#include "app/config.hpp"
#include "platform/datadir.hpp"
#include "platform/windowgeom.hpp"
#include "platform/process.hpp"	// plat::unsetEnv
#include "platform/windowgate.hpp"
#include "render/readback.hpp"
#include "render/renderqueue.hpp"
#include "input/inputqueue.hpp"
#include "app/sappcmd.hpp"
#include "app/logicthread.hpp"
#include "frame/lastframe.hpp"
#include "frame/renderframe.hpp"
#include "app/opencont.hpp"
#include "render/weff.hpp"
#include "net/progressfeed.hpp"
#include "ui/ui.hpp"
#include "ui/gfx.hpp"
#include "ui/hud.hpp"
#include "ui/options.hpp"
#include "ui/gear.hpp"
#include "ui/itemwindows.hpp"
#include "ui/stats.hpp"
#include "ui/keys.hpp"
#include "ui/overview.hpp"
#include "ui/squads.hpp"
#include "ui/research.hpp"
#include "ui/crafting.hpp"
#include "ui/faction.hpp"
#include "ui/progressbars.hpp"
#include "ui/namelabels.hpp"
#include "ui/prompt.hpp"
#include "ui/icons.hpp"
#include "ui/creator.hpp"
#include "ui/menus.hpp"
#include "ui/savepanel.hpp"
#include "ui/newgame.hpp"
#include "ui/advanced.hpp"
#include "ui/tutorials.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "ui/dev/lightspanel.hpp"
#endif
#include "ui/layout.hpp"
#include "ui/prospecting.hpp"
#include "ui/build.hpp"
#include "ui/style.hpp"
#include "app/serverrows.hpp"
#include "ui/trade.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/bubbles.hpp"
#include "ui/dialogue.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/coords.hpp"
#include "assets/materials.hpp"
#include "assets/mods.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "kutil/crashlog.hpp"
#include "app/eventlog.hpp"
#include "world/squads.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ui/dev/debugtree.hpp"
#include "ksim/debugview/enums.hpp"
#endif
#include "characters/characters.hpp"
#include "characters/creator.hpp"
#include "characters/creatorscene.hpp"
#include "kworld/gamedata.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "render/reflection.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"
#include "world/weatherinstances.hpp"
#include "audio/audiohook.hpp"
#include "audio/events.hpp"
#include "bridge/simview.hpp"
#include "frame/trails.hpp"
#include "input/collisionpick.hpp"
#include "input/controlhints.hpp"	// the control-hints types and the resolver stub (kenshi-k16.2.294.2)
#include "input/pick.hpp"	// Pick and the target predicates the hover / click / menu read (kenshi-k16.2.294.2)
#include "input/cursors.hpp"
#include "render/drawlist_gpu.hpp"
#include "render/overlaylist.hpp"
#include "world/worldbuildings.hpp"
#include "world/worldmap.hpp"
#include "host/app.hpp"
#include "host/pick.hpp"
#include "host/orders.hpp"
#include "host/hud.hpp"
#include "host/script.hpp"
#include "host/session.hpp"

namespace host
{

constexpr int kRecordsFrameMs = 16;	// the characters step's panel records per frame (kenshi-k16.2.26): the budget is the loading screen's own frame
const cfg::Bool kStandRayFaces{ cfg::Group{ "Simulation" }, "sim.stand_ray_faces", false, "Standing ray fallback (kenshi-k16.2.344): where no collision shape is under a standing character, the nearest drawn building triangle stands in. Off by default: the first decode of a building mesh costs ~100 ms on the logic thread, and in the Heft crowd it found 70 of 4606 misses" };
const cfg::Bool kStandRay{ cfg::Group{ "Simulation" }, "sim.stand_ray", true, "Standing height (kenshi-k16.2.344): a drawn standing character is placed by a vertical ray from its wire y onto the client collision (nearest hit) and the heightmap; off draws the wire y" };

// Whether this run needs the game data and character assets (kenshi-k16.2.26): the characters step loads them, and the first step starts that load on a
// worker so it overlaps the terrain and map steps instead of blocking the window.
static bool wants_characters()
{
	return !app.opt.characters.empty() || !app.opt.simlink.empty() || app.local || !app.opt.creator.empty()
		|| dev::scenesNeedCharacters()	// a scene places char= specs (dev/scenetest.cpp)
		|| dev::labScripted();	// a headless animation lab run (the `lab` script op): no world, no characters in it
}

void init()
{
	(void)dev_context();	// filled here, on the main thread, before any frame (its first use is on the logic side)
	sg_desc sd = {};
	sd.environment = sglue_environment();
	app.swapFormat = sd.environment.defaults;
	if (!dev::viewing())	// every scene pipeline renders into the HDR target (hdr.hpp); the overlay below keeps the swapchain format
	{
		sd.environment.defaults.color_format = SG_PIXELFORMAT_RGBA16F;
		sd.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;	// the HDR target's depth image (hdr.cpp, reflection.cpp): a pipeline that leaves .depth.pixel_format alone must match it, not the swapchain's (validation: VALIDATE_APIP_DEPTHSTENCILATTACHMENT_FORMAT)
	}
	sd.logger.func = log_cb;
	sd.image_pool_size = sd.view_pool_size = (int)Terrain::kMaxResident + 512 + 2048;	// one image + view per resident chunk
	sd.buffer_pool_size = 8192;
	sd.shader_pool_size = 128;
	sd.pipeline_pool_size = 256;
	sg_setup(&sd);
	dev::sgOwnerInstall();	// dev builds: every sg_* call off this thread aborts (dev/sgowner.cpp)
	{
		// shaders/origin.inc flips an offscreen read where sokol-shdc does not define SOKOL_GLSL (every target but GL); the backend must say the same
#if defined(SOKOL_GLCORE)
		const bool glslOrigin = false;	// GL: bottom left
#else
		const bool glslOrigin = true;
#endif
		if (sg_query_features().origin_top_left != glslOrigin) { fprintf(stderr, "kclient: the texture origin sokol reports (top left: %d) is not the one shaders/origin.inc compiled for (%d): offscreen reads are upside down\n", (int)sg_query_features().origin_top_left, (int)glslOrigin); }
	}
#if defined(SOKOL_GLCORE)
	printf("GL_RENDERER: %s | GL_VENDOR: %s | GL_VERSION: %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_VERSION));
#endif
	GPU_INIT(app.gpu);
	if (dev::viewing())
	{
		sgl_desc_t gd = {};
		gd.logger.func = slog_func;
		sgl_setup(&gd);
		sdtx_desc_t td = {};
		td.fonts[0] = sdtx_font_kc853();
		td.logger.func = slog_func;
		sdtx_setup(&td);
		init_view();
		return;
	}
	sgl_desc_t gd = {};
	gd.logger.func = slog_func;
	gd.color_format = app.swapFormat.color_format;
	gd.depth_format = app.swapFormat.depth_format;
	gd.sample_count = app.swapFormat.sample_count;
	// The default context: the HUD, minimap, markers, navmesh and the windows (the trails have their own, k16.19.6.1). 262144, not sokol's 65536: the
	// navmesh overlay alone may take 56,000 (its cap), and the windows must keep room beside it (client lead, k16.19.6.1 review).
	gd.max_vertices = 1 << 18;
	sgl_setup(&gd);
	// The debug trails' own sgl context: a full history can no longer fill the default context and silently drop the HUD and windows with it (k16.19.6).
	app.trails.initSgl(app.swapFormat.color_format, app.swapFormat.depth_format, app.swapFormat.sample_count);
	sdtx_desc_t td = {};
	td.fonts[0] = sdtx_font_kc853();
	td.context.color_format = app.swapFormat.color_format;
	td.context.depth_format = app.swapFormat.depth_format;
	td.context.sample_count = app.swapFormat.sample_count;
	td.context.char_buf_size = 16384;	// the HUD lines plus the F1 key list (every bound action) overflow the default 4096
	td.logger.func = slog_func;
	sdtx_setup(&td);
	init_ui();
	// The rest of the startup is a list of steps (a step returns false to be retried next frame). Scripted runs (screenshots, scenes) run them all here;
	// interactive runs run one per frame behind the loading screen (frame_loading), so the window presents and stays responsive.
	app.loadSteps = {
		{ "graphics: HDR target, fog volumes, shadows", []()
		{
			app.detailFut = std::async(std::launch::async, [] { return app.terrain.detail.prepare(app.opt.data); });	// the slowest part of startup (CPU only), overlapped with the steps below
			if (wants_characters()) { app.charsFut = std::async(std::launch::async, [] { return kworld::loadMergedGameData(app.opt.data); }); }	// the characters step's CPU work (kenshi-k16.2.26): overlaps these steps, never a frame
			if (!app.opt.simlink.empty()) { app.hashFut = std::async(std::launch::async, [] { return kworld::dataHash(kworld::resolveLoadOrder(app.opt.data)); }); }	// the simlink hello's data hash (kenshi-k16.2.26)
			app.hdr.init(app.swapFormat);
			app.refl.init();
			app.hdr.loadHaze(app.opt.data);
			app.post.init(app.swapFormat);
			interiorclip::init();
			app.lightVolumes.init();
			app.sunShafts.init();
			app.dust.init();
			if (app.opt.fogVolumes && app.fogvol.load(app.opt.data)) { app.fogvol.init(); }
			else { app.fogvol.enabled = false; }
			if (app.opt.particles) { app.particles.init(app.opt.data); }
			app.blood.init(app.opt.data);
			app.portraits.init();
			app.portraitTable.setOrigin(sg_query_features().origin_top_left);	// the uv's row order, read here once (logic asks sokol nothing)
			if (app.portraits.atlas().id != SG_INVALID_ID)	// the UI's id for the atlas, bound here at its creation (the portraits' refs carry it)
			{
				app.portraitsTex = dl::newTex();
				dl::gpu::bind(app.portraitsTex, app.portraits.atlas(), app.portraits.sampler());
			}
			app.terrain.blood = &app.blood;
			app.shadow.init();
			app.shadow.enabled = app.opt.shadows;
			app.shadow.stagger = app.opt.shadowStagger >= 0 ? app.opt.shadowStagger > 0 : !app.scripted();
			app.terrain.shadow = &app.shadow;
			app.buildings.shadow = &app.shadow;
			app.chars.shadow = &app.shadow;	// with the others, not in the lazy characters step: a run without characters still calls chars.render (k16.2.281)
			return true;
		} },
		{ "terrain: biome map", []()
		{
			if (!app.terrain.init(&*app.hm, app.opt.data + "/newland/land/biomemap.png"))
			{
				fprintf(stderr, "kclient: cannot load biomemap.png under %s\n", app.opt.data.c_str());
				exit(1);
			}
			return true;
		} },
		{ "terrain: colour map", []()
		{
			static std::future<bool> fut;	// the cache read (and a first-run build) on a worker: the step polls it (kenshi-k16.2.26)
			if (!fut.valid()) { fut = std::async(std::launch::async, [] { return app.terrain.colour.load(app.opt.data + "/newland/land/overlaymaps"); }); }
			if (!app.scripted() && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
			app.terrain.initColour(fut.get());
			if (app.terrain.hasColour)
			{
				const GroundColour& c = app.terrain.colour;
				printf("colour: %dx%d BC1 uploaded, cache %.1f MiB%s\n", c.uploadedSize, c.uploadedSize, c.cacheBytes / 1048576.0,
					c.buildSeconds > 0 ? (" (built in " + std::to_string(c.buildSeconds) + " s)").c_str() : " (existing)");
			}
			return true;
		} },
		{ "world map image", []()
		{
			if (!app.wmap.init(app.opt.map))
			{
				fprintf(stderr, "kclient: cannot load map image %s (need 8192x8192 DXT1 DDS)\n", app.opt.map.c_str());
				exit(1);
			}
			return true;
		} },
		{ "squads, towns, sky constants", []()
		{
			if (app.opt.squads.empty())
			{
				std::error_code ec;
				app.opt.squads = (kutil::executablePath(ec).parent_path().parent_path().parent_path() / "cpp/build/default_world_squads.json").string();
			}
			app.squads.load(app.opt.squads, *app.hm);
			app.squads.init();
			app.townFut = std::async(std::launch::async, TownSet::load, app.opt.data);
			app.skyFut = std::async(std::launch::async, SkyConstants::load, app.opt.data);
			return true;
		} },
		{ "sky and weather", []()
		{
			const std::string root = std::filesystem::path(app.opt.data).parent_path().string();
			if (!app.res.init(root)) { fprintf(stderr, "kclient: no resources.cfg under %s\n", root.c_str()); exit(1); }
			app.sky.init(app.res);
			{
				std::string err;
				if (!app.creatorScene3d.load(app.opt.data, err)) { fprintf(stderr, "kclient: creator scene: %s\n", err.c_str()); exit(1); }
			}
			app.wx.init();
			if (!spec_cube_init(app.opt.data)) { fprintf(stderr, "kclient: mp_specularity.dds not usable under %s/materials: no environment specular\n", app.opt.data.c_str()); }
			app.atlasFut = std::async(std::launch::async, WeatherAtlas::load, app.opt.data);
			audiohook_init(app.opt.data);
			return true;
		} },
		{ "water", []()
		{
			if (app.opt.water)
			{
				static std::future<Water::Prepared> fut;	// the PNG decodes on a worker (kenshi-k16.2.26: 2048^2 flowmap, ~40 ms); the uploads are here
				if (!fut.valid()) { fut = std::async(std::launch::async, [] { return Water::prepare(app.opt.data); }); }
				if (!app.scripted() && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
				if (app.water.init(app.opt.data, fut.get())) { app.water.flat = app.opt.waterFlat; }
				else { app.opt.water = false; }
			}
			else { app.opt.water = false; }
			app.sky.clock.setHours(app.opt.time);
			app.sky.clock.setSpeed(app.opt.timescale);
			app.sky.clock.day = app.opt.day - 1;
			app.sky.setSkyHours(app.opt.skyHours);
			app.weatherAuto = app.opt.weather.empty() || lower(app.opt.weather) == "auto";
			if (!app.weatherAuto)
			{
				app.weatherFut = std::async(std::launch::async, [](std::string data, std::string name)
				{
					SkyWeather w;
					w.load(data, name);
					return w;
				}, app.opt.data, app.opt.weather);
			}
			return true;
		} },
		{ "terrain: detail textures", []()
		{
			static TerrainDetail::Prepared prepared;
			static int part = 0;	// one upload part a frame (kenshi-k16.2.26): the window keeps redrawing during the big uploads
			if (part == 0)
			{
				if (!app.scripted() && app.detailFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }	// still reading on the worker
				prepared = app.detailFut.get();
			}
			if (app.terrain.initDetailPart(prepared, part) > 0) { part++; return false; }
			if (app.terrain.detail.ok)
			{
				const TerrainDetail& d = app.terrain.detail;
				printf("terrain detail: %d biomes, %d diffuse + %d normal layers (%d^2), overlay %d^2, loaded in %.2f s\n", d.biomeCount, d.layersDiffuse, d.layersNormal, TerrainDetail::kLayerSize, TerrainDetail::kOverlaySize, d.loadSeconds);
			}
			return true;
		} },
		{ "buildings and materials", []()
		{
			// the stealth arrows' mesh (kenshi-h2v.27.2): the game's own data/meshes/arrow.mesh, loaded once; a failure is one stderr line and no
			// arrows (never a substitute shape)
			{
				std::string err;
				const std::string at = app.res.locate("data/meshes/arrow.mesh");
				const std::string file = at.empty() ? app.res.find("arrow.mesh") : at;
				if (file.empty() || !app.arrows.init(file, err)) { fprintf(stderr, "kclient: stealth arrows: %s\n", file.empty() ? "arrow.mesh not found" : err.c_str()); }
			}
			if (app.opt.bradius > 0 || app.opt.foliage)
			{
				app.mats.load(app.res);
				app.buildings.init(&app.res, &app.mats);
				app.buildings.radius = app.opt.bradius;
				app.buildings.offsetFrame = app.opt.offsetFrame;
				app.buildings.hideMarkers = app.opt.hideMarkers;
				app.buildings.interiors = app.opt.interiors;
				app.buildings.setGround(app.terrain.colour.view, app.terrain.colour.smp, app.terrain.colour.alphaView);
				app.buildings.detail = &app.terrain.detail;
				if (app.opt.bradius > 0) { app.bldFut = std::async(std::launch::async, BuildingSet::load, app.opt.data, app.opt.features); }
				else { app.buildings.setData(BuildingSet()); app.bldSet = true; }
			}
			return true;
		} },
		{ "foliage and grass", []()
		{
			if (app.opt.foliage)
			{
				app.foliage.radius = app.opt.fradius;
				app.foliage.foliageRange = app.opt.foliageRange;
				app.foliage.init(&app.buildings, app.opt.data);
				app.foliageOn = true;
				if (app.opt.grass)
				{
					app.grass.rangeScale = app.opt.grassRange;
					app.grass.density = app.opt.grassDensity;
					app.grass.paintMap = app.opt.grassPaint;
					app.grass.shadow = &app.shadow;
					app.grass.init(&app.res, &app.foliage, app.opt.data);
					app.grassOn = true;
				}
			}
			return true;
		} },
		{ "characters", []()
		{
			if (wants_characters())
			{
				if (!init_characters() && app.charsPhase < 2) { return false; }	// a phase still to run: the step is retried next frame (kenshi-k16.2.26); 2 = done or failed
				if (app.charsReady) { place_characters(app.opt.characters, app.opt.x, app.opt.z, app.opt.yaw * M_PI / 180.0); }
			}
			if (!app.opt.creator.empty() && app.charsReady) { open_creator(app.opt.creator); }
			place_blood(app.opt.blood, app.opt.x, app.opt.z, app.opt.yaw * M_PI / 180.0);
			std::string err;
			app.sim.startTick = app.opt.simlinkTick;
			if (!app.opt.simlink.empty() && app.charsReady)
			{
				if (!app.hashErr.empty()) { err = "data hash: " + app.hashErr; }
				else
				{
					// kenshi-k16.2.268: a scripted run's first world frame blocks on these futures (.get(), 9..12 s under load) before its first pump, and the
					// server ends a session whose HELLO is not in 10 s of the connect. Wait for them here, before the connect, so the timer starts after.
					if (app.scripted())
					{
						if (app.townFut.valid()) { app.townFut.wait(); }
						if (app.bldFut.valid()) { app.bldFut.wait(); }
						if (app.skyFut.valid()) { app.skyFut.wait(); }
					}
					app.sim.open(app.opt.simlink, app.dataHash, err);
				}
				if (!err.empty()) { fprintf(stderr, "kclient: --simlink: %s\n", err.c_str()); }
			}
			if (app.sim.active()) { sim_opened(); }
			return true;
		} },
	};
	if (app.scripted()) { while (app.loading()) { run_load_step(); } }
	else { app.loadScreen.init(app.opt.data, app.uiOn ? dl::gpu::view(ui::gfx::atlas()) : sg_view{}); }
}

// The Buildings_Click switch values, indexed by the BUILDING record's `select sound` int (the original's 1405504a0 reads it; 0 = no click sound and
// the event is skipped). The names are the game's switch table, kenshi-inspect findings/rwh65/switches.tsv (141e17308's values); past the table: ""
// rather than a guess.
const char* buildings_click_sound(int v)
{
	static const char* const kNames[] = { "", "Bed", "Dummy", "Farm", "Generator", "Grain_Silo", "House", "Mine_Ore", "Mine_Stone", "Research_Bench",
		"Store_Armor", "Store_Building_Materials", "Store_Flour", "Store_Ore", "Store_Prisoner", "Store_Rum", "Store_Stones", "Stove", "Well",
		"Wind_Generator", "Chain_Bench", "Fabric_Loom", "Leather_Bench", "Medical_Desk", "Plate_Station", "Shop_Counter", "Smithy", "Stone_Processor",
		"Armorer", "Bar", "Boxes", "Campfire", "Gate", "Gen_Goods", "Hive_Hut", "Lamp", "Long_House", "Outhouse", "Store_Armor",
		"Store_Building_Materials", "Store_Flour", "Store_Ore", "Store_Prisoner", "Tailor", "Tower", "Travel", "Turret", "Wall_Metal",
		"Wall_Station_House", "Wall_Stone", "Wall_Wood", "Wood_Obj_Big", "Wood_Obj_Small" };
	return v >= 0 && v < (int)std::size(kNames) ? kNames[v] : "";
}

// The original's new-game start order (k16.17.4.2; kenshi-inspect findings/newgame_startorder_static.md, kenshi-rwh.72, our own port of its reference
// implementation): there is no order field. The panel walks the game-data container's per-type index, a boost::unordered_map<stringID, GameData*> kept at
// B = 16 buckets while 16 or fewer records load: a sid's bucket is its hash & (B - 1); the first record of a bucket becomes the global list's head and a
// later record of the same bucket goes in front of that bucket's first, so the list reads buckets in reverse order of first appearance, newest first inside
// each bucket. Feed the sids in merge order (GameData::byType's stable merge order: gamedata.base, data/*.mod, then the mods.cfg list; a repeated sid keeps
// its first slot). The panel opens on the hard-coded sid 1980-gamedata.base (Wanderer) and its arrows wrap, so the player's list is this walk rotated to
// that sid (a set without it opens on the walk's first entry).
// The walk itself (the sid hash, the buckets, the rehash past 16 records) is sidorder.hpp, shared with the animation manager's idle list (kenshi-rwh.101).
std::vector<std::string> start_order(const std::vector<std::string>& sidsInLoadOrder)
{
	std::vector<std::string> out;
	for (const std::size_t i : sidorder::walk(sidsInLoadOrder)) { out.push_back(sidsInLoadOrder[i]); }
	return out;
}

// --character SPEC: `;`-separated keys (clip names hold spaces): race=NAME, male / female, clip=NAME (a raw clip), anim=ANIMATION (repeatable, layered), move=SPEED (locomotion: picks and blends the records; 0 = idle), strafe=LATERAL SPEED, combat, stealth,
// action=COMBAT_TECHNIQUE[ override]|ANIMATION|clip (layer 1, full body; a block: upper body, ` override` = the R arm override clone) with action_t=SECONDS since it started and action_speed=F (default: the technique's mult),
// shape=SLIDER:VALUE (repeatable; editor sliders by name, raw values, e.g. shape=Height:115; the rest at the race xml's mid), muscle=M, skinny=0..1, strong=0..1 (normal map blend), wound=PART:HEALTH (repeatable; LOCATIONAL_DAMAGE part, health 0..1: the pain overlay), stance=ANIMATION, legs=R,L (leg values, 100 healthy), posture= / shoulders= / neck= (editor 0..100), t=SECONDS, speed=F, head=N, skin=0..1, wear=ARMOUR|CONTAINER (repeatable; CONTAINER = a backpack),
// weapon=WEAPON|CROSSBOW[/MATERIAL_SPECS_WEAPON] (repeatable: hip, back), drawn (the first weapon in hand), loaded (crossbows hold a bolt), faction=FACTION (armour colours), carry=left|right (carrying a body),
// carried=N (carried by the Nth spec of this list, which has carry=; `carry me` on its shoulder, at= / yaw= unused),
// hair=ATTACHMENT, beard=ATTACHMENT, haircol=COLOR_DATA, settle=SECONDS (steps the animation clock that long before the first frame: a still character's idle fit engages), yaw=DEG (added to facing the camera), at=FORWARD,RIGHT (world units from the start camera; default: a row 30 ahead). Feet on the terrain.
bool init_characters()
{
	try
	{
		// The CPU work (the game data merge, file reads, decode) is the future step 1 started (kenshi-k16.2.26): poll it, never block the frame. A
		// scripted run waits here (it runs every step in one frame for deterministic shots, where polling would only spin).
		if (app.charsPhase == 0)
		{
			if (!app.charsFut.valid()) { app.charsFut = std::async(std::launch::async, [] { return kworld::loadMergedGameData(app.opt.data); }); }
			// Both workers' results before either is consumed (a .get() on a retried phase must not be needed twice).
			const bool charsIn = app.scripted() || app.charsFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			const bool hashIn = !app.hashFut.valid() || app.scripted() || app.hashFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			if (!charsIn || !hashIn) { return false; }	// still on the workers: retried next frame
			app.gdPending = std::make_unique<kdata::GameData>(app.charsFut.get());
			if (app.hashFut.valid())
			{
				try { app.dataHash = app.hashFut.get(); }
				catch (const std::exception& e) { app.hashErr = e.what(); }
			}
			app.chars.shadow = &app.shadow;
			app.chars.culled = char_cut_hidden;	// on a floor the cut hides (user 2026-11-05: the guard on the roof at floor 0; k16.2.264.1: by the wire's own floor, shared with the tags and labels)
			app.chars.init(&app.res, *app.gdPending);
			// the animation lab's own character set: never drawn in the world (it renders offscreen only). Setup is cheap; the lab inits on its first
			// open or `lab` op (every game start used to pay for it), and a failure there is a warning: the client runs on.
			dev::labSetup(app.res, app.opt.data, app.shadow);
			app.charsPhase = 1;
			return false;	// the panel records next frames: they and the asset build used to be one 134 ms step (kenshi-k16.2.26)
		}
		// The panel records (kenshi-k16.2.26): one job per record, run under a per-frame time budget. As one step, or one record type a frame, they
		// still blocked the window for 134-190 ms; the research records' reference lookups alone were ~190 ms.
		static std::vector<std::function<void()>> recs;
		static size_t recsNext = 0;
		if (recs.empty() && recsNext == 0)
		{
			const kdata::GameData* gdp = app.gdPending.get();
			const kdata::GameData& gd = *gdp;
			const auto itemName = [](const kdata::GameData& d, const std::string& id) { const kdata::Record* r = d.find(id); return r ? r->name : id; };
			// The new game panel's starts (docs/reference/STARTS_CAGES_STATIC.md a0: description / difficulty / style are the window's; money the start's)
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			{
				recs.push_back([r]
				{
					std::string info;
					for (const char* k : { "difficulty", "style" }) { if (std::string v = r->stringOr(k); !v.empty()) { info += (char)std::toupper((unsigned char)k[0]) + std::string(k + 1) + "\t" + v + "\n"; } }
					info += "Money\t" + std::to_string(r->intOr("money", 0)) + " c\n\n" + r->stringOr("description");
					app.starts.push_back({ r->stringID, r->name, info });
				});
			}
			recs.push_back([]
			{
				// The original's order (k16.17.4.2): the walk over the sids in merge order (app.starts is filled in byType order), then rotated so the
				// preset start 1980-gamedata.base comes first; a set without it opens on the walk's first entry.
				std::vector<std::string> sids;
				for (const ui::StartEntry& s : app.starts) { sids.push_back(s.id); }
				const std::vector<std::string> walk = start_order(sids);
				std::vector<ui::StartEntry> ordered;
				for (const std::string& sid : walk)
				{
					for (const ui::StartEntry& s : app.starts) { if (s.id == sid) { ordered.push_back(s); break; } }
				}
				const auto preset = std::find_if(ordered.begin(), ordered.end(), [](const ui::StartEntry& s) { return s.id == "1980-gamedata.base"; });
				const bool rotated = preset != ordered.end();
				if (rotated) { std::rotate(ordered.begin(), preset, ordered.end()); }
				app.starts = std::move(ordered);
				if (app.opt.debug)
				{
					std::string names;
					for (size_t i = 0; i < app.starts.size(); i++) { names += (i ? ", " : "") + app.starts[i].title; }
					printf("new game: starts order (%zu, the original's walk rotated to %s): %s\n", app.starts.size(),
						rotated ? "1980-gamedata.base" : "the walk's first (1980-gamedata.base absent)", names.c_str());
					// The stock set (findings section 3): the walk's array and its rotation are known, so a drift shows here.
					static const std::vector<std::string> kStock = {
						"1533659-rebirth.mod", "1533582-rebirth.mod", "47924-rebirth.mod", "1981-gamedata.base", "3062-gamedata.base", "1980-gamedata.base",
						"1994-gamedata.base", "2060-gamedata.base", "2918-gamedata.base", "1533657-rebirth.mod", "1985-gamedata.base", "5425-gamedata.base",
						"1532934-rebirth.mod", "2287-gamedata.base" };
					// The shipped rebirth.mod marks 5425-gamedata.base (The Hungover Ninja) REMOVED, so the stock set is the table's 14 less it; a removal
					// only unlinks one node, so the expected walk is the table filtered to the sids present.
					bool stock = true;
					for (const std::string& sid : sids) { stock = stock && std::find(kStock.begin(), kStock.end(), sid) != kStock.end(); }
					std::vector<std::string> want;
					for (const std::string& sid : kStock) { if (std::find(sids.begin(), sids.end(), sid) != sids.end()) { want.push_back(sid); } }
					if (stock)
					{
						const bool ok = walk == want && app.starts.size() == want.size() && !app.starts.empty() && app.starts[0].id == "1980-gamedata.base"
							&& (app.starts.size() < 2 || app.starts[1].id == "1994-gamedata.base");
						const bool ninja = std::find(sids.begin(), sids.end(), "5425-gamedata.base") != sids.end();
						printf("new game: starts order check: %s (the stock %zu, the findings table%s, rotated: 1980-gamedata.base, 1994-gamedata.base)\n",
							ok ? "ok" : "MISMATCH", sids.size(), ninja ? "" : " less The Hungover Ninja, which rebirth.mod marks REMOVED");
					}
				}
				if (app.newGame) { app.newGame->setStarts(app.starts); }
			});
			// The build window's records (build_menu.md 2-3); which are listed is the sim's (UNLOCKS 36)
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("BUILDING")))
			{
				recs.push_back([gdp, r, itemName]
				{
					const kdata::GameData& gd = *gdp;
					App::BuildRec b;
					b.name = r->name;
					b.category = r->stringOr("building category", "misc");
					b.group = r->stringOr("building group");
					b.description = r->stringOr("Description");
					b.type = r->boolOr("is exterior furniture", false) ? "Exterior" : r->boolOr("is interior furniture", false) ? "Interior" : "Building shell";
					b.selectSound = buildings_click_sound(r->intOr("select sound", 0));
					b.materials = r->intOr("build materials", 0);
					b.speedMult = r->floatOr("build speed mult", 1);
					b.powerOut = r->intOr("power output", 0);
					b.powerCap = r->intOr("power capacity", 0);
					b.operators = r->intOr("max operators", 0);
					if (const kdata::RefCategory* c = r->findRefs("construction")) { for (const kdata::RefEntry& e : c->entries) { b.cost.push_back({ itemName(gd, e.targetID), e.v0 }); } }
					const kdata::RefCategory* fn = r->findRefs("functionality");
					if (const kdata::Record* f = fn && !fn->entries.empty() ? gd.find(fn->entries.front().targetID) : nullptr)
					{
						b.function = f->intOr("function", 0);
						b.outputRate = f->floatOr("output rate", 0);
						b.mining = f->intOr("world resource mining", 0);
						const kdata::RefCategory* p = f->findRefs("produces");
						if (p && !p->entries.empty()) { b.produces = itemName(gd, p->entries.front().targetID); }
					}
					app.buildRecs[r->stringID] = std::move(b);
					app.interiorSounds[r->name] = { r->intOr("interior ambience", 0), r->floatOr("interior integrity", 0), r->intOr("cavernous", 0) != 0 };
					app.sim.setDoorSound(r->stringID, r->intOr("door type", 0), r->floatOr("door move speed", 0.5f));
					app.doorSlides[r->stringID] = { r->intOr("door axis", 0), r->floatOr("door move dist", 30), r->floatOr("door comes out", 0), r->floatOr("door move speed", 0.5f) };
				});
			}
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("TOWN")))
			{
				recs.push_back([r]
				{
					app.townTypes[r->stringID] = r->intOr("type", 0);
					app.townUnexplored[r->stringID] = r->stringOr("unexplored name");
				});
			}
			for (const char* type : { "ATTACHMENT", "COLOR_DATA" })
			{
				const std::string prefix = std::string(type) + ":";
				for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type))) { recs.push_back([r, prefix] { app.creatorSids[prefix + r->name] = r->stringID; }); }
			}
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RACE")))
			{
				recs.push_back([gdp, r]
				{
					app.creatorSids["RACE:" + r->name] = r->stringID;
					app.raceNames[r->stringID] = r->name;
					for (int g = 0; g < 2; g++)
					{
						const kdata::RefCategory* heads = r->findRefs(g ? "heads female" : "heads male");
						if (!heads) { continue; }
						for (const kdata::RefEntry& e : heads->entries) { if (gdp->find(e.targetID)) { app.raceHeads[r->name][g].push_back(e.targetID); } }	// as Characters counts them
					}
				});
			}
			// The RESEARCH page's records; the unlock labels are ours
			static const std::pair<const char*, const char*> unlockRefs[] = { { "enable buildings", "New building:" }, { "improve buildings", "Improves:" },
				{ "enable weapon type", "New weapon class:" }, { "enable weapon model", "New weapon grade:" }, { "enable armour", "New armour:" },
				{ "enable item", "New item:" }, { "enable crossbow", "New crossbow:" }, { "enable backpack", "New backpack:" }, { "enable robotics", "New robot limb:" } };
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RESEARCH")))
			{
				if (r->name.starts_with("_")) { continue; }	// the start-off sets (_Default Start, ...): never researched (MINE until the static)
				recs.push_back([gdp, r, itemName]
				{
					const kdata::GameData& gd = *gdp;
					App::ResearchRec rr;
					rr.name = r->name;
					rr.category = r->stringOr("category", "misc");
					rr.description = r->stringOr("description");
					rr.level = r->intOr("level", 1);
					rr.time = r->intOr("time", 0);
					rr.blueprintOnly = r->boolOr("blueprint only", false);
					rr.levelUpgrade = r->boolOr("is level upgrade", false);
					if (const kdata::RefCategory* c = r->findRefs("cost")) { for (const kdata::RefEntry& e : c->entries) { rr.cost.push_back({ itemName(gd, e.targetID), e.v0 }); } }
					if (const kdata::RefCategory* c = r->findRefs("enable buildings")) { for (const kdata::RefEntry& e : c->entries) { rr.buildings.push_back(e.targetID); } }
					if (const kdata::RefCategory* c = r->findRefs("requirements")) { for (const kdata::RefEntry& e : c->entries) { rr.needs.push_back(e.targetID); } }
					for (const auto& [ref, label] : unlockRefs)
					{
						if (const kdata::RefCategory* c = r->findRefs(ref)) { for (const kdata::RefEntry& e : c->entries) { rr.unlocks.push_back({ label, itemName(gd, e.targetID) }); } }
					}
					app.researchRecs[r->stringID] = std::move(rr);
				});
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		while (recsNext < recs.size() && (recsNext == 0 || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(kRecordsFrameMs))) { recs[recsNext++](); }
		if (recsNext < recs.size()) { return false; }	// the budget ran out: the rest next frame
		recs.clear();
		recsNext = 0;
		if (app.gdPending) { app.gdFreeFut = std::async(std::launch::async, [gd = std::move(app.gdPending)]() mutable { gd.reset(); }); }	// its records free in ~140 ms: a worker, never the frame (kenshi-k16.2.26)
		app.charsPhase = 2;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "kclient: characters: game data not loaded (%s)\n", e.what());
		app.charsPhase = 2;	// failed: the step carries on without characters (no retry on a consumed future)
		return false;
	}
	return app.charsReady = true;
}

// specs placed from a camera at x, z looking along yawRad (the start camera, or a scene's)
void place_characters(const std::vector<std::string>& specs, double x, double z, double yr)
{
	const double fx = std::sin(yr), fz = -std::cos(yr);
	const size_t n = specs.size(), base = app.chars.size();
	double settle = 0;
	for (size_t i = 0; i < n; i++)
	{
		Characters::Spec s;
		double fwd = 30, right = ((double)i - (double)(n - 1) * 0.5) * 8.0, yaw = 0;
		std::stringstream ss(specs[i]);
		std::string kv;
		while (std::getline(ss, kv, ';'))
		{
			const size_t eq = kv.find('=');
			const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
			if (k == "race") { s.race = v; }
			else if (k == "female" || k == "male") { s.female = k == "female"; }
			else if (k == "clip") { s.clip = v; }
			else if (k == "anim") { s.anims.push_back(v); }
			else if (k == "t") { s.time = (float)atof(v.c_str()); }
			else if (k == "speed") { s.speed = (float)atof(v.c_str()); }
			else if (k == "move") { s.loco = true; s.move = (float)atof(v.c_str()); }
			else if (k == "combat") { s.combat = true; }
			else if (k == "strafe") { s.strafe = (float)atof(v.c_str()); }
			else if (k == "wound") { const size_t c = v.rfind(':'); if (c != std::string::npos) { s.wounds.push_back({ v.substr(0, c), (float)atof(v.c_str() + c + 1) }); } }
			else if (k == "shape") { const size_t c = v.rfind(':'); if (c != std::string::npos) { s.shape.push_back({ v.substr(0, c), (float)atof(v.c_str() + c + 1) }); } }
			else if (k == "muscle") { s.muscle = (float)atof(v.c_str()); }
			else if (k == "skinny") { s.skinny = (float)atof(v.c_str()); }
			else if (k == "strong") { s.strong = (float)atof(v.c_str()); }
			else if (k == "action") { s.action = v; }
			else if (k == "action_t") { s.actionTime = (float)atof(v.c_str()); }
			else if (k == "action_speed") { s.actionSpeed = (float)atof(v.c_str()); }
			else if (k == "stealth") { s.stealth = true; }
			else if (k == "stance") { s.stance = v; }
			else if (k == "posture") { s.posture = (float)atof(v.c_str()); }
			else if (k == "shoulders") { s.shoulderSet = (float)atof(v.c_str()); }
			else if (k == "neck") { s.neckPosition = (float)atof(v.c_str()); }
			else if (k == "legs") { sscanf(v.c_str(), "%f,%f", &s.legs[0], &s.legs[1]); }
			else if (k == "head") { s.head = atoi(v.c_str()); }
			else if (k == "skin") { s.skin = (float)atof(v.c_str()); }
			else if (k == "wear") { s.wear.push_back(v); }
			else if (k == "weapon") { s.weapons.push_back(v); }
			else if (k == "drawn") { s.drawn = true; }
			else if (k == "loaded") { s.loaded = true; }
			else if (k == "carry") { s.carry = v == "left" ? 1 : v == "right" ? 2 : 0; }
			else if (k == "carried") { s.carriedBy = (int)base + atoi(v.c_str()); }
			else if (k == "hair") { s.hair = v; }
			else if (k == "beard") { s.beard = v; }
			else if (k == "haircol") { s.hairColour = v; }
			else if (k == "faction") { s.faction = v; }
			else if (k == "severed") { s.severed = atoi(v.c_str()); }	// limb bits: 1 left arm, 2 right arm, 4 left leg, 8 right leg
			else if (k == "yaw") { yaw = atof(v.c_str()); }
			else if (k == "at") { sscanf(v.c_str(), "%lf,%lf", &fwd, &right); }
			else if (k == "settle") { settle = std::max(settle, atof(v.c_str())); }
			else { fprintf(stderr, "kclient: --character: unknown key %s\n", k.c_str()); }
		}
		s.pos[0] = x + fx * fwd - fz * right;
		s.pos[2] = z + fz * fwd + fx * right;
		s.pos[1] = app.hm->height(s.pos[0], s.pos[2]);
		// the mesh faces model +Z: yaw -camera yaw turns it towards the camera
		s.yaw = (float)(-yr + yaw * M_PI / 180.0);
		std::string err;
		if (!app.chars.add(s, err)) { fprintf(stderr, "kclient: --character %s: %s\n", specs[i].c_str(), err.c_str()); }
	}
	// settle=SECONDS (any spec): step every character's animation clock that long in 1/60 s frames, so a still character's idle fit (idleFor >= 0.5 s)
	// has engaged before the shot; scripted runs and scenes otherwise freeze it (the shot stays deterministic: fixed steps)
	for (int i = 0, frames = (int)std::lround(settle * 60); i < frames; i++) { app.chars.update(1.0 / 60.0, 1.0 / 60.0); }
	app.settledChars = settle > 0;
	if (n) { printf("characters: %zu placed\n", app.chars.size()); }
}

// --blood / blood= SPEC: TYPE:FORWARD,RIGHT,SIZE[,DIRDEG[,IMAGE]] from a camera at x, z looking along yawRad. TYPE = splash | pool | smear | line (BloodDecals), SIZE as
// the game's emitters (hit splash 0.05..0.5, smear 0.2, pool drop <= 0.5), DIRDEG = the image's u (width) axis, clockwise from the camera's forward. Human blood colour.
void place_blood(const std::vector<std::string>& specs, double x, double z, double yr)
{
	const double fx = std::sin(yr), fz = -std::cos(yr);
	for (const std::string& spec : specs)
	{
		const size_t colon = spec.find(':');
		const int type = BloodDecals::typeByName(spec.substr(0, colon));
		double fwd = 0, right = 0, size = 0, deg = 0;
		int image = -1;
		if (type < 0 || colon == std::string::npos || sscanf(spec.c_str() + colon + 1, "%lf,%lf,%lf,%lf,%d", &fwd, &right, &size, &deg, &image) < 3)
		{
			fprintf(stderr, "kclient: --blood %s: want TYPE:FORWARD,RIGHT,SIZE[,DIRDEG[,IMAGE]]\n", spec.c_str());
			continue;
		}
		const float colour[4] = { 128 / 255.0f, 0, 0, 1 };	// RACE Human `blood colour` 0x800000
		const double a = yr + deg * M_PI / 180.0;
		app.blood.add((BloodDecals::Type)type, x + fx * fwd - fz * right, z + fz * fwd + fx * right, (float)size, (float)std::atan2(-std::cos(a), std::sin(a)), colour, image);
	}
	if (!specs.empty()) { printf("blood: %zu decals\n", app.blood.size()); }
}

void run_load_step()
{
	static const auto start = std::chrono::steady_clock::now();
	const auto t0 = std::chrono::steady_clock::now();
	if (!app.loadSteps[app.loadIdx].second()) { return; }	// not finished (waiting on a worker): retried next frame
	const auto t1 = std::chrono::steady_clock::now();
	fprintf(stderr, "kclient: load step %zu/%zu %-48s %7.0f ms (%.0f ms since the first step)\n", app.loadIdx + 1, app.loadSteps.size(), app.loadSteps[app.loadIdx].first,
		std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t1 - start).count());
	app.loadIdx++;
}

// The sim link is up: the click layer and the cursors start with it. Once the server's HELLO is in (no order may go before it), update_ui asks for
// the saves (CONTINUE).
void sim_opened()
{
	logicthread::renderIdle();	// overlap (rule 12): the new game's link, collision and the buildings' callbacks change under no draw
	app.collide.start(app.opt.data, &*app.hm);
	app.sim.ylogAlt = [](double x, double z) { return app.hm->height(x, z); };	// KYLOG: the bilinear height() beside the drawn-triangle terrainHeight()
	app.sim.setDepenetrate([](double x, double z, double y, double r, double cap, double out[2], std::string* why) { return app.collide.depenetrate(x, z, y, r, cap, out, why); });
	if (kStandRay.get())
	{
		app.sim.setGroundRay([](double x, double z, double wy, double up, double down, double& y, const void** gen) { int r = app.collide.groundY(x, z, wy, up, down, y, gen); double dy; if (r == -1 && !app.buildings.groundDrawn(x, z, wy, 12.0, 12.0, dy)) { return 3; } /* k16.2.382.1: collision not loaded, no building drawn over x,z: open ground */ if (r == 0 && kStandRayFaces.get() && app.buildings.groundDrawn(x, z, wy, up, down, y)) { return 2; } return r; },
			[]() -> const void* { const auto g = app.collide.gathered(); return g.get(); });
	}
#ifdef KCLIENT_DEBUG_VIEW
	app.debugView.reset();
	app.debugSubscribed = false;
	dev::shotTreesReset();	// no shot's trees capture is pending at a session's start
	app.sim.setDebugSink([](uint16_t type, const uint8_t* p, size_t n)
	{
		const uint32_t before = app.debugView.treeSerial;
		app.debugView.frame(type, p, n);
		if (app.debugView.treeSerial != before) { dev::shotTreesTree(dev_context(), app.debugView.tree); }	// one answer: the pending capture's, if it asked
	});
#endif
	app.buildings.lineHits = [](const double from[3], const double to[3], uint32_t building)	// a building shape inside that building's box, along the segment
	{
		double dir[3], p[3] = { from[0], from[1], from[2] }, t;
		const double len = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
		for (int k = 0; k < 3; k++) { dir[k] = (to[k] - from[k]) / len; }
		bool isBuilding = false;
		float box[6];
		for (double done = 0; done < len && app.collide.ray(p, dir, len - done, t, box, {}, nullptr, nullptr, &isBuilding); done += t + 0.5)
		{
			for (int k = 0; k < 3; k++) { p[k] += dir[k] * t; }
			// the follow camera passes thin props (the boom's sweep does), so one in the line does not ghost its whole building: the cone dithers it
			if (isBuilding && !(app.follow && CollisionPick::thin(box)) && app.buildings.holds((int)building, p, false)) { return true; }
			for (int k = 0; k < 3; k++) { p[k] += dir[k] * 0.5; }
		}
		return false;
	};
	app.cursors.init(app.opt.data, app.uiOn ? std::function<sg_view()>([] { return dl::gpu::view(ui::gfx::atlas()); }) : std::function<sg_view()>(), (int)ui::gfx::kAtlasW, (int)ui::gfx::kAtlasH);	// the callback runs in the render-side job: the atlas TexId resolves there (.38)
	app.greeted = false;
	app.progressLevelSent = -1;	// a new connection: send ui.progress_bars' level again at its HELLO
	// kenshi-k16.2.112.1: a session with no explicit position starts with no camera target — nothing streams or draws until the squad, a new game,
	// a scenario or a load places the camera (the camera sits at the Hub default meanwhile, never the world origin). Once a world has been drawn,
	// a later session (EXIT GAME, a new title game) keeps its camera: that is a place in the game, not a spoiler.
	if (!app.opt.explicitPos && !app.firstWorldFrame)
	{
		app.worldTarget = false;
		app.targetReason = "";
		app.targetWait = 0;
		app.loadPending = false;
	}
}

void cleanup()
{
	lightbake::cancel();	// first: an in-flight sun/probe bake stops at its next chunk while the logic thread and the server stop (the window no longer pumps events here)
	readback::flushWrites();	// every quit path: the screenshots still being written land first (a script's quit right after its shot)
	if (logicthread::running())	// rule 8: the stop flag, the notify, the join; only then anything below (it all touches what logic used)
	{
		const double ms = logicthread::stop();
		fprintf(stderr, "kclient: quit: logic thread joined in %.0f ms\n", ms);
		lastframe::shutdown();
	}
	{	// every quit path (menu button, window close, scripted quit) ends here: stop the in-process server before anything else, not in the static App's
		// destructor, which can hang on another member first (Grass::~Grass) and never reach ~LocalServer: the server kept ticking (kenshi-379.12)
		const auto t0 = std::chrono::steady_clock::now();
		app.server.stop();
		const auto t1 = std::chrono::steady_clock::now();
		const auto ms = [](auto d) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
		fprintf(stderr, "kclient: quit: server stopped in %lld ms (%s)\n", ms(t1 - t0),
			g_quitAsked == std::chrono::steady_clock::time_point{} ? "window close" : ("quit requested " + std::to_string(ms(t1 - g_quitAsked)) + " ms ago").c_str());
	}
	app.buildings.stopThreads();	// the early returns below skip buildings.shutdown(): its workers would block ~App (second hang, worldbuildings workers in cond_destroy)
	dev::cleanup();
	windowgate::flush();	// a change still held at exit is applied, so the saved geometry is the state the client applied (k16.2.222)
	windowgeom::save();	// before cfg::save() below writes it
	audiohook_shutdown();	// before the early returns: the audio thread may be running from the loading screen on
	cfg::save();
	if (!dev::viewing()) { app.hud.reset(); app.options.reset(); app.items.reset(); app.nameLabels.reset(); app.speechBubbles.reset(); app.dialogue.reset(); app.squadsUi.reset(); app.research.reset(); app.crafting.reset(); app.faction.reset(); app.prompt.reset(); app.overview.reset(); app.prospect.reset(); app.build.reset(); app.creatorUi.reset(); app.tutorials.reset();
#ifdef KCLIENT_DEV_TOOLS
		dev::resetDebugPanels(); dev::resetAnimBrowser();
#endif
#ifdef KCLIENT_DEBUG_VIEW
		app.debugTree.reset();
#endif
		 app.gameMenu.reset(); app.mainMenu.reset(); app.newGame.reset(); app.adv.reset(); app.saves.reset(); ui::shutdown(); }
	rq::flushAll();	// the resource jobs the last logic frame queued (docs/RENDER_THREAD.md §7.2), while every renderer is still up
	if (app.loading() && !dev::viewing()) { app.loadScreen.shutdown(); rq::flushAll(); sdtx_shutdown(); sgl_shutdown(); sg_shutdown(); return; }	// closed during the loading screen: most subsystems never started
	if (dev::viewing()) { dev::viewShutdown(); rq::flushAll(); sdtx_shutdown(); sgl_shutdown(); sg_shutdown(); return; }	// the mesh viewer's buffers, while sokol still is
	if (!app.opt.matReport.empty())
	{
		std::ofstream(app.opt.matReport) << app.buildings.materialReport();
	}
	if (app.grassOn) { app.grass.shutdown(); }
	if (app.foliageOn) { app.foliage.shutdown(); }
	if (app.fogvol.enabled) { app.fogvol.shutdown(); }
	if (app.opt.particles) { app.particles.shutdown(); }
	app.blood.shutdown();
	dev::labShutdown();	// the animation lab's render target and character, while sokol still is
	if (app.portraitsTex) { dl::gpu::unbind(app.portraitsTex); app.portraitsTex = 0; }
	app.portraits.shutdown();
	app.portraitTable.clear();
	app.post.shutdown();
	interiorclip::shutdown();
	app.sunShafts.shutdown();
	app.dust.shutdown();
	app.lightVolumes.shutdown();
	app.hdr.shutdown();
	app.refl.shutdown();
	app.idbuf.shutdown();
	GPU_SHUTDOWN(app.gpu);
	app.squads.shutdown();
	app.arrows.shutdown();
	app.chars.shutdown();
	if (app.bldSet) { app.buildings.shutdown(); }
	if (app.opt.water) { app.water.shutdown(); }
	app.wx.shutdown();
	spec_cube_shutdown();
	app.creatorScene3d.shutdown();
	app.sky.shutdown();
	app.terrain.shutdown();
	app.shadow.shutdown();
	app.wmap.shutdown();
	app.cursors.shutdown();
	app.trails.shutdown();	// the dump's open segment (the context is sgl_shutdown's, k16.19.6.1)
	rq::flushAll();	// the shutdowns' queued destroys
	dl::gpu::shutdown();	// the draw list's textures, pipeline and samplers (render hook 8; none until something records)
	sdtx_shutdown();
	sgl_shutdown();
	sg_shutdown();
	if (g_exitCode) { exit(g_exitCode); }
}

}
