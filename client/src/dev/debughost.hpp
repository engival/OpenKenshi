#pragma once
// The shared context of the dev-only debug ops (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.17): references to the App
// subsystems the ops read, filled once by main.cpp (the host side) and handed to the ops in dev/debughost.cpp, so the dev code never sees App.
// Later passes (kenshi-k16.2.153.18 on) add the subsystems their ops need. Plain references only: nothing here is dev-gated, so main.cpp
// can build it in every configuration; the ops that use it are (dev/devhooks.hpp).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "doorpose.hpp"
#include "idbuffer.hpp"	// IdBuffer::Hit: `dump peek` reads the cursor pass's last result
#include "pick.hpp"	// Pick: the shot sidecar's cursor pick (dev/shotsidecars.cpp)
#include "opencont.hpp"	// OpenCont: `dump windows` lists the open item windows
#include "ui/hud.hpp"	// ui::HudBuildingInfo: the machine panel main.cpp builds (dev::Context::machinePanel)

class BeamField;
class BloodDecals;
class ClientNav;
class CollisionPick;
class Characters;
class Grass;
class GpuTimer;
class Hdr;
class Particles;
class Terrain;
class ParticleWorld;
class PhysicsWorld;
class Ragdolls;
class StaticColliders;
class Foliage;
class Trails;
class Squads;
struct TownSet;
class Shadow;
class SimView;
class Sky;
class WeatherInstances;
class WeatherMap;
class WorldBuildings;
struct Camera;
struct SkyRamp;
struct Weff;
namespace cfg { class Float; class Bool; }
namespace dbg { struct DebugView; }
namespace lightvol { struct Volume; }
namespace kworld { class Heightmap; }
namespace ui { class ItemWindows; class ProgressBars; }

namespace dev
{
	using ::DoorSlide;	// a door record's sliding opening (App::doorSlides, doorpose.hpp)

	struct PickSkip { std::string name; double at[3]; std::string why; };	// a shape the pick passed and why (main.cpp's PickSkip, kenshi-k16.2.259)

