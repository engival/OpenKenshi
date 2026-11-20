// Venge's sky beams (docs/reference/VENGE_BEAMS_STATIC.md, SIMLINK "BEAMS", beams.hpp):
//
//  1. the schedule: the Venge season picks `venge beams` only inside its 5-23 window, the instance ends no later than 23:00, and an
//     hour with nothing eligible is the null "off" weather for 5 to 60 minutes;
//  2. the field: 40 handlers of the SkyBeam EFFECT, every beam inside the group's cells, the segments covering the life exactly, the
//     incremental position equal to beamPositionAt every tick, each beam spawning where the last ended, a beam clipped to its limit;
//  3. determinism: the same seed gives the same beams, another does not, a save and load mid-beam gives the same beams, the weather's
//     end (23:00) ends them;
//  4. the frame (BEAMS 58): decoded by hand; a beam goes out exactly 3 s before it strikes, once per session; a beam out of range of the
//     session goes out later, once, when the camera comes within range; a replan (a pin) and a load reset the client's copy;
//  5. the burn: 10 x dt x strength x (1 - P3) per organic part at a beam's centre, the ring's falloff, none outside, none indoors, none
//     for a race immune to type 3; the burn is part health (the wear and stun pools stay 0);
//  6. rwh.175: the visible burst (6.0 to 10.6 s) vs the damage window (10 to 30 s), the loaded-cell population (~7 of 40, 0 to 2 on
//     screen, 2 to 3 new per minute in view).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/world/beams.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/weather.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr std::uint32_t kVengeColour = 6566400; // BIOME_GROUP `index` of Venge (the group whose season lists `venge beams`)

struct Venge
{
	std::size_t group = 0;
	std::string groupSid;
	std::string weatherSid; // `venge beams`
	const EffectDef* effect = nullptr;
};

// The group, the weather and the EFFECT, found by the data's shape: the group with colour 6566400, its weather with an hour window and a
// burning wandering effect.
Venge findVenge(const WeatherModel& model)
{
	Venge venge;
	for (std::size_t g = 0; g < model.groups().size(); ++g)
	{
		if (model.groups()[g].colour != kVengeColour)
			continue;
		venge.group = g;
		venge.groupSid = model.groups()[g].sid;
		for (const SeasonDef& season : model.groups()[g].seasons)
			for (const SeasonWeather& entry : season.weathers)
			{
				const WeatherDef* def = model.weather(entry.weather);
				for (const EffectRef& ref : def->effects)
				{
					const EffectDef* effect = model.effect(ref.effect);
					if (effect != nullptr && effect->type == kEffectWandering && effect->affectType == kAffectBurn)
					{
						venge.weatherSid = entry.weather;
						venge.effect = effect;
					}
				}
			}
	}
	test::require(!venge.weatherSid.empty(), "no Venge weather with a burning wandering effect");
	return venge;
}

// ---- 1. the schedule ----

void testSchedule(const WeatherModel& model, const Venge& venge)
{
	const WeatherDef* beams = model.weather(venge.weatherSid);
	test::require(beams->startHour == 5.0 && beams->endHour == 23.0, "venge beams is not the 5-23 window");
	test::require(beams->effects.size() == 1 && beams->effects[0].count == 40, "venge beams does not list 40 handlers");
	test::require(venge.effect->ttlMin == 10.0 && venge.effect->ttlMax == 30.0 && venge.effect->speed == 130.0
			&& venge.effect->radius == 200.0 && venge.effect->strengthMin == 0.75 && venge.effect->strengthMax == 0.75
			&& venge.effect->particles == "SkyBeam" && venge.effect->sound == "Inferno",
		"the SkyBeam EFFECT is not the merged record of the oracle note");

	int day = 0;
	int night = 0;
	int clamped = 0;
	int picks = 0;
	for (std::uint64_t seed = 1; seed <= 300; ++seed)
	{
		// Night (02:00): never venge beams, whatever the seed; and the pick (dust swirls) is inside its own window.
		state::WeatherRegion region = model.freshRegion(venge.group, seed);
		model.step(venge.group, region, tickAt(3, 2, 0));
		test::require(region.weather != venge.weatherSid, "venge beams picked at 02:00, seed " + std::to_string(seed));
		night += !region.weather.empty();
		// Day (10:00).
		region = model.freshRegion(venge.group, seed);
		model.step(venge.group, region, tickAt(3, 10, 0));
		day += region.weather == venge.weatherSid;
		// Late afternoon (20:00): venge beams ends no later than 23:00 (the 60..480 minute lifetime would run past it).
		region = model.freshRegion(venge.group, seed);
		model.step(venge.group, region, tickAt(3, 20, 0));
		if (region.weather == venge.weatherSid)
		{
			++picks;
			test::require(region.endMinute <= 3 * 1440 + 23 * 60, "venge beams runs past 23:00, seed " + std::to_string(seed));
			clamped += region.endMinute == 3 * 1440 + 23 * 60;
		}
	}
	std::cout << "schedule: night picks " << night << "/300, day venge " << day << "/300, 20:00 venge " << picks << " (" << clamped << " cut at 23:00)\n";
	test::require(night > 0, "nothing picked at night");
	test::require(day > 30, "venge beams is (almost) never picked by day");
	test::require(clamped > 0, "no 20:00 pick was cut at 23:00");

	// Nothing eligible: the null weather for 5 to 60 minutes (a season with the one windowed weather, at 02:00).
	WeatherDef only;
	only.sid = "W";
	only.startHour = 5.0;
	only.endHour = 23.0;
	SeasonDef season;
	season.sid = "S";
	season.order = 0;
	season.lengthWeight = 1;
	season.weathers.push_back({"W", 1.0, 60, 120});
	WeatherGroupDef group;
	group.sid = "G";
	group.seasons.push_back(season);
	const WeatherModel hand({only}, {group}, 100);
	for (std::uint64_t seed = 1; seed <= 40; ++seed)
	{
		state::WeatherRegion region = hand.freshRegion(0, seed);
		hand.step(0, region, tickAt(3, 2, 0));
		const std::uint64_t minutes = region.endMinute - region.startMinute;
		test::require(region.weather.empty() && minutes >= 5 && minutes <= 60, "the off weather is not 5 to 60 minutes, seed " + std::to_string(seed));
	}
	// And by day the same model picks W.
	state::WeatherRegion region = hand.freshRegion(0, 1);
	hand.step(0, region, tickAt(3, 10, 0));
	test::require(region.weather == "W", "the hand model did not pick W by day");
}

