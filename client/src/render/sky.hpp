#pragma once
#include <string>
#include <utility>
#include <vector>

#include "assets/resources.hpp"
#include "sokol_gfx.h"

namespace kdata { struct Record; class GameData; }

// Game clock, sun, atmosphere, lighting and sky drawing (milestone M3). Formulas and constants: docs/reference/SKY_EXE.md.

// Hours 0..24, advanced by speed x 0.0091666663 h per real second (24 h = 2618 s at speed 1). Speeds are 0 (pause), 1, 2, 5.
struct GameClock
{
	static constexpr double kHoursPerSecond = 0.0091666663;
	double hours = 12.0;
	int day = 0;
	double speed = 1;		// 1, 2 or 5 (the exe's "Normal", "x2", "x4"); other values only for testing
	bool paused = false;

	double rate() const { return paused ? 0.0 : speed * kHoursPerSecond; }
	void advance(double realDt) { setHours(hours + realDt * rate()); }
	void nudge(double dh) { setHours(hours + dh); }
	void setHours(double h);	// wraps into 0..24, counting days
	void cycleSpeed() { speed = speed == 1 ? 2 : speed == 2 ? 5 : 1; }
	void setSpeed(double s) { paused = s <= 0; if (s > 0) { speed = s; } }	// 0 pauses (speed kept); CLI --timescale, script
};

// The sky's own cloud density and colour mult (the exe's Sky+0x54 / +0x48; kenshi-inspect findings/sky_cloud_density_static.md, kenshi-rwh.26).
// One value pair on the sky, not a regional blend: fed only by the camera group's current WEATHER record (raw, not x strength). A new weather there, or the
// camera entering another group, retargets it with a linear velocity (target - current) / 30 integrated in game-speed seconds (30 s at x1, frozen while
// paused); a camera jump of more than sqrt(8000) ~ 89 u in one frame into another group snaps it (duration 0). The exe's start value is the save's or the
// ctor's 0.25; ours snaps on the first frame (no saved sky state).
struct SkyRamp
{
	static constexpr double kSeconds = 30.0;	// 14066cc80's duration from 1409dc4e0 (141693f4c)
	static constexpr double kJump2 = 8000.0;	// 1416cfae8: squared xz camera move per frame that snaps on a group change

	bool valid = false;
	float density = 0.25f;
	float colour[3] = { 1, 1, 1 };

	void retarget(float d, const float c[3], double seconds);	// 0 = snap
	void update(double gameDt);					// dt x game speed, 0 while paused

private:
	float vDensity_ = 0, vColour_[3] = {};
	double elapsed_ = 0, duration_ = 0;
	bool updating_ = false;
};

// GLOBAL CONSTANTS record fields the sky needs (merged game data).
struct SkyConstants
{
	float sunrise = 6.0f, sunset = 20.0f;	// hours; replaced by 6 / 20 when sunset <= sunrise
	float latitude = 0.0f;			// radians
	float nightDarkness = 0.5f;
	float exposureMin = 0.8f, exposureMax = 0.95f;
	std::string source = "defaults (GLOBAL CONSTANTS not loaded)";

	// Reads the merged game data (gamedata.base, Newwworld, Dialogue, rebirth). Falls back to the defaults above if the record is missing.
	static SkyConstants load(const std::string& dataDir);
};

