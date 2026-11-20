#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net/simlink.hpp"
#include "render/sky.hpp"
#include "sokol_gfx.h"

// Regional weather (docs/CLIENT_GOALS.md "regional weather blended in space"). Data chain, all from the merged game data (see the .cpp header for the rule):
//   areasmap.tga pixel colour == BIOME_GROUP.index -> BIOME_GROUP.seasons -> SEASON.weathers -> WEATHER.

// CPU side, loadable on a worker thread: the areasmap as BIOME_GROUP indices (256^2 cells of 1152 world units) and one SkyWeather per group. Without a sim
// the group shows representativeWeather(); WeatherMap::setLive replaces it with the sim's (SIMLINK WEATHER 41).
struct WeatherAtlas
{
	static constexpr int kN = 256;
	static constexpr double kWorld = 294912.0;	// x and z in +-147456, as every other world map
	std::vector<std::string> groupNames;		// BIOME_GROUP names ("The Great Desert"); [0] = "" (no group)
	std::vector<std::string> groupIds;		// their stringIDs, as WEATHER 41 names them; [0] = ""
	std::vector<SkyWeather> groupWeather;		// what each group shows; [0] = the default weather (groups without seasons, ocean)
	std::map<std::string, SkyWeather> byId;		// every WEATHER record by stringID (the live weathers)
	std::vector<uint8_t> group;			// kN * kN indices into `groupNames`; row 0 = north (z = -147456), column 0 = x = -147456
	const std::string& groupAt(double x, double z) const;	// the area name at a world position (the zone banner), "" outside or unloaded
	int groupIndexAt(double x, double z) const;		// its group index, -1 outside (the weather world-instance owner fallback)
	// Each group's cells (col, row), for the weather effects' random spawn cell (kenshi-inspect findings/weather_effects.md §3).
	std::vector<std::vector<std::pair<uint16_t, uint16_t>>> groupCells;
	std::string source;				// what was loaded, or why it is empty

	// Reads the merged game data and `<dataDir>/newland/land/areasmap.tga`. Empty (group.empty()) with the reason in `source` when either is missing.
	// KWEATHERLOG=1 prints every group's chosen season and weather on stderr.
	static WeatherAtlas load(const std::string& dataDir);
};

// Shader binding of the weather map (one 256^2 RGBA16F texture: rgb = fog colour x density x 1e4, a = density x 1e4, premultiplied and blurred). A 1x1 dummy until a map exists.
struct FogMapBinding
{
	sg_view view = {};
	sg_sampler smp = {};
};
const FogMapBinding& fog_map_binding();

// Owns the blurred fog texture and answers the camera blend. setAtlas / setLive / blend are the logic half's; the fog texture is made and swapped
// by a queued job on the render side (renderqueue.hpp), which owns img_, view_ and the binding. init / shutdown run on the one thread there is then.
class WeatherMap
{
public:
	void init();			// the dummy texture + sampler; registers fog_map_binding()
	void shutdown();
	// Takes the atlas and builds the fog texture. frontBlurKm = Gaussian sigma of the blur in km (the softness of storm fronts in the distance).
	void setAtlas(WeatherAtlas&& a, float frontBlurKm);
	bool ready() const { return !atlas_.group.empty(); }
	// The sim's weather per group (WEATHER 41, docs/reference/WEATHER_CYCLE_LIVE.md section 4): fog lerps previous -> current by fogBlend, sky colour mult and
	// cloud density by skyBlend; rain, wetness, dust, heat haze and the effect amounts are the current weather's x strength. Rebuilds the fog map when the
	// fog changed. Groups not listed keep what they show.
	void setLive(const std::vector<simlink::Weather>& live, const simlink::SimLinkDecoder& dec);
	const WeatherAtlas& atlas() const { return atlas_; }

	// Stage 1: the weather around the camera = areasmap cells weighted by a Gaussian about `pos` (full width at half maximum = widthKm). `mix` gets the HUD text,
	// e.g. "Clear 70% / Dust Storm 30%" (up to three weathers of at least 1%).
	SkyWeather blend(const double pos[3], float widthKm, std::string& mix) const;

private:
	WeatherAtlas atlas_;
	std::map<std::string, uint8_t> groupIndex_;	// stringID -> group index
	float frontBlurKm_ = 0;
	void buildFogMap();	// logic side: the blurred map on the CPU, its image queued
	void makeFogMap(const std::vector<uint16_t>& half);	// render side
	sg_image dummy_ = {}, img_ = {};
	sg_view dummyView_ = {}, view_ = {};
	sg_sampler smp_ = {};
};
