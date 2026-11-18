#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "render/particles.hpp"
#include "render/sky.hpp"

struct WeatherAtlas;
namespace kworld { class Heightmap; }

// The weather's world instances (kenshi-inspect findings/weather_effects.md §1-§5, rwh81_weather_effects_placement.md, weather_lightning.md):
// every WEATHER `effects` ref of a world class (EFFECT types 2, 3, 7, 8, 9, 10) runs the exe's group tick: v0 = the live-instance cap (0 = none),
// v1 = the first delay and the minimum respawn, v2 = the maximum respawn, in game-speed seconds. Each instance draws its own time to live
// (EFFECT min/max, max <= 0 = eternal) and strength (EFFECT min/max). A spawn lands in a random cell of the owner biome group, on the terrain,
// filtered by the EFFECT altitude and slope; the wandering types (3/8/9) drift by their heading x wandering speed and the wind; lightning (7)
// lands near the nearest player character instead (spawnBolt). Type 10 (GlobalPoint) is a band refill of handlers on a disc around the camera
// pivot (rwh.81 §2): ~v0 inside the EFFECT maximum view distance M (0 = 1000), ~v0 more in [M, 2M], killed past 4M, no timer.
// Instances are handed out as placed particle systems at the world position (Particles::setPlaced), with stable keys. Visibility (W2d): each
// instance fades in at spawn, out over its last kFadeOut seconds, and towards the EFFECT maximum view distance (type 10: its 4M kill radius);
// on a weather change the old weather's instances fade out over kChangeFade seconds instead of vanishing. Lightning has none of those ramps
// (each bolt is ~1 s and the script's own colour curve is its flash).
class WeatherInstances
{
public:
	void clear();
	// dtGame: game-speed seconds (0 while paused = frozen; the type 10 refill runs every frame as the exe's, no timer). ownerGroup: the
	// WeatherAtlas group the weather belongs to (-1 = none). windX / windZ: the region wind in world units per second. cam: the camera
	// (instances further than the draw radius keep their record but are not drawn, the exe's "active cell" idea at our streaming scale).
	// pivot: the camera pivot (the type 10 disc centre). players: the player-faction characters' feet (SimView::isPlayer + placed), which
	// type 7 places around.
	void update(double dtGame, const SkyWeather& w, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
		float windX, float windZ, const double cam[3], const double pivot[3], const std::vector<std::array<double, 3>>& players);
	// The instances within the draw radius this frame. suppressedScripts: effects the sim draws itself (its BEAMS schedule of the burning
	// wandering type 9 effects, BeamField); the local instances of those scripts are kept but not handed out, so a beam is drawn once.
	std::vector<Particles::PlacedSystem> systems(const std::vector<std::string>& suppressedScripts = {}) const;
	std::string describe() const;				// `dump weather`: the live instances (system, position, age, ttl)
	size_t live() const;					// the region-wide instance records

private:
	struct Inst
	{
		double pos[3] = {};
		float heading[2] = {};		// wandering: the horizontal unit vector
		double age = 0, ttl = 0;	// ttl <= 0: eternal
		double reroll = 0;		// wandering: seconds to the next heading roll; lightning: to the next biome-cell check
		float strength = 1;
		float fade = 1;			// 0..1: the spawn ramp, the view-distance ramp and the end-of-life ramp, the lowest wins
		uint64_t key = 0;
		bool drawn = false;
		bool kill = false;		// lightning: the cell under the bolt left the owner biome group (the exe drops the psys)
	};
	struct Group
	{
		SkyWeather::Effect e;
		double timer = 0;		// the respawn countdown
		std::vector<Inst> inst;
	};
	struct Old	// a weather change: the previous weather's instances finish their fade rather than vanish
	{
		SkyWeather::Effect e;
		Inst in;
		float fade = 1;
	};
	std::vector<Group> groups_;
	std::vector<Old> olds_;
	std::mt19937 rng_{ 1 };
	std::string weather_;
	uint32_t serial_ = 0;
	void reset(const SkyWeather& w);
	void retire();		// moves the live instances to olds_ (a weather change)
	void init_instance(const Group& g, Inst& out, double x, double y, double z);	// the TTL / strength / heading rolls and the key at a placed point
	bool spawn(Group& g, Inst& out, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
		const std::vector<std::array<double, 3>>& players);
	bool spawnBolt(Group& g, Inst& out, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
		const std::vector<std::array<double, 3>>& players);	// type 7: the bolt lands near the nearest player character (weather_lightning.md)
	// Type 10 (rwh.81 §2): kill past 4M, then refill the inner disc and the annulus up to v0 handlers each, no timer.
	void refill_global_point(Group& g, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup, const double pivot[3]);
};