// Weather hook: the values a WEATHER record gives the sky (SKY_EXE section 3, "Per-weather lighting"; SKY3_EXE Q2). Regional weather blends them across the
// camera's groups (weathermap.hpp), except `sky color mult` and `clouds density`, which follow SkyRamp below. The default is a fixed look: no weather fog,
// multiplier 1, density 0.25. `--weather NAME` loads a WEATHER record for comparison shots.
struct SkyWeather
{
	std::string name = "default";
	float skyColourMult[3] = { 1, 1, 1 };		// Weather "sky color mult" (sky+0x48, default 1,1,1): scales the zenith / nadir ambient samples
	float cloudsDensity = 0.25f;			// Weather "clouds density" (default 0.25)
	bool fogEnabled = false;			// Weather "fog enabled": the global weather fog (SharedFogParams) is off when false
	float fogColour[3] = { 1, 1, 1 };		// Weather "fog color" -> SharedFogParams.fogColour.rgb
	float fogDistance = 0;				// Weather "fog distance min" (the fog wind term is not applied): SharedFogParams.fogDensity = 1 / distance
	float wind[2] = { 1.0f, 1.0f };			// cloud wind velocity (SkyX layer default (1,1)); accumulated x game dt. The record's wind speed is not applied
	float windSpeed = 0;				// Weather "wind speed max" (m/s); live: lerp(min, max, strength), or the sim's WEATHER tail when present. Rain/dust drift and the wind sound
	float windSpeedMin = 0;				// Weather "wind speed min" (m/s): the live lerp's low end
	float windYaw = 0;				// live wind direction (character yaw: toward = (sin, cos), 0 = +Z); valid only when windValid
	bool windValid = false;				// the sim's WEATHER wind tail was present: windSpeed / windYaw are the camera group's live values (SIMLINK "WEATHER")
	float heatHaze = 0;				// Weather "heat haze" 0..1: carried and blended, no post effect yet
	float dust = 0;					// Weather "dust" -> dustAmount.x (surface dust coat on buildings, shaders/mesh.glsl dust_coat)
	float dustInside = 0;				// Weather "dust inside" -> dustAmount.y (interiors; the client has no interior flag, so unused by the shaders)
	float dustSlope = 0;				// Weather "dust slope" -> dustAmount.z (stickiness)
	float wetness = 0;				// Weather "wetness" -> shared param `wetness` (makeWet darkening, lighting.inc light_wet)
	float rain = 0;					// Weather "rain intensity" / 100 (the exe: rain = intensity x effect strength, WEATHER_LIGHT_EXE); scales the rain particle effects (particles.hpp)
	float windIntensity = 0;			// Weather "wind intensity" (0..100): audio only, the exe's Wwise Intensity = this x wind speed x 0.01 (audiohook.cpp)

	// WEATHER `effects` -> EFFECT `particle system` (ParticleUniverse script name), docs/CLIENT_GOALS.md "Particles". `type` is the effect class
	// (1 ash flakes, 2 steam, 3 dust cloud/twister, 4 sand stream/mist, 5 rain, 6 camera-cover, 7 lightning, 8, 9 beams/poison, 10 detritus).
	// Anchoring (kenshi-inspect world_effects §2): 1, 5 and 6 sit on the camera node, 4 is one handler on the player/view node, 2/7 are world
	// points, 3/8/9 wander in the biome's cells and 10 is a global point.
	// amount 0..1 scales the emission rate: rain (type 5) uses `rain`, the others the weather's `dust` (1 when it is 0): MINE, the exe's per-effect
	// strength is not traced. Blended weathers hold the weighted sum.
	enum Anchor { World, Camera, Player };
	struct Effect
	{
		std::string system;
		int type = 0;
		float amount = 1;
		bool windAffected = false;
		// The spawn recipe (world_effects §1/§3): the weather's effects ref and the EFFECT record.
		int cap = 0;				// ref v0: live-instance cap, 0 = none (the anchored types ignore the ref values)
		float delay = 0;			// ref v1: first spawn delay, and the low end of every later respawn (game-speed seconds)
		float respawnMax = 0;			// ref v2: respawn high end (game-speed seconds)
		float ttlMin = 0, ttlMax = 0;		// EFFECT min / max time to live (per instance)
		float strengthMin = 1, strengthMax = 1;	// EFFECT effect strength min / max (per instance, times the weather's ramping strength)
		float minAltitude = 0, maxAltitude = 0;	// EFFECT min / max altitude (the placement filter; also the type 4 y clamp)
		float minSlope = 0, maxSlope = 0;	// EFFECT min / max slope (the placement filter, normal.y)
		float wanderSpeed = 0;			// EFFECT wandering speed; the drift adds the weather wind (MINE until the sim sends its direction)
		float radius = 0;			// EFFECT effect radius: the spawn ring around a random world point
		float viewDistance = 0;			// EFFECT maximum view distance, 0 = no limit
		Anchor anchor() const { return type == 1 || type == 5 || type == 6 ? Camera : type == 4 ? Player : World; }
	};
	std::vector<Effect> effects;