	struct Context
	{
		SimView& sim;
		Characters& chars;
		BeamField& beams;
		std::unordered_map<std::string, DoorSlide>& doorSlides;
		const int& frames;	// App::frames
		// kenshi-k16.2.153.19 (pass 2): the buildings / floor / light / window dumps and the machine op.
		WorldBuildings& buildings;	// App::buildings
		double (&pos)[3];	// App::pos, the camera eye (the scene test and the viewer move it)
		const double& mouseX;	// App::mouseX, where the cursor is (`dump peek`)
		const double& mouseY;	// App::mouseY
		const std::unique_ptr<ui::ItemWindows>& items;	// App::items, the open item windows (`dump windows`)
		const uint64_t& gearChar;	// App::gearChar, the character the inventory window shows
		const bool& itemsShown;	// App::itemsShown, the character window (I) is up
		const uint64_t& itemTarget;	// App::itemTarget, the marked window's owner
		const uint64_t& pendingId;	// App::pending.id, the box or body asked for by a click, not open yet
		const std::vector<OpenCont>& conts;	// App::conts, the open item windows besides the character's
		uint64_t& machine;	// App::machine, the open machine window (`machine id=N`)
		uint64_t& panelProducer;	// App::panelProducer, the thing the focus panel follows (`machine id=N`)
		ui::HudBuildingInfo (*machinePanel)();	// main.cpp's machine_panel (game code: the HUD draws it too)
		void (*scriptError)(int frame, const char* op);	// main.cpp's script_error: a bad script command exits
		// kenshi-k16.2.153.21 (pass 3): the sky / weather / gpu / effects dumps and the debug weather / weff / time ops.
		Sky& sky;	// App::sky
		Hdr& hdr;	// App::hdr, the exposure chain's readback (`dump sky`: read only inside a readback::sample request, on the render side)
		float& hdrSceneLum;	// App::hdrSceneLum
		float& hdrAdapted;	// App::hdrAdapted
		const WeatherMap& wx;	// App::wx, the regional weather's atlas (`dump sky`)
		const SkyRamp& skyRamp;	// App::skyRamp
		const WeatherInstances& weatherInst;	// App::weatherInst, the world-placed effects (`dump weather`)
		const cfg::Float& interiorLight;	// kInteriorLight, render.interior_light (the stored value: the amount of original lighting, 1 = the effect off)
		const cfg::Float& sunBounce;	// kSunBounce, render.sun_bounce
		Particles& particles;	// App::particles, the script parser (`dump effects`)
		const ParticleWorld& particleWorld;	// App::particleWorld
		const bool& particlesOn;	// App::opt.particles
		std::vector<Weff>& weff;	// App::weff, the `debug weff` pins
		// kenshi-k16.2.153.23 (pass 4): the chars / anim ops (dev/charops.cpp).
		const std::optional<kworld::Heightmap>& hm;	// App::hm, the drawn terrain
		const Shadow& shadow;	// App::shadow (`dump feet`: where the sole's shadow lands)
		const bool& shadowFlip;	// App::shadowFlip, F3's pending flip of shadow.enabled (the menu shows the state after it)
		CollisionPick& collide;	// App::collide, the floor ray of the foot log
		const std::unique_ptr<ui::ProgressBars>& progressBars;	// App::progressBars (`dump bars`)
		double& yaw;	// App::yaw, the camera's yaw in radians
		double& pitch;	// App::pitch, the camera's pitch in radians
		bool& walk;	// App::walk, the camera on the ground
		bool (*worldToScreen)(const double p[3], float& x, float& y);	// main.cpp's world_to_screen
		void (*warnFrozenAnimLog)(const char* op);	// main.cpp's warn_frozen_anim_log: a scripted run holds the animation still
		// kenshi-k16.2.153.25 (pass 5): the nav / pick dumps and the debug panel / tree / squads / command ops.
		ClientNav& nav;	// App::nav, the server's navmesh tiles (`dump nav|pick`)
		bool& navTrace;	// App::navTrace: `dump navdraw` asks the next overlay draw to print its stairs
		void (*mouseRay)(double mx, double my, double dir[3]);	// main.cpp's mouse_ray
		double (*cameraNear)();	// main.cpp's camera_near: the pick ray starts on the near plane
		bool (*rayGround)(const double origin[3], const double dir[3], double hit[3]);	// main.cpp's ray_ground
		bool (*rayHit)(const double origin[3], const double dir[3], double hit[3], bool* building, const double* eye);	// main.cpp's ray_hit: the world hit
		bool (*seeThrough)(const double p[3], double bottom);	// main.cpp's see_through
		void (*pickAt)(double mx, double my, int& kind, uint64_t& id, double hit[3], std::vector<PickSkip>& skipped);	// main.cpp's pick_screen: Pick::kind, id, hit
		std::string (*hintsAt)(double mx, double my);	// main.cpp's hint_report: the control-hints list at a screen point (`hints at X Y`, kenshi-k16.2.294.2)
		// kenshi-k16.2.153.27 (pass 6): the physics / ragdoll / foliage / trails / sound ops.
		PhysicsWorld& physics;	// App::physics
		StaticColliders& statics;	// App::statics (`dump physics`: the colliders posted)
		Ragdolls& ragdolls;	// App::ragdolls
		Foliage& foliage;	// App::foliage
		Trails& trails;	// App::trails
		const std::string& dataDir;	// App::opt.data
		void (*cameraBasis)(double f[3], double r[3], double u[3]);	// main.cpp's camera_basis
		// kenshi-k16.2.153.11 (pass 7): the scene test, the `probe depth` op and the mesh viewer.
		bool& hideUi;	// App::hideUi (a scene shot hides the interface)
		std::string& sceneName;	// App::sceneName, the debug overlay's scene row
		bool& settledChars;	// App::settledChars (`settle=` scenes report foot_idle)
		bool& firstWorldFrame;	// App::firstWorldFrame (the viewer prints its own first_world_frame)
		const bool& charsReady;	// App::charsReady
		const bool& bldSet;	// App::bldSet
		const bool& bldReady;	// App::bldReady
		const bool& grassOn;	// App::grassOn
		const bool& foliageOn;	// App::foliageOn
		const std::string& wantWeather;	// App::wantWeather
		Grass& grass;	// App::grass
		Terrain& terrain;	// App::terrain
		BloodDecals& blood;	// App::blood, the scene's blood= decals
		GpuTimer& gpu;	// App::gpu
		const double& exposure;	// App::opt.exposure
		const int& particlesDrawn;	// App::particlesDrawn
		int& sgErrors;	// g_sgErrors
		int& exitCode;	// g_exitCode
		float& rainForce, &dustForce, &cloudsForce;	// App::rainForce / dustForce / cloudsForce
		double& windForce;	// App::windForce
		void (*setWeather)(const std::string&);	// main.cpp's set_weather
		void (*placeCharacters)(const std::vector<std::string>&, double, double, double);	// main.cpp's place_characters
		void (*placeBlood)(const std::vector<std::string>&, double, double, double);	// main.cpp's place_blood
		void (*requestQuit)();	// main.cpp's request_quit
		void (*requestHdrRead)();	// main.cpp's request_hdr_read
		void (*hudBox)(float out[4]);	// main.cpp's hud_box: x y w h
		void (*runScript)(bool post);	// main.cpp's run_script
		bool (*worldLoading)(int* pending);	// main.cpp's world_loading: a stream (buildings, foliage, grass, weather, the building set, the stream lane) still on its way
		void (*forwardVec)(double yaw, double pitch, double f[3]);	// main.cpp's forward_vec
		void (*currentCamera)(float nearZ, float farZ, Camera& out);	// main.cpp's current_camera
		// kenshi-k16.2.153.12 (pass 8): the debug panel and trails panel host side (dev/debughost.cpp).
		Squads& squads;	// App::squads (the panel's overlay row and the source list)
		const TownSet& towns;	// App::towns (`debug.spawn_bandits`)
		const bool& townsReady;	// App::townsReady
		const bool& follow;	// App::follow
		const bool& rts;	// App::rts
		const bool& autorun;	// App::autorun
		const cfg::Bool& navOverlay;	// kNavOverlay, debug.navmesh
		const cfg::Bool& navPruned;	// kNavPruned, debug.navmesh_pruned
		void (*debugToggle)(const std::string& id);	// main.cpp's debug_toggle (a panel toggle row)
		// kenshi-k16.2.153.13 (pass 9): the shot's debug sidecars (dev/shotsidecars.cpp).
		const std::string& cmdline;	// App::opt.cmdline (`.txt`: `command:`)
		const bool& debug;	// App::opt.debug (the trails sidecar records only under --debug)
		const bool& look;	// App::look, mouse look (`.txt`: `cursor:`)
		const std::string& weatherMix;	// App::weatherMix, the regional blend's HUD text (`.txt`: `weather:`)
		const uint64_t& followId;	// App::followId
		const double& followDist;	// App::followDist
		const double& followOffset;	// App::followOffset
	double (&focus)[3];	// App::focus, the RTS pivot (`.txt`: `pivot:` / `recreate:`)
		const double& rtsDist;	// App::rtsDist
		const char* const& focusOn;	// App::focusOn
		const char* (*compass)(double deg);	// main.cpp's compass: "N" / "NE" / ... for a heading in degrees
		const char* (*cameraMode)();	// main.cpp's camera_mode_name: "free" / "rts" / "follow" / "drive"
		std::string (*keysHeld)();	// main.cpp's keys_held: the keys down now, " K1 K2" (" none")
		std::string (*cursorText)();	// App::cursors.current(): the cursor's name
		std::string (*clicksReport)();	// main.cpp's clicks_report: the last clicks' lines (kenshi-k16.2.259.3)
		std::string (*pickOwnerReport)(const Pick&, double mx, double my, const std::vector<PickSkip>& skipped);	// main.cpp's pick_owner_report: the `owner:` / `primitive:` / `rule:` lines
		std::string (*hintsForPick)(const Pick&);	// the `hints:` block for a pick, as the `hints at X Y` op prints it
		Pick (*pickScreen)(double mx, double my, std::vector<PickSkip>& skipped);	// main.cpp's pick_screen: the sidecar's pick, `building` and skips included
		uint64_t (*cameraSubject)();	// main.cpp's camera_subject (`.txt`: `tracked:`)
		void (*buildMvp)(float mvp[16], float nearZ, float farZ);	// main.cpp's build_mvp (the trees capture's on-screen test)
		std::string (*hudText)();	// main.cpp's shot_hud_text: "hud.KEY: TEXT" lines of the developer text block under --debug, "" otherwise (k16.2.335.1)
#ifdef KCLIENT_DEBUG_VIEW
		dbg::DebugView& debugView;	// App::debugView
		const bool& debugSubscribed;	// App::debugSubscribed
		const uint64_t& infoChar;	// App::infoChar, the inspect / status panel's character
		void (*debugTreeOpen)(uint8_t kind, uint64_t id);	// main.cpp's debug_tree_open: DEBUG_QUERY and the tree window
		std::string (*treeStateValue)(uint8_t kind, uint64_t id, const std::string& key, const std::string& value);	// main.cpp's tree_state_value
#endif
	};
}
