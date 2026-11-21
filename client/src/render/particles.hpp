#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "assets/dds.hpp"
#include "sokol_gfx.h"
#include "render/sky.hpp"
#include "render/camera.hpp"

// far is an empty macro once <windows.h> is in scope (sokol_app.h and libc++'s <locale> include it); the member named far below is a plain
// identifier (kenshi-379.31.3, caught by the local llvm-mingw cross-check).
#undef near
#undef far

// Weather particle effects (docs/CLIENT_GOALS.md "Particles"): a small subset of Ogre ParticleUniverse, enough for the rain and dust scripts under data/particles/scripts.
// The .pu format and the semantics of its attributes follow the public ParticleUniverse documentation (Henry van Bakel / Kenshi's Ogre plugin, MIT); the parser and the
// simulation here are our own code. Per frame: simulate on the CPU (a few thousand billboards), write one instance buffer, draw instanced quads in their own pass after the fog
// volumes. The scene depth is sampled in the fragment shader (like fogvolume.hpp), so nothing is drawn behind the terrain.
class Particles
{
public:
	// dataDir = the game's data/ directory. Texture and material names are resolved under particles/.
	void init(const std::string& dataDir);
	void shutdown();

	// Replaces the active effects: the weather's camera-cover effects (EFFECT types 1 and 5: ash, rain), with the scene overrides (rain / dust >= 0 force that strength, see main.cpp
	// scene keys `rain=` `dust=`). Cheap when nothing changed. New systems are pre-warmed (simulated for their longest lifetime at a fixed step) so the first frame already shows a steady state.
	void setEffects(const std::vector<SkyWeather::Effect>& effects, float rainForce, float dustForce);
	// The placed world effects (ParticleWorld): every system it wants simulated this frame, with its world transform, a placement-derived
	// seed and the driver's 0..1 fade. Diffed by key, so a system leaving the list is removed and a surviving one keeps its particles.
	struct PlacedSystem
	{
		uint64_t key = 0;
		std::string script;
		double pos[3] = {};
		float quat[4] = { 1, 0, 0, 0 };	// the effect node's world orientation (the building's scene-node rotation x the EFFECT instance's)
		float fade = 1;
		float nodeScale = 1;	// the owning building's scene-node scale: positions the node only, never enters the particle math (rwh.155)
		uint32_t seed = 1;
		float amount = 1;		// emission rate multiplier: a weather instance's effect amount x its rolled strength
		bool prewarm = true;	// false: an event system (the weather's lightning bolt) plays from t = 0; a 1 s pre-warm would consume its one-shot emitter
		// The psys outlives the effect instance that made it (the lightning bolt, kenshi-k16.2.177.1): the exe removes the 1 s EFFECT
		// handler, stops the system and destroys it only when the last particle is gone, so the flash outlives its handler (rwh.120 1).
		// A system that leaves the list while it still has particles keeps running with emission off until the pools empty.
		bool outlive = false;
	};
	void setPlaced(const std::vector<PlacedSystem>& systems);
	// KBEAMSLOG: placed systems of these keys that no longer emit but still hold particles (the tail), and those particles
	size_t tailSystems(const std::vector<uint64_t>& keys, size_t* particles = nullptr, const double* cam = nullptr, double radius = 0, size_t* near = nullptr) const;
	// Clears every system and reseeds, so the next setEffects starts from the same state (scene tests).
	void reset(uint32_t seed);

	static constexpr double kCoverAhead = 94.0;	// MINE (measured on the live camera node): where the camera-cover effects (EFFECT types 1/5/6) sit along the view, and the free camera's pivot (k16.2.11.3)