// ---- 2 and 3. the field ----

std::unique_ptr<Simulation> makeSim(const std::shared_ptr<const kdata::GameData>& gd, std::uint64_t seed, std::uint64_t clock)
{
	auto sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
	sim->setGameData(gd);
	sim->setClockTick(clock);
	sim->enableWeather(*gd);
	return sim;
}

// Every slot's chain state in one string: equal strings = equal beams.
std::string describe(const BeamField& field)
{
	std::ostringstream out;
	out.precision(9);
	for (const BeamGroup& group : field.groups())
	{
		out << group.key.weather << '/' << group.key.startMinute << '/' << group.limitTick << ':' << group.slots.size() << ';';
		for (const BeamSlot& slot : group.slots)
		{
			for (const Beam* beam : {&slot.current, &slot.next})
			{
				out << beam->id << ',' << beam->spawnTick << ',' << beam->endTick << ',' << beam->x << ',' << beam->z << ',' << beam->strength;
				for (const BeamSegment& s : beam->segments)
					out << ',' << s.ticks << '@' << s.yaw;
				out << '|';
			}
			out << slot.px << ',' << slot.pz << ',' << slot.posTick << ';';
		}
	}
	return out.str();
}

const BeamGroup& vengeGroup(const Simulation& sim, const Venge& venge)
{
	return sim.beams()->groups()[venge.group];
}

void checkBeam(const Beam& beam, const Venge& venge, const AreaLookup& areas, const std::string& where)
{
	std::uint64_t sum = 0;
	for (const BeamSegment& s : beam.segments)
	{
		test::require(s.ticks > 0, where + ": an empty segment");
		sum += s.ticks;
	}
	test::require(sum == beam.endTick - beam.spawnTick, where + ": the segments do not cover the life");
	test::require(beam.endTick > beam.spawnTick, where + ": no life");
	test::require(beam.speed == 130.0f && beam.radius == 200.0f && beam.strength == 0.75f, where + ": speed, radius or strength");
	// The start lies in a zone cell of the group (the cell's centre colour, the cell's 10 % inset).
	const kworld::ZoneCell cell = kworld::zoneCellOf(beam.x, beam.z);
	const double cx = kworld::zoneMinX(cell.x) + 0.5 * kworld::kZoneSize;
	const double cz = kworld::zoneMinZ(cell.z) + 0.5 * kworld::kZoneSize;
	test::require(areas.colourAt(cx, cz) == kVengeColour, where + ": the start is not in a Venge cell");
	const double fx = (beam.x - kworld::zoneMinX(cell.x)) / kworld::kZoneSize;
	const double fz = (beam.z - kworld::zoneMinZ(cell.z)) / kworld::kZoneSize;
	test::require(fx >= 0.0999 && fx <= 0.9001 && fz >= 0.0999 && fz <= 0.9001, where + ": the start is not inside the 10 % inset");
	(void)venge;
}