	// Derived by finalize() (regional weather, WEATHER_MAP section of CLIENT.md): the fog the shaders actually use. Equals the record's fog when it is enabled; else the
	// exe's cloud-density haze (SKY_EXE C35, LIGHTING_EXE Q6 horizonClouds): stormDark = pow(sat(d - 0.5), 0.3) x sat(1.4 d - 0.3); the atmosphere fog colour itself now follows horizonClouds (sky.cpp, LIGHTING.md section 9); stormDark is only used as a client extension, becomes a global haze of density stormDark / kStormHazeDistance. Zero at d <= 0.5, so clear weathers are unchanged.
	float stormDark = 0;
	float fogRho = 0;				// effective global fog density (1 / distance), 0 = off
	float fogRaw[3] = { 1, 1, 1 };			// effective global fog colour (raw, before the light factor)
	static constexpr float kStormHazeDistance = 20000.0f;	// tunable, mine: full density at this distance x stormDark (the exe atmosphere fog runs 20000..38000)
	void finalize();

	// Weighted mix of weathers (weights need not sum to 1). Fog is mixed as density x colour so a clear region does not wash out a storm's colour.
	static SkyWeather blend(const std::vector<std::pair<const SkyWeather*, float>>& parts);
	static SkyWeather fromRecord(const kdata::Record& r, const kdata::GameData& gd);

	// Merged game data. Matches a WEATHER record by name or stringID (case-insensitive); false (and a message on stderr) when none matches.
	bool load(const std::string& dataDir, const std::string& recordName);
	// Every WEATHER record of the merged data, sorted by name (case-insensitive), one per name.
	static std::vector<SkyWeather> loadAll(const std::string& dataDir);
};

// Matches the `light_params` uniform block of shaders/lighting.inc (11 vec4 + the building lights); the shaders' generated structs are memcpy targets.
struct LightParams
{
	static constexpr int kMaxPoints = 16;	// MINE: the game draws every light in the frustum (deferred); the nearest 16 (WorldBuildings::fillLights)
	float v[11][4];
	float plHead[4];			// x = lights in pl, y = kReflectPass: the water reflection pass (lighting.inc reflect_pass: albedo luma, no light), z = water plane y - camera y (wet_under)
	static constexpr float kReflectPass = 1.0f;
	float pl[kMaxPoints * 4][4];		// lighting.inc light_points
	float sh[9][4] = {};			// lighting.inc amb_sh: the dynamic ambient's irradiance polynomial (Sky::ambientSH); sh[0][3] = its weight, 0 = the game's cube
};

// The sky's values for one frame's draw (the frame packet's sky block, render thread kenshi-k16.2.205.12.45): Sky::frame() takes them on the logic side
// after the frame's last change to the sky (logic_frame's end), Sky::render and the passes that light with the sky (the fog volumes) read them on the
// draw side. The pipeline, the textures and which of them loaded stay Sky's (render-owned).
struct SkyFrame
{
	LightParams light = {};			// Sky::light()
	double sunDir[3] = { 0, 1, 0 };		// Sky::sunDir(): unclamped
	float sunColour[3] = {};		// Sky::sunColour()
	double starScroll = 0;			// the star UV scroll
	double hours = 12.0;			// the clock: the moons' spin
	int day = 0;
	float skyColourMult[3] = { 1, 1, 1 };	// the weather's sky colour mult
	float cloudsDensity = 0.25f;		// the weather's clouds density
	bool clouds = true;			// Sky::cloudsOn
	double windAcc[2] = { 0, 0 };		// the accumulated cloud wind
	float minLum = 0.8f, maxLum = 1.2f;	// Sky::minLum() / maxLum(): the exposure's luminance range (hdr.hpp)
	float heatHaze = 0;			// the weather's heat haze (the draw zeroes it for the full world map; the clock's day and hours above give gameTime)
};

class Sky
{
public:
	// Creates the pipeline and loads the textures (starfield, cloud noise, two moons) through `res`; a missing texture is reported and drawn black.
	void init(const ResourceIndex& res);
	void shutdown();

	void setConstants(const SkyConstants& c) { k_ = c; recompute(); }
	const SkyConstants& constants() const { return k_; }
	// Advances the clock (real seconds) and recomputes sun, lighting and fog. fogDistance = the view distance (fog runs 0.8 .. 0.96 of it).
	void update(double realDt, float fogDistance, const double camPos[3]);
	void setSkyHours(double h) { skyHours_ = h; }