	struct Frame
	{
		double cam[3];			// camera position (world)
		double view[3] = { 0, 0, 1 };	// camera forward (unit): the camera-cover effects sit kCoverAhead along it (kenshi-inspect world_effects §3)
		double anchor[3] = {};		// the player/view node (drawn feet, terrain-snapped): the type 4 handler rides it
		float groundY = 0;		// terrain height under the camera (rain dies here)
		float sunY = 1;			// unclamped sun height: the particle vertex colour is x clamp(5 max(y, 0) + 0.2, 0.1, 1) (materials/forward/basic.hlsl)
		float exposure = 1;		// baked exposure (light.v[4][3])
		float wind[2] = { 0, 0 };	// world x / z velocity added to wind affected systems (m/s)
		float dt = 0;			// seconds to simulate (0 = frozen, scripted runs)
		int warmSteps = 100;		// pre-warm steps one prepare may run across all new systems (kenshi-k16.2.437.4.4); a burst warms over frames
	};
	// One frame's instance data, in the frame packet slot (render hook 7a, kenshi-k16.2.205.12.14; double-buffered: one per packet).
	// prepare() fills it on the logic side; upload() and render() read only it.
	struct Batch
	{
		int tex;		// the technique's texture: index into FrameData::texNames (the render side makes the image, never prepare)
		bool additive, clip;
		int first, count;
		float soft;
	};
	struct FrameData
	{
		std::vector<float> inst;	// kInstFloats per particle, camera relative
		std::vector<Batch> batches;
		std::vector<std::string> texNames;	// particle texture names the batches use; upload() resolves them (prepare queues each name's create once)
		int count = 0;			// particles drawn (inst rows)
		bool depthTest = true;		// setDepthTest's value at prepare
		void clear() { inst.clear(); batches.clear(); texNames.clear(); count = 0; }
	};
	// Logic side: simulates and builds the instance data into `out`. Returns the number of particles drawn.
	int prepare(const Frame& f, FrameData& out);
	// Render side: writes the transient instance buffer. Call before any pass of the frame, every frame render() draws.
	void upload(const FrameData& d);

	// Draws inside the open particle pass (Hdr::particlePass). depth = the scene depth as a texture. mask = the interior mask for this view
	// (Hdr::maskTex; the zero view from noMaskView when no mask ran) - the materials with CLIP_INTERIOR drop the fragments inside a building.
	void render(const FrameData& d, const Camera& c, sg_view depth, sg_view mask);
	// The interior mask is needed while a clipped particle material is simulated (main renders it before this frame's particle pass).
	bool needsInteriorMask();
	// A 1x1 zero mask: reading it never clips (R and G 0).
	sg_view noMaskView() const { return zeroMask_; }

	bool active() const { return count_ > 0; }
	int count() const { return count_; }
	// The scene depth test: part_fs drops a fragment behind the scene depth. On unless a dev A/B (`set partdepth off`) turns it off
	// (kenshi-h2v.14.2.1: with it off the beam column fills whatever is behind it, so the two shots differ only by the test).
	void setDepthTest(bool on) { depthTest_ = on; }
	bool depthTest() const { return depthTest_; }
	// Dev probe (KCLIENT_DEV_TOOLS `probe depth`): the instance nearest a screen pixel (top-left origin), its v_w and its quad, for
	// the depth-readback diagnostics. covers = the pixel falls inside the quad.
	struct PixelHit
	{
		bool any = false, covers = false;
		double world[3] = {};	// the instance centre (world)
		float w = 0;			// v_w at the centre = dot(centre - camera, forward), the FS's v_w scale
		float sx = 0, sy = 0;	// the projected centre (pixels, top-left origin)
		float halfW = 0, halfH = 0;	// the projected quad's half extents (pixels)
	};
	PixelHit probePixel(const Camera& c, int px, int pyTop) const;	// dev-only (KCLIENT_DEV_TOOLS): reads probe_, a copy taken at the end of prepare (logic side)
	std::string describe() const;	// "Kenshi_Heavy_Rain x0.80 4160, Sand-Stream x0.50 310"
	// Dev (`dump beamfeet`, kenshi-h2v.14.9.1): the placed (world) systems and, per technique, the live particle count with its centroid
	// and farthest particle offset from the system's position, so a beam's foot fire can be measured against its foot.
	struct PlacedTech
	{
		std::string material;
		size_t count = 0;
		double centroid[3] = {};	// offset from the system's position
		double far = 0;			// the farthest particle's distance from the system's position
	};
	struct PlacedInfo
	{
		uint64_t key = 0;
		std::string script;
		double pos[3] = {};
		double time = 0;
		float fade = 1;
		std::vector<PlacedTech> techs;
	};
	std::vector<PlacedInfo> placedInfo() const;
	std::string placedReport() const;	// placedInfo() as text (one line per system, one per technique)