void testField(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	const std::uint64_t start = tickAt(3, 10, 0);
	auto sim = makeSim(gd, 7, start);
	sim->run(1);
	test::require(sim->beams() != nullptr, "no beam field");
	// Not pinned: the season's own pick at 10:00 may or may not be venge beams; pin it.
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	sim->run(1);
	const AreaLookup& areas = *sim->weather()->areas();
	test::require(vengeGroup(*sim, venge).slots.size() == 40, "not 40 handlers: " + std::to_string(vengeGroup(*sim, venge).slots.size()));
	for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
	{
		checkBeam(slot.current, venge, areas, "first beam");
		checkBeam(slot.next, venge, areas, "next beam");
		const std::uint64_t life = slot.current.endTick - slot.current.spawnTick;
		test::require(life >= 200 && life <= 600, "a beam lives " + std::to_string(life) + " ticks, not 10..30 s");
		test::require(slot.next.spawnTick == slot.current.endTick && slot.next.id == slot.current.id + 1, "the next beam does not follow");
	}
	// Different handlers start in different places (a hash of the slot, not one draw).
	const BeamGroup& group = vengeGroup(*sim, venge);
	test::require(group.slots[0].current.x != group.slots[1].current.x || group.slots[0].current.z != group.slots[1].current.z, "two slots start together");

	// The onset is staggered over the first 30 s (h2v.14.14): run past it, then every slot has a beam alive.
	sim->run(601);
	// Run 80 s: the incremental position is the pure function's, every tick; every beam hands over to its planned successor.
	std::size_t handovers = 0;
	std::vector<std::uint32_t> ids;
	for (const BeamSlot& slot : group.slots)
		ids.push_back(slot.current.id);
	for (int t = 0; t < 1600; ++t)
	{
		sim->run(1);
		const BeamGroup& now = vengeGroup(*sim, venge);
		for (std::size_t s = 0; s < now.slots.size(); ++s)
		{
			const BeamSlot& slot = now.slots[s];
			test::require(slot.posTick == sim->clockTick() || slot.posTick == slot.current.endTick, "a slot's position is not at this tick");
			float x = 0.0f;
			float z = 0.0f;
			beamPositionAt(slot.current, slot.posTick, x, z);
			test::require(x == slot.px && z == slot.pz, "the incremental position differs from beamPositionAt at tick " + std::to_string(slot.posTick));
			test::require(slot.current.spawnTick <= sim->clockTick() && sim->clockTick() < slot.current.endTick, "no beam alive in a slot");
			if (slot.current.id != ids[s])
			{
				++handovers;
				test::require(slot.current.id == ids[s] + 1, "a beam id skipped");
				test::require(slot.current.spawnTick <= sim->clockTick(), "a handed-over beam is in the future");
				ids[s] = slot.current.id;
			}
		}
	}
	std::cout << "field: 40 handlers, " << handovers << " handovers in 80 s\n";
	test::require(handovers > 40, "the beams do not hand over");

	// A beam's burn: full inside the radius, falling off to 0 at radius x sqrt 2.
	const BeamSlot& slot = vengeGroup(*sim, venge).slots[0];
	int type = 0;
	test::require(sim->beams()->burnAt(slot.px, slot.pz, type) == 0.75 && type == 3, "no full burn at a beam's centre");
	test::require(std::fabs(sim->beams()->burnAt(slot.px + 250.0, slot.pz, type) - 0.75 * (1.0 - 22500.0 / 40000.0)) < 1.0e-5,
		"the ring's burn is not linear in the squared distance");
	test::require(sim->beams()->burnAt(slot.px + 290.0, slot.pz, type) < 1.0e-9,
		"a beam burns beyond radius x sqrt 2");

	// Clipping: a limit inside the life cuts the beam, and the segments still cover it.
	const BeamPlanKey key{7, venge.groupSid, venge.weatherSid, 123};
	const std::vector<int> cells{0};
	const Beam full = planBeam(key, *venge.effect, cells, 3, 5, 1000, 1000000);
	const Beam cut = planBeam(key, *venge.effect, cells, 3, 5, 1000, 1100);
	test::require(full.endTick - full.spawnTick >= 200 && cut.endTick == 1100 && cut.spawnTick == 1000, "the clip is wrong");
	std::uint64_t sum = 0;
	for (const BeamSegment& s : cut.segments)
		sum += s.ticks;
	test::require(sum == 100, "a clipped beam's segments do not sum to its life");
	// The cut beam is the full beam's prefix: same start, same heading.
	test::require(cut.x == full.x && cut.z == full.z && cut.segments.front().yaw == full.segments.front().yaw, "the clip changed the start or heading");
	test::require(planBeam(key, *venge.effect, cells, 3, 5, 1000, 1000).effect == nullptr, "a beam with no room exists");
	test::require(planBeam(key, *venge.effect, {}, 3, 5, 1000, 5000).effect == nullptr, "a beam with no cells exists");
}

void testDeterminism(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	const std::uint64_t start = tickAt(3, 10, 0);
	auto a = makeSim(gd, 21, start);
	auto b = makeSim(gd, 21, start);
	auto c = makeSim(gd, 22, start);
	for (auto* sim : {a.get(), b.get(), c.get()})
	{
		sim->run(1);
		sim->pinWeather(venge.groupSid, venge.weatherSid);
		sim->run(500);
	}
	test::require(describe(*a->beams()) == describe(*b->beams()), "the same seed gave different beams");
	test::require(describe(*a->beams()) != describe(*c->beams()), "two seeds gave the same beams");

	// Save mid-beam, load, and the loaded run re-derives the same beams (nothing about them is saved).
	std::ostringstream saved;
	a->save(saved);
	std::istringstream in(saved.str());
	Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, gd);
	test::require(loaded.beams() != nullptr, "a loaded save lost the beam field");
	test::require(loaded.stateHash() == a->stateHash(), "the state hash changed across save and load");
	a->run(1);
	loaded.run(1);
	test::require(describe(*loaded.beams()) == describe(*a->beams()), "the beams after save and load differ");
	a->run(700);
	loaded.run(700);
	test::require(describe(*loaded.beams()) == describe(*a->beams()), "the beams after save, load and continue differ");
	test::require(loaded.stateHash() == a->stateHash(), "the state hash after save, load and continue differs");
}