	// This frame's values for the draw (logic side; SkyFrame).
	SkyFrame frame() const;
	// Draws the sky of `s` (frame()) as the first thing in the pass. cam_* are the camera forward/right/up unit vectors; fovY is the vertical field of view.
	// viewHeight = framebuffer height in pixels (moon texture LOD). Reads only `s` and the render-owned resources.
	void render(const SkyFrame& s, const double f[3], const double r[3], const double u[3], float fovY, float aspect, int viewHeight) const;

	const LightParams& light() const { return light_; }
	const float* sunColour() const { return sunColour_; }	// the exe's sunColour.rgb before alpha (water.hlsl lights with it raw)
	float exposure() const { return exposure_; }	// the CPU estimate the world shaders bake in (light().v[4][3]); hdr.hpp corrects it to the GPU auto exposure
	float minLum() const { return minLum_; }	// MIN_LUMINANCE of this frame (the exe: exposureMin x (nightDarkness + (1 - nightDarkness) x sat(5 sunY)))
	float maxLum() const { return maxLum_; }	// MAX_LUMINANCE
	const double* sunDir() const { return sunReal_; }	// unclamped, world axes (+x east, y up, +z south)

	GameClock clock;
	SkyWeather weather;
	bool cloudsOn = true;
	float fogRhoForce = 0;	// > 0: replaces the weather's global fog density (scene key fogdist=)
	float ambientBlurKm = 0;	// > 0: ambientmap.png (BIOMES sun / ambient) is averaged over a Gaussian of this full width about the camera instead of read at it (regional weather)
	bool regionalFog = false;	// true: the world shaders take the global fog from the weather map (weathermap.hpp) per pixel instead of the uniform
	// Dynamic ambient (post.sky_ambient, ours; docs/CLIENT_RENDER_NEXT.md 3.1): weight 0..1 of the sky + ground bounce irradiance over the game's static cube.
	// groundAlbedo: the colour map under the camera (raw, as the terrain shades it); without it (hasGround false) the lower hemisphere is the sky floor only.
	float skyAmbient = 0;
	float moonlight = 0;	// post.moonlight: floor of the sky's irradiance at night, as a fraction of the game's cube (1 = its night level)
	// Logic thread only (kenshi-k16.2.205.12.56): written at logic_frame's camera update, read by ambientSH (update -> recompute, logic); the render
	// thread reads only the packet's copy (FramePacket::Scene groundOk / ground) and Sky::render reads neither.
	bool hasGround = false;
	float groundAlbedo[3] = {};

	// Debug text for the HUD / script log.
	std::string describe() const;

private:
	void recompute();
	void ambientSH(const double l[3]);	// fills light_.sh from the sky model and the ground bounce (skyAmbient > 0)
	void sampleAmbientMap(const double pos[3], float out[4]) const;	// ambientmap.png at a world position (bilinear, clamped): rgb, a = alpha x 2

	SkyConstants k_;
	LightParams light_ = {};
	double sunReal_[3] = { 0, 1, 0 };
	float sunColour_[3] = {};	// the exe's sunColour.rgb before alpha (the cloud layer reads it raw)
	float alpha_ = 1, exposure_ = 1, minLum_ = 0, maxLum_ = 0, sceneLum_ = 0;
	double starScroll_ = 0;		// star UV scroll (both axes), 0.05 x skyHours_
	double skyHours_ = 0, lastHours_ = -1;	// SkyX+0x7c: game hours played since start (clock jumps excluded); the clock total seen last frame
	double windAcc_[2] = { 0, 0 };	// accumulated wind (weather wind x game dt), scrolls the clouds
	float fogDist_ = 60000.0f;
	sg_buffer vbuf_ = {};
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_sampler smp_ = {};
	sg_image img_[6] = {};	// starfield, cloud tile noise, moon, moon 2, cloud density (Clouds.dds), cloud normals
	sg_view view_[6] = {};
	sg_image blackImg_ = {};
	sg_view blackView_ = {};
	bool hasStars_ = false, hasClouds_ = false;
	std::vector<unsigned char> ambMap_;	// ambientmap.png, 1024 x 1024 RGBA (empty = not found: multipliers 1)
	float ambMult_[4] = { 1, 1, 1, 1 };	// sampled at the camera by update()
};

// Sun direction per SKY_EXE section 1 (+x = east at sunrise, y up, z = latitude tilt = south in kclient's world axes).
void sky_sun_direction(double hours, double sunrise, double sunset, double latitude, double out[3]);