	// Slice 1 diagnostics: parses + caches a script by its `system` name and returns one line (path, techniques total / drawable /
	// disabled). Unknown .pu keys are logged once per script by loadScript. No materials or textures are loaded, nothing is drawn.
	std::string inspectScript(const std::string& name);

	// --- ParticleUniverse subset (public so the parser reads as plain data; nothing outside particles.cpp needs it) ---
	struct Dyn			// a dyn_* value: fixed, dyn_random min/max, dyn_curved_linear / spline of x (or t), dyn_oscillate
	{
		enum Kind { Fixed, Random, Linear, Spline, Oscillate } kind = Fixed;
		float a = 0, b = 0;				// fixed value / min, max
		std::vector<std::pair<float, float>> pts;	// control points sorted by x
		float osc[4] = { 0, 0, 0, 0 };			// frequency, phase, base, amplitude
		bool sine = true;
		float eval(float x, float rnd, float t) const;	// x = the curve abscissa (life fraction), rnd 0..1 for dyn_random, t = seconds for dyn_oscillate
		float maxX() const { return pts.empty() ? 1.0f : pts.back().first; }
		float sample(float rnd, float t) const { return eval(maxX() * rnd, rnd, t); }	// emission time value: curves are sampled at a random abscissa
	};
	struct Emitter
	{
		enum Kind { Box, Circle, Point, SphereSurface, Line } kind = Box;
		std::string name;		// the script's emitter name (an affector's exclude_emitter refers to it)
		Dyn rate = {}, ttl = {}, vel = {}, angle = {}, dimW = {}, dimH = {}, duration = {}, repeat = {};
		Dyn mass = {};
		bool hasMass = false;		// `mass`: the emitted particle's mass (PU default 1), only the Gravity affector reads it
		bool hasDim = false, hasDuration = false;
		// `repeat_delay`: the pause after `duration` before the emitter starts again. Absent, the emitter plays its duration once and never
		// again (rwh.175 2.6: SkyBeam's foot fire, `duration` 9 s and no repeat_delay, is over ~11 s into a handler that lives 10-30 s).
		bool hasRepeat = false;
		// SphereSurface: PU's `auto_direction` (default false) picks the emission direction. Set: the particle leaves along the sphere's outward
		// normal (deviated by `angle`). Clear: the emitter's own `direction` (default +y) deviated by `angle` -- a script with a narrow angle (the
		// streetlight fire jet: 10) is a cone, not a sphere. (ParticleUniverseSphereSurfaceEmitter::_initParticleDirection.)
		bool autoDirection = false;
		float pos[3] = {}, dir[3] = { 0, 1, 0 }, box[3] = { 100, 100, 100 }, radius = 100, col[4] = { 1, 1, 1, 1 };
		// Emission base colour: each particle takes a random colour between start and end when the script gives ranges, else the emitter's
		// `colour` (the fires give colour 0 0 0 0 and orange ranges; whether the game replaces or multiplies is an exe question, MINE).
		bool hasColourRange = false;
		// Line: particles are born along the segment from the emitter position to `end`, each one `min_increment`..`max_increment` further than the last
		// (back to the start past the end), shaken by up to `max_deviation` on every axis (kenshi_lightning_beam2).
		float end[3] = {}, minInc = 0, maxInc = 0, maxDev = 0;
		float colStart[4] = { 1, 1, 1, 1 }, colEnd[4] = { 1, 1, 1, 1 };
	};
	struct Affector
	{
		enum Kind { Colour, Scale, Rotator, Force, Vortex, Align, Gravity } kind = Colour;
		std::vector<std::pair<float, std::array<float, 4>>> timeColour;
		bool multiply = false;
		Dyn xyz = {}, x = {}, y = {}, z = {};
		bool hasXyz = false, hasX = false, hasY = false, hasZ = false;
		bool sinceStartSystem = false;	// `since_start_system`: the scale dyn's abscissa is system seconds, not the particle's life fraction
		Dyn rotation = {}, rotSpeed = {};
		float force[3] = {};
		// Gravity (kenshi-rwh.174): `gravity` mGravity (PU default 1), `mass_affector` (PU default 1, not 100), `position` = the
		// affector's local position in the technique's frame (the affector's own `scale` is unit in every .pu here)
		float gravity = 1, affMass = 1, gpos[3] = {};
		std::vector<std::string> exclude;	// `exclude_emitter` names, resolved to excludeIdx after the technique is read
		std::vector<int> excludeIdx;
		bool resize = false;			// Align: the billboard's height becomes the distance to the previous particle
		float axis[3] = { 0, 1, 0 };		// Vortex: the rotation axis, normalised
		Dyn vortexSpeed = {};			// Vortex: rotation_speed (rad/s, per particle)
	};
	struct Technique
	{
		std::string name, material;
		int quota = 1000;
		float defW = 100, defH = 100;
		bool oriented = false, bottomCentre = false;
		// billboard_type oriented_common: every billboard in the technique turns around one fixed axis (common_direction) to face the camera;
		// oriented_self turns each billboard around its own velocity. A zero commonDir (a script without common_direction) falls back to point.
		bool common = false;
		// billboard_type oriented_shape: the quad is not turned to the camera; it lies in the plane of the particle's own orientation (Ogre
		// BBT_ORIENTED_SHAPE). The Align affector sets that orientation (the particle's long axis points at the previous particle in the pool).
		bool shape = false;
		bool align = false, alignResize = false;	// the technique has an Align affector (and its `resize`)
		// `keep_local`: the particles are in the system's frame, so they move with the system node (a placed system whose position changes
		// each frame: SkyBeam's strip rides the wandering beam foot, rwh.175 2.6). Without it they stay where they were emitted.
		bool keepLocal = false;
		float commonDir[3] = { 0, 0, 0 };
		std::vector<Emitter> emitters;
		std::vector<Affector> affectors;
		float pos[3] = {};	// the technique's own `position`: every emitter of the technique is offset by it x the system scale
		bool killBelow = false;
		float killY = 0;
	};
	struct Script
	{
		std::string name, file;
		float scale[3] = { 1, 1, 1 };
		float scaleVelocity = 1;
		int techniquesTotal = 0, techniquesDisabled = 0;
		std::vector<Technique> techniques;
	};

private:
	struct Particle
	{
		double pos[3];
		float vel[3];
		float age, ttl;
		float w, h, rot, rotSpeed;
		float col[4];
		float seed;
		int emitter = 0;		// the emitter index (an affector's exclude_emitter)
		float mass = 1;		// the emitter's `mass`, sampled at emission (Gravity affector)
		float vortex = 0;		// the Vortex affector's rotation speed for this particle (rad/s, sampled at emission)
	};
	struct Sim
	{
		std::shared_ptr<Script> script;
		std::string name;
		float amount = 1;
		bool windAffected = false;
		int anchor = 0;			// SkyWeather::Anchor: 0 camera (the camera node + kCoverAhead along the view), 1 player (the frame's anchor point)
		float minAlt = 0, maxAlt = 0;	// the type 4 handler's y clamp (the EFFECT altitude range)
		std::vector<float> wrapR;	// per technique: the type 4 ParticleWrapExtern radius (1.5 x the largest emitter extent x the script scale, rwh.81 §3)
		double wrapDir[3] = {};		// the pivot's movement direction this step (the wrap moves a particle along it, not the wind)
		double prevAnchor[3] = {};
		bool hasPrev = false, wrapDirValid = false;
		std::vector<std::vector<Particle>> pools;	// one per technique
		struct EmitState { double acc = 0, phaseStart = 0, phaseEnd = 0, lineLen = 0; bool on = true, started = false; };
		std::vector<std::vector<EmitState>> em;		// per technique, per emitter
		double time = 0;
		bool warm = false;
		int warmSteps = 0;	// pre-warm steps still to run (spread over frames by kWarmStepsPerFrame, kenshi-k16.2.437.4.4)
		// A placed (world) system: particles are born at `pos` rotated by `quat`, its own RNG, and the driver's fade. Weather systems keep
		// the camera-local emitter and the shared rng_ so their shots stay identical.
		bool placed = false;
		bool prewarm = true;	// false: start empty and play from t = 0 (an event system; PlacedSystem::prewarm)
		bool emitting = true;	// false: the instance is gone; the leftover particles keep running (PlacedSystem::outlive)
		bool outlive = false;
		bool hasParticles() const { for (const std::vector<Particle>& p : pools) { if (!p.empty()) { return true; } } return false; }
		uint64_t key = 0;
		double pos[3] = {};
		float quat[4] = { 1, 0, 0, 0 };
		float nodeScale = 1;
		float fade = 1;
		std::mt19937 rng{ 1 };
	};
	struct Material
	{
		bool additive = false;
		bool ambient = true;		// Basic_Coloured_Ambient_VP: colour x clamp(5 sunY + 0.2, 0.1, 1) (weather dust/rain); flames use the plain VP
		bool clip = false;		// Basic_Texture_Clipped_FP_HLSL: drops the fragments inside buildings (the InteriorMask target, main.cpp)
		bool soft = false;		// Particle_Blend_Depth: the game's soft-particle fragment
		float colour[4] = { 1, 1, 1, 1 };	// fragment_program_ref param_named colour (sparks: float4 5 5 5 1)
		std::string texture;
	};
	struct Tex
	{
		sg_image image = {};
		sg_view view = {};
		bool ok = false;
	};