// The weather's own end ends the beams: a natural venge beams instance cut at 23:00 (the clamp), then none.
void testDayEnd(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	std::uint64_t seed = 0;
	{
		auto probe = makeSim(gd, 1, tickAt(3, 20, 0));
		const WeatherModel& model = probe->weather()->model();
		for (std::uint64_t s = 1; s <= 300 && seed == 0; ++s)
		{
			state::WeatherRegion region = model.freshRegion(venge.group, s);
			model.step(venge.group, region, tickAt(3, 20, 0));
			if (region.weather == venge.weatherSid && region.endMinute == 3 * 1440 + 23 * 60)
				seed = s;
		}
	}
	test::require(seed != 0, "no seed with venge beams cut at 23:00");
	auto sim = makeSim(gd, seed, tickAt(3, 20, 0));
	sim->run(1);
	const std::uint64_t end = tickAt(3, 23, 0);
	test::require(vengeGroup(*sim, venge).key.weather == venge.weatherSid, "the instance is not venge beams");
	test::require(vengeGroup(*sim, venge).limitTick == end, "the limit is not 23:00");
	while (sim->clockTick() + 1 < end)
	{
		sim->run(std::min<std::uint64_t>(end - 1 - sim->clockTick(), 97));
		for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
			test::require(slot.current.endTick <= end && slot.next.endTick <= end, "a beam outlives 23:00");
	}
	double inside = 0.0;
	for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
	{
		int type = 0;
		inside = std::max(inside, sim->beams()->burnAt(slot.px, slot.pz, type));
	}
	test::require(inside > 0.0, "nothing burns just before 23:00");
	sim->run(3);
	test::require(vengeGroup(*sim, venge).key.weather.empty() || vengeGroup(*sim, venge).key.weather != venge.weatherSid,
		"the instance is still venge beams after 23:00");
	for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
		test::require(slot.current.effect == nullptr, "a beam is alive after 23:00");
	std::cout << "day end: seed " << seed << ", beams gone at 23:00\n";
}

// ---- 4. the frame ----

struct Buf
{
	std::vector<std::uint8_t> bytes;
	template <typename T>
	Buf& put(T v)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
		bytes.insert(bytes.end(), p, p + sizeof(T));
		return *this;
	}
};

struct Cursor
{
	const std::vector<std::uint8_t>& p;
	std::size_t at = 0;
	template <typename T>
	T get()
	{
		test::require(at + sizeof(T) <= p.size(), "frame overrun");
		T v;
		std::memcpy(&v, &p[at], sizeof(T));
		at += sizeof(T);
		return v;
	}
};

struct WireBeam
{
	std::uint32_t id = 0;
	std::string effect;
	std::uint64_t spawnTick = 0;
	std::uint64_t endTick = 0;
	std::uint16_t visibleTicks = 0;
	float x = 0, z = 0, speed = 0, radius = 0, strength = 0;
	std::string particles;
	std::string sound;
	std::vector<BeamSegment> segments;
};

struct WireGroup
{
	std::string group;
	std::uint8_t flags = 0;
	std::vector<WireBeam> beams;
};

struct WireFrame
{
	std::uint64_t tick = 0;
	std::vector<WireGroup> groups;
};

// One session on a simulation, decoding every frame 58 (u64 tick, u16 nGroups x {strid group, u8 flags, u16 n x {...}}) with the string
// table the stream carried.
struct Watch
{
	Simulation& sim;
	ks::ServerContext& context;
	std::unique_ptr<ks::Session> session;
	std::vector<std::string> strings; // id - 1
	std::vector<WireFrame> frames;
	std::uint32_t orderSeq = 500;

	Watch(Simulation& s, ks::ServerContext& c)
		: sim(s)
		, context(c)
	{
		connect();
	}

	std::string text(std::uint32_t id) const { return id == 0 ? std::string() : strings.at(id - 1); }

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (std::size_t at = 0; at < bytes.size();)
		{
			std::uint32_t len = 0;
			std::uint16_t type = 0;
			std::memcpy(&len, &bytes[at], 4);
			std::memcpy(&type, &bytes[at + 4], 2);
			const std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(at + 6), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
			at += 4 + len;
			Cursor c{payload};
			if (type == ks::kStrtab)
			{
				c.get<std::uint32_t>();
				const std::uint32_t n = c.get<std::uint32_t>();
				for (std::uint32_t i = 0; i < n; ++i)
				{
					c.get<std::uint8_t>();
					const std::uint16_t size = c.get<std::uint16_t>();
					std::string value;
					for (std::uint16_t k = 0; k < size; ++k)
						value += static_cast<char>(c.get<std::uint8_t>());
					strings.push_back(value);
				}
			} else if (type == ks::kBeams) {
				WireFrame frame;
				frame.tick = c.get<std::uint64_t>();
				const std::uint16_t groups = c.get<std::uint16_t>();
				for (std::uint16_t g = 0; g < groups; ++g)
				{
					WireGroup group;
					group.group = text(c.get<std::uint32_t>());
					group.flags = c.get<std::uint8_t>();
					const std::uint16_t n = c.get<std::uint16_t>();
					for (std::uint16_t b = 0; b < n; ++b)
					{
						WireBeam beam;
						beam.id = c.get<std::uint32_t>();
						beam.effect = text(c.get<std::uint32_t>());
						beam.spawnTick = c.get<std::uint64_t>();
						beam.endTick = c.get<std::uint64_t>();
						beam.visibleTicks = c.get<std::uint16_t>();
						beam.x = c.get<float>();
						beam.z = c.get<float>();
						beam.speed = c.get<float>();
						beam.radius = c.get<float>();
						beam.strength = c.get<float>();
						beam.particles = text(c.get<std::uint32_t>());
						beam.sound = text(c.get<std::uint32_t>());
						const std::uint16_t segments = c.get<std::uint16_t>();
						for (std::uint16_t k = 0; k < segments; ++k)
						{
							BeamSegment segment;
							segment.ticks = c.get<std::uint16_t>();
							segment.yaw = c.get<float>();
							beam.segments.push_back(segment);
						}
						group.beams.push_back(std::move(beam));
					}
					frame.groups.push_back(std::move(group));
				}
				test::require(c.at == payload.size(), "frame 58 has trailing bytes");
				frames.push_back(std::move(frame));
			}
		}
	}

	void connect()
	{
		session = std::make_unique<ks::Session>(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash);
		hello.put<std::uint16_t>(4).put<std::uint8_t>('t').put<std::uint8_t>('e').put<std::uint8_t>('s').put<std::uint8_t>('t');
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		collect(bytes);
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
		{
			sim.run(1);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			collect(bytes);
		}
	}

	// Order 51 set_view (u32 seq, u16 kind, f32 x, f32 z).
	void setView(float x, float z)
	{
		Buf order;
		order.put<std::uint32_t>(++orderSeq).put<std::uint16_t>(51).put<float>(x).put<float>(z);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, order.bytes.data(), order.bytes.size(), w), "set_view refused");
		collect(bytes);
	}
};

// The venge group's beams of one frame, by id.
std::size_t beamsIn(const WireFrame& frame, const std::string& group)
{
	std::size_t n = 0;
	for (const WireGroup& g : frame.groups)
		if (g.group == group)
			n += g.beams.size();
	return n;
}

// The distance from a point to a beam's path, sampled every 10 ticks (the test's own, not the server's polyline).
double sampledDistance(const Beam& beam, double px, double pz)
{
	double best = 1.0e18;
	for (std::uint64_t t = beam.spawnTick; t <= beam.endTick; t += 10)
	{
		float x = 0.0f;
		float z = 0.0f;
		beamPositionAt(beam, t, x, z);
		best = std::min(best, std::hypot(px - x, pz - z));
	}
	return best;
}

void testFrame(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	ks::ServerContext context;
	context.gameData = gd;
	context.dataHash = ks::dataHash(kworld::vanillaLoadOrder(KENSHI_DATA));
	auto sim = makeSim(gd, 33, tickAt(3, 10, 0));
	sim->run(1);
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	sim->run(1);
	const BeamGroup& group = vengeGroup(*sim, venge);

	// A session with no camera and no characters: the connect frame resets every group and carries no beam.
	Watch far(*sim, context);
	test::require(far.frames.size() == 1, "no frame 58 on connect");
	for (const WireGroup& g : far.frames[0].groups)
		test::require(g.flags == 1 && g.beams.empty(), "the connect frame carries a beam to a session that sees nowhere");
	test::require(far.frames[0].groups.size() == sim->weather()->model().groups().size(), "the connect frame misses a group");

	// A camera well out of range of every beam (the corner of the world farthest from the group).
	float farX = 0.0f;
	float farZ = 0.0f;
	double farBest = 0.0;
	for (const float cx : {-120000.0f, 120000.0f})
		for (const float cz : {-120000.0f, 120000.0f})
		{
			double nearest = 1.0e18;
			for (const BeamSlot& slot : group.slots)
				nearest = std::min(nearest, std::min(sampledDistance(slot.current, cx, cz), sampledDistance(slot.next, cx, cz)));
			if (nearest > farBest)
			{
				farBest = nearest;
				farX = cx;
				farZ = cz;
			}
		}
	std::cout << "frame: the farthest corner is " << farBest << " u from every beam\n";
	test::require(farBest > kBeamRange + 500.0, "no corner of the world is out of range of every beam");
	far.setView(farX, farZ);
	far.frames.clear();
	far.tick(1500); // 75 s: every handler hands over at least twice
	for (const WireFrame& frame : far.frames)
		test::require(beamsIn(frame, venge.groupSid) == 0, "a beam went to a session whose camera is out of range");

	// The camera comes into range of slot 5's beam: the next tick sends what is in range and inside the lead, once.
	Watch near(*sim, context);
	near.setView(farX, farZ);
	near.tick(2);
	near.frames.clear();
	const BeamSlot& slot = group.slots[5];
	near.setView(slot.px + 1000.0f, slot.pz);
	near.tick(1);
	test::require(!near.frames.empty() && beamsIn(near.frames.back(), venge.groupSid) > 0, "a beam in range later was not sent on the next tick");
	test::require(near.frames.back().groups.front().flags == 0 || near.frames.back().groups.size() > 0, "frame shape");
	std::set<std::uint32_t> seen;
	std::size_t lead60 = 0;
	std::size_t checked = 0;
	// `late`: the frame where the camera came into range, so the beams already inside their lead go out with less than 3 s.
	const auto take = [&](const WireFrame& frame, bool late)
	{
		for (const WireGroup& g : frame.groups)
		{
			if (g.group != venge.groupSid)
				continue;
			test::require((g.flags & 1) == 0, "an unexpected reset at tick " + std::to_string(frame.tick));
			for (const WireBeam& b : g.beams)
			{
				test::require(seen.insert(b.id).second, "beam " + std::to_string(b.id) + " was sent twice");
				test::require(b.spawnTick <= frame.tick + kBeamLeadTicks, "a beam went out more than 3 s early");
				test::require(b.endTick > frame.tick, "a dead beam was sent");
				if (b.spawnTick > frame.tick)
				{
					test::require(late ? b.spawnTick - frame.tick <= kBeamLeadTicks : b.spawnTick - frame.tick == kBeamLeadTicks,
						"a planned beam did not go out 3 s ahead");
					lead60 += !late;
				}
				// The wire is the sim's beam: same id, same path.
				const BeamSlot& owner = group.slots[b.id >> 16];
				const Beam* mine = owner.current.id == b.id ? &owner.current : (owner.next.id == b.id ? &owner.next : nullptr);
				if (mine != nullptr)
				{
					++checked;
					test::require(b.effect == venge.effect->sid && b.particles == "SkyBeam" && b.sound == "Inferno", "the wire's effect, particles or sound");
					test::require(b.spawnTick == mine->spawnTick && b.endTick == mine->endTick && b.visibleTicks == mine->visibleTicks && b.x == mine->x && b.z == mine->z
							&& b.speed == mine->speed && b.radius == 200.0f && b.strength == 0.75f && b.segments.size() == mine->segments.size(),
						"the wire's beam differs from the sim's");
					for (std::size_t i = 0; i < b.segments.size(); ++i)
						test::require(b.segments[i].ticks == mine->segments[i].ticks && b.segments[i].yaw == mine->segments[i].yaw, "a wire segment differs");
				}
			}
		}
	};
	for (const WireFrame& frame : near.frames)
		take(frame, true);
	near.frames.clear();
	for (int t = 0; t < 1200; ++t)
	{
		near.tick(1);
		for (const WireFrame& frame : near.frames)
			take(frame, false);
		near.frames.clear();
	}
	std::cout << "frame: " << seen.size() << " beams sent once each, " << lead60 << " exactly 3 s ahead, " << checked << " matched against the sim\n";
	test::require(lead60 > 0 && checked > 0, "no planned beam went out");

	// A replan (a pin) resets the group at once: flags bit 0 and what is in range.
	near.frames.clear();
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "second pin failed");
	near.tick(1);
	bool reset = false;
	for (const WireFrame& frame : near.frames)
		for (const WireGroup& g : frame.groups)
			reset = reset || (g.group == venge.groupSid && (g.flags & 1) != 0);
	test::require(reset, "a replan did not reset the client's beams");
}