	void emit(Sim& s, int ti, int ei, const Frame& f, float dt);
	void step(Sim& s, const Frame& f, float dt);
	const Material* material(const std::string& name);
	// A technique texture's CPU data: read on the logic side the first time prepare() sees its name, made into an image by a queued job
	// (renderqueue.hpp, frame lane: before the draw of that packet).
	struct TexCpu
	{
		bool dds = false, ok = false;
		DdsImage image;			// dds
		int w = 0, h = 0;
		std::vector<unsigned char> rgba;	// png / tga, RGBA8
	};
	TexCpu loadTexture(const std::string& name) const;	// logic side, no GPU call
	void makeTexture(const std::string& name, const TexCpu& cpu);	// render side
	void requestTexture(const std::string& name);	// logic side: queues the create once per name
	std::string scriptPath(const std::string& name);	// `system` name -> .pu path (the filename differs for 14 scripts)
	std::shared_ptr<Script> loadScript(const std::string& name);

	std::string dir_;
	std::map<std::string, Material> materials_;
	bool materialsLoaded_ = false;
	std::map<std::string, std::shared_ptr<Script>> scripts_;
	std::map<std::string, std::string> scriptFiles_;
	bool scriptFilesScanned_ = false;
	std::map<std::string, Tex> textures_;	// render side (the queued creates fill it)
	std::set<std::string> requested_;	// logic side: names whose create is queued
	std::vector<Sim> sims_;
	bool depthTest_ = true;		// the FS depth test (setDepthTest)
	std::mt19937 rng_{ 1 };
	uint32_t seed_ = 1;

	std::vector<float> probe_;		// copy of the last prepare's instance rows for probePixel (dev, logic side; not a pointer into a packet slot)
	std::vector<const Tex*> resolved_;	// render side: texNames of the frame being uploaded -> Tex (upload() fills, render() reads)
	int count_ = 0;
	sg_shader shd_ = {};
	sg_pipeline pipAdd_ = {}, pipAlpha_ = {};
	sg_buffer quad_ = {}, instBuf_ = {};
	sg_sampler smp_ = {}, depthSmp_ = {};
	sg_view zeroMask_ = {};
	sg_image zeroMaskImg_ = {};
	Tex whiteTex_ = {};
};