// ---- 5. the burn ----

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const kworld::LoadOrder& order)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		sim->removeFighter(duel.enemy);
		sim->tick();
	}

	CombatArena& arena() { return *const_cast<CombatArena*>(sim->combat()); }
	state::Fighter& player() { return *arena().find(duel.player); }
	void put(double x, double z)
	{
		state::Fighter& p = player();
		p.x = x;
		p.y = 0.0;
		p.z = z;
	}
};

// The burn taken so far: the health the organic parts have lost (rwh.175: the beams take health, not the wear pool or stun).
double wearOf(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.robotic ? 0.0 : p.maxHealth - p.health;
	return sum;
}

// The wear and stun pools the beams must leave alone.
double poolsOf(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.wear + p.stun;
	return sum;
}

// Every part back to full health, so the long burn chain of one test never downs the character.
void heal(state::Fighter& f)
{
	for (state::FighterPart& p : f.parts)
		p.health = p.maxHealth;
}

std::size_t organicParts(const state::Fighter& f)
{
	return static_cast<std::size_t>(std::count_if(f.parts.begin(), f.parts.end(), [](const state::FighterPart& p) { return !p.robotic; }));
}

// rwh.175: two clocks per beam, and only the loaded-cell population reaches a client. Thresholds, all from the live readings:
//  - visible burst 6.0 to 10.6 s (120 to 212 ticks, mean ~8.3 s), damage window 10 to 30 s (200 to 600 ticks); a burst never outlives
//    its beam and most beams burn on after theirs ends;
//  - loaded population: median 7 of 40 handlers, range 4 to 13 per snapshot (so the mean over a sim-minute lies in 4..10);
//  - on screen near a character: 0 to 2 bursts at once on average (mean <= 2 within a view-sized 2500 u disc), and 2 to 3 new ones
//    per minute in view (accepted 1..6: the test disc is only about a view).
void testPopulation(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	ks::ServerContext context;
	context.gameData = gd;
	context.dataHash = ks::dataHash(kworld::vanillaLoadOrder(KENSHI_DATA));
	auto sim = makeSim(gd, 41, tickAt(3, 10, 0));
	sim->run(1);
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	sim->run(1);

	// The reference point: the Venge cell centre nearest the median of the group's cells (the 10000 u disc is clipped by the strip edges there).
	const AreaLookup& areas = *sim->weather()->areas();
	std::vector<std::pair<double, double>> centres;
	for (int index = 0; index < kworld::kZoneCount; ++index)
	{
		const kworld::ZoneCell cell = kworld::zoneCellOfIndex(index);
		const double cx = kworld::zoneMinX(cell.x) + 0.5 * kworld::kZoneSize;
		const double cz = kworld::zoneMinZ(cell.z) + 0.5 * kworld::kZoneSize;
		if (areas.colourAt(cx, cz) == kVengeColour)
			centres.emplace_back(cx, cz);
	}
	test::require(!centres.empty(), "no Venge cells");
	std::vector<double> xs;
	std::vector<double> zs;
	for (const auto& c : centres)
	{
		xs.push_back(c.first);
		zs.push_back(c.second);
	}
	std::sort(xs.begin(), xs.end());
	std::sort(zs.begin(), zs.end());
	const double medX = xs[xs.size() / 2];
	const double medZ = zs[zs.size() / 2];
	const auto ref = *std::min_element(centres.begin(), centres.end(), [&](const auto& a, const auto& b)
		{ return std::hypot(a.first - medX, a.second - medZ) < std::hypot(b.first - medX, b.second - medZ); });
	std::cout << "population: " << centres.size() << " Venge cells, reference (" << ref.first << ", " << ref.second << ")\n";

	Watch watch(*sim, context);
	watch.setView(static_cast<float>(ref.first), static_cast<float>(ref.second));
	watch.frames.clear();

	constexpr double kViewRadius = 2500.0;
	constexpr int kSeconds = 600;
	std::set<std::uint32_t> seen; // beam ids of the whole run: one slot-chain beam each (ids are unique inside the instance)
	std::set<std::uint32_t> viewed;
	std::size_t outlived = 0;
	std::uint64_t minVisible = UINT64_MAX;
	std::uint64_t maxVisible = 0;
	double sumVisible = 0.0;
	double loaded = 0.0;
	double onScreen = 0.0;
	std::size_t sent = 0;
	for (int second = 0; second < kSeconds; ++second)
	{
		watch.tick(20);
		const std::uint64_t now = sim->clockTick();
		for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
		{
			const Beam& beam = slot.current;
			if (seen.insert(beam.id).second)
			{
				const std::uint64_t life = beam.endTick - beam.spawnTick;
				test::require(life >= 200 && life <= 600, "damage window " + std::to_string(life) + " ticks, not 10..30 s");
				test::require(beam.visibleTicks >= 120 && beam.visibleTicks <= 212 && beam.visibleTicks <= life,
					"visible burst " + std::to_string(beam.visibleTicks) + " ticks, not 6.0..10.6 s inside the life");
				minVisible = std::min<std::uint64_t>(minVisible, beam.visibleTicks);
				maxVisible = std::max<std::uint64_t>(maxVisible, beam.visibleTicks);
				sumVisible += beam.visibleTicks;
				outlived += life > beam.visibleTicks + 20u;
			}
			const double d = std::hypot(slot.px - ref.first, slot.pz - ref.second);
			loaded += d <= kBeamRange;
			if (d <= kViewRadius && beamVisibleAt(beam, now))
			{
				onScreen += 1.0;
				viewed.insert(beam.id);
			}
		}
	}
	for (const WireFrame& frame : watch.frames)
		sent += beamsIn(frame, venge.groupSid);
	const double n = static_cast<double>(seen.size());
	const double minutes = kSeconds / 60.0;
	std::cout << "population: " << seen.size() << " beams, visible " << minVisible / 20.0 << ".." << maxVisible / 20.0 << " s (mean "
		<< sumVisible / n / 20.0 << "), " << outlived << " burn on after their burst; loaded mean " << loaded / kSeconds << " of 40 within "
		<< kBeamRange << " u, " << sent / minutes << " sent/min; in view (" << kViewRadius << " u) mean " << onScreen / kSeconds << " at once, "
		<< viewed.size() / minutes << " per min\n";
	test::require(minVisible < 140 && maxVisible > 190, "the visible burst does not spread over 6.0..10.6 s");
	test::require(sumVisible / n / 20.0 > 7.5 && sumVisible / n / 20.0 < 9.1, "the mean burst is not about 8.3 s");
	test::require(outlived > seen.size() / 2, "the damage window does not usually outlast the burst");
	test::require(loaded / kSeconds >= 4.0 && loaded / kSeconds <= 10.0, "the loaded population is not about 7 of 40");
	test::require(onScreen / kSeconds <= 2.0, "more than 2 bursts at once on average in view");
	test::require(viewed.size() / minutes >= 1.0 && viewed.size() / minutes <= 6.0, "new bursts in view per minute are not 2..3");
	// A session inside the zone gets every beam of the group (h2v.14.14: ~2 a second hand over; the client draws the closest few).
	test::require(sent / minutes > 100.0 && sent / minutes < 150.0, "a session in the zone does not get every beam of the group");
}

void testBurn(const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<const CombatData>& data,
	const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order, const Venge& venge)
{
	Rig rig(gd, data, nav, order);
	rig.sim->setClockTick(tickAt(3, 10, 0));
	rig.sim->enableWeather(*gd);
	rig.sim->tick();
	test::require(rig.sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	rig.sim->tick();
	const BeamField& field = *rig.sim->beams();
	test::require(field.groups().size() > venge.group, "no Venge group in the field");
	const BeamGroup& group = field.groups()[venge.group];
	test::require(group.slots.size() == 40, "no beams after the pin");

	// The onset is staggered over the first 30 s (h2v.14.14): wait it out so slot 0's beam is alive.
	for (int t = 0; t < 601; ++t)
		rig.sim->tick();
	auto burnAtSlot = [&](std::size_t slot, double dx)
	{
		heal(rig.player());
		const double before = wearOf(rig.player());
		for (int t = 0; t < 20; ++t)
		{
			const BeamSlot& s = field.groups()[venge.group].slots[slot];
			rig.put(s.px + dx, s.pz);
			rig.sim->tick();
		}
		return (wearOf(rig.player()) - before) / static_cast<double>(organicParts(rig.player()));
	};

	// At a beam's centre: 10 x 1 s x 0.75 = 7.5 per organic part (no other beam within 200 u: the 40 beams roam the group's cells).
	const double centre = burnAtSlot(0, 0.0);
	std::cout << "burn: centre " << centre << " per part per second\n";
	test::require(std::fabs(centre - 7.5) < 0.2, "the burn at a beam's centre is " + std::to_string(centre) + ", not 7.5 per part per second");
	// rwh.175: the burn is part HEALTH; the wear pool and the stun stay at 0 (today's code filled the wear pool).
	test::require(poolsOf(rig.player()) < 1.0e-9, "the beam filled the wear or stun pool: " + std::to_string(poolsOf(rig.player())));
	// In the ring (250 u from the start of each tick, the beam moving 6.5 u a tick: d2 about 55000..62500, radius^2 = 40000): between the
	// full burn and none, near 10 x 0.75 x (1 - 22500 / 40000) = 3.3 per part per second (the exact value depends on the heading).
	const double ring = burnAtSlot(0, 250.0);
	std::cout << "burn: ring " << ring << "\n";
	test::require(ring > 2.5 && ring < 4.5, "the burn in the ring is " + std::to_string(ring));
	// Outside radius x sqrt 2 = 283 u: none.
	const double outside = burnAtSlot(0, 320.0);
	test::require(outside < 1.0e-9, "a beam burns 320 u away: " + std::to_string(outside));

	// Worn protection of type 3 (P = 0.5 on one piece): half the burn. P >= 1: none.
	const kdata::Record* dustcoat = gd->find("3061-gamedata.base");
	test::require(dustcoat != nullptr, "no Dustcoat ARMOUR record");
	state::Fighter& p = rig.player();
	p.armour.clear();
	p.armour.push_back(data->makeArmour(*dustcoat, 1.0));
	p.armour.back().weatherTypes = 1u << 3;
	p.armour.back().weatherProtection = 0.5;
	const double half = burnAtSlot(0, 0.0);
	test::require(std::fabs(half - 3.75) < 0.1, "P3 = 0.5 gives " + std::to_string(half) + ", not 3.75");
	p.armour.back().weatherProtection = 1.0;
	test::require(burnAtSlot(0, 0.0) < 1.0e-9, "total protection still burns");
	// Protection against another type (2, acid) does not help.
	p.armour.back().weatherTypes = 1u << 2;
	test::require(std::fabs(burnAtSlot(0, 0.0) - 7.5) < 0.2, "acid protection reduced the beam burn");
	p.armour.clear();

	// A race immune to type 3: none, bare.
	std::string immune;
	for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("RACE")))
		for (const kdata::FieldInt& field : record->ints)
			if (field.key.starts_with("weather immunity") && field.value == 3 && immune.empty())
				immune = record->stringID;
	if (immune.empty())
	{
		std::cout << "no race lists weather immunity 3 in the merged data: immunity checked with the 2-bit race against type 2 only\n";
	} else {
		const std::string was = p.raceSid;
		p.raceSid = immune;
		test::require(burnAtSlot(0, 0.0) < 1.0e-9, "a race immune to type 3 burns");
		p.raceSid = was;
		test::require(burnAtSlot(0, 0.0) > 7.0, "the race did not go back to burning");
	}

	// Indoors: exempt. The flag is recomputed by the arena from the building under the fighter; hold it with the beam callback off the
	// field and the player at a spot with no building but `indoors` forced is overwritten, so the check uses the Hub Bar's ground floor
	// with a callback that burns everywhere (the gate under test is Fighter::indoors alone, as ksim_indoors does for acid).
	rig.arena().environmentMutable().beamAt = [](double, double, int& type, double& strength)
	{
		type = 3;
		strength = 1.0;
	};
	const BuildingSite* bar = nullptr;
	for (const BuildingSite& site : rig.sim->world().buildingSites())
		if (std::hypot(site.cx - -51181.0, site.cz - 2639.0) < 30.0)
			bar = &site;
	test::require(bar != nullptr && bar->hull, "no Hub Bar");
	auto wearAt = [&](double x, double y, double z)
	{
		state::Fighter& f = rig.player();
		heal(f);
		f.x = x;
		f.y = y;
		f.z = z;
		rig.sim->tick(); // the placement step
		rig.sim->tick();
		rig.sim->tick();
		const double before = wearOf(rig.player());
		for (int t = 0; t < 20; ++t)
		{
			f.x = x;
			f.y = y;
			f.z = z;
			rig.sim->tick();
		}
		return wearOf(rig.player()) - before;
	};
	test::require(wearAt(bar->hullCx, bar->hullMinY + 5.0, bar->hullCz) < 1.0e-9, "the beam burns indoors");
	test::require(wearAt(bar->hullCx, bar->floors.front(), bar->hullCz) > 1.0, "the beam does not burn on the roof");
	std::cout << "burn: centre, ring, outside, protection, immunity and indoors ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_venge_beams HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const WeatherModel model = WeatherModel::fromGameData(*gd);
		const Venge venge = findVenge(model);
		testSchedule(model, venge);
		testField(gd, venge);
		testDeterminism(gd, venge);
		testDayEnd(gd, venge);
		testFrame(gd, venge);
		testPopulation(gd, venge);
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		testBurn(gd, data, nav, kworld::vanillaLoadOrder(KENSHI_DATA), venge);
		std::cout << "ksim_venge_beams ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
