// Venge beams' population against the original (rwh.175, kenshi-h2v.14.14): venge_beams for 5 game-minutes by day, measured around a
// player standing at the user's playtest spot. rwh.175: 40 handlers, ~7 in loaded cells, 0 to 2 bursting in view at once, 2 to 3 per
// minute entering the view, none placed within 500 u of the player. Rows: cpp/tests/validity/misc_b.tsv.
// argv[1]: unused.

#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/weather.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
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

std::unique_ptr<Simulation> makeSim(const std::shared_ptr<const kdata::GameData>& gd, std::uint64_t seed, std::uint64_t clock)
{
	auto sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
	sim->setGameData(gd);
	sim->setClockTick(clock);
	sim->enableWeather(*gd);
	return sim;
}

const BeamGroup& vengeGroup(const Simulation& sim, const Venge& venge)
{
	return sim.beams()->groups()[venge.group];
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
	std::size_t beamBytes = 0; // BEAMS frames (58), with the 4 byte length
	double sendMs = 0.0; // wall time in sendTick (the frame build for this one session)

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
				beamBytes += 4 + len;
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
			const auto t0 = std::chrono::steady_clock::now();
			session->sendTick(w, false);
			sendMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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

inline int quadrantOf(double x, double z, double mx, double mz)
{
	// By the cell's centre (a spawn point is classed with its cell; cell centres tie on the medians).
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	return (kworld::zoneMinX(cell.x) + 0.5 * kworld::kZoneSize >= mx ? 1 : 0) + (kworld::zoneMinZ(cell.z) + 0.5 * kworld::kZoneSize >= mz ? 2 : 0);
}

struct Totals
{
	double burstMean = 0.0, newPerMin = 0.0, loaded = 0.0;
	int maxBursting = 0;
	std::size_t seconds = 0, overTwo = 0;
	std::size_t near500 = 0, spawns = 0, runs = 0;
	std::size_t quadSpawns = 0;
	std::size_t quad[4] = {0, 0, 0, 0}; // spawns by quadrant of the zone's cell medians
	std::size_t cellsIn[4] = {0, 0, 0, 0};
	double medX = 0.0, medZ = 0.0;
	double nearest = 1.0e18;
};

// A session at (x, z) for 2 game-minutes: what reaches it (amendment 3: every live beam inside the zone or within view range of it, none beyond).
void reach(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge, const char* label, double x, double z, bool expectBeams)
{
	ks::ServerContext context;
	context.gameData = gd;
	context.dataHash = ks::dataHash(kworld::vanillaLoadOrder(KENSHI_DATA));
	auto sim = makeSim(gd, 41, tickAt(3, 10, 0));
	sim->run(1);
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	sim->run(1);
	Watch watch(*sim, context);
	watch.setView(static_cast<float>(x), static_cast<float>(z));
	watch.frames.clear();
	watch.beamBytes = 0;
	constexpr int kSeconds = 120;
	std::set<std::uint32_t> spawned;
	std::set<std::uint32_t> sent;
	for (int second = 0; second < kSeconds; ++second)
	{
		watch.tick(20);
		for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
			spawned.insert(slot.current.id);
	}
	for (const WireFrame& frame : watch.frames)
		for (const WireGroup& g : frame.groups)
			if (g.group == venge.groupSid)
				for (const WireBeam& b : g.beams)
					sent.insert(b.id);
	std::cout << "reach " << label << " (" << x << ", " << z << "): " << sent.size() << " beams sent of " << spawned.size() << " spawned, BEAMS "
		<< watch.beamBytes / static_cast<double>(kSeconds) << " B/s, frame build " << watch.sendMs / (kSeconds * 20.0) << " ms/tick\n";
	if (expectBeams)
		test::require(sent.size() * 10 >= spawned.size() * 9, std::string(label) + ": the session does not get every beam of the group");
	else
		test::require(sent.empty(), std::string(label) + ": a beam reached a session far outside the zone");
}

// The onset (user 2026-11-19): the handlers start at random times over the first time-to-live, never all 40 in one tick, then refill.
void onset(const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge)
{
	auto sim = makeSim(gd, 51, tickAt(3, 10, 0));
	sim->run(1);
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	std::size_t previous = 0;
	std::size_t biggestStep = 0;
	std::size_t atOneSecond = 0;
	std::size_t atEnd = 0;
	for (int tick = 0; tick < 700; ++tick)
	{
		sim->run(1);
		std::size_t alive = 0;
		for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
			alive += slot.current.effect != nullptr && slot.current.spawnTick <= sim->clockTick() && sim->clockTick() < slot.current.endTick;
		if (tick < 600)
			biggestStep = std::max(biggestStep, alive > previous ? alive - previous : 0);
		previous = alive;
		if (tick == 20)
			atOneSecond = alive;
		atEnd = alive;
	}
	std::cout << "onset: " << atOneSecond << " handlers alive after 1 s, at most " << biggestStep << " new in one tick, " << atEnd << " at 35 s\n";
	test::require(atOneSecond < 20, "the handlers start together at the onset");
	test::require(biggestStep <= 6, "many handlers start in one tick");
	test::require(atEnd == 40, "the handlers do not all run after the onset");

	// Night: 23:30, no pin: the weather window is shut, no beam alive for 10 game-minutes.
	auto night = makeSim(gd, 52, tickAt(3, 23, 30));
	night->run(1);
	for (int tick = 0; tick < 12000; ++tick)
	{
		night->run(1);
		for (const BeamSlot& slot : vengeGroup(*night, venge).slots)
			test::require(slot.current.effect == nullptr, "a beam is alive at night");
	}
}

void measure(Totals& total, const std::shared_ptr<const kdata::GameData>& gd, const Venge& venge, std::uint64_t seed, double px, double pz)
{
	ks::ServerContext context;
	context.gameData = gd;
	context.dataHash = ks::dataHash(kworld::vanillaLoadOrder(KENSHI_DATA));
	auto sim = makeSim(gd, seed, tickAt(3, 10, 0));
	sim->run(1);
	test::require(sim->pinWeather(venge.groupSid, venge.weatherSid), "pin failed");
	sim->run(1);
	if (total.runs == 0)
	{
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
		std::vector<double> xs, zs;
		for (const auto& c : centres)
		{
			xs.push_back(c.first);
			zs.push_back(c.second);
		}
		std::sort(xs.begin(), xs.end());
		std::sort(zs.begin(), zs.end());
		total.medX = xs[xs.size() / 2];
		total.medZ = zs[zs.size() / 2];
		for (const auto& c : centres)
			++total.cellsIn[quadrantOf(c.first, c.second, total.medX, total.medZ)];
	}
	Watch watch(*sim, context);
	watch.setView(static_cast<float>(px), static_cast<float>(pz));
	watch.frames.clear();
	constexpr double kView = 3000.0;
	constexpr int kSeconds = 300;
	std::set<std::uint32_t> seen;
	std::set<std::uint32_t> viewed;
	std::size_t near500 = 0;
	std::size_t spawns = 0;
	double bursting = 0.0;
	int maxBursting = 0;
	std::size_t overTwoSeconds = 0;
	double loaded = 0.0;
	double nearest = 1.0e18;
	for (int second = 0; second < kSeconds; ++second)
	{
		watch.tick(20);
		const std::uint64_t now = sim->clockTick();
		int burstNow = 0;
		for (const BeamSlot& slot : vengeGroup(*sim, venge).slots)
		{
			const Beam& beam = slot.current;
			const double d = std::hypot(slot.px - px, slot.pz - pz);
			if (seen.insert(beam.id).second)
			{
				++spawns;
				near500 += std::hypot(beam.x - px, beam.z - pz) < 500.0;
				if (px == 22521.0) // one count per seed (the second spot repeats the same beams)
				{
					++total.quad[quadrantOf(beam.x, beam.z, total.medX, total.medZ)];
					++total.quadSpawns;
				}
			}
			loaded += d <= kBeamRange;
			if (beamVisibleAt(beam, now))
			{
				nearest = std::min(nearest, d);
				if (d <= kView)
				{
					++burstNow;
					viewed.insert(beam.id);
				}
			}
		}
		bursting += burstNow;
		maxBursting = std::max(maxBursting, burstNow);
		overTwoSeconds += burstNow > 2;
	}
	total.burstMean += bursting / kSeconds;
	total.newPerMin += viewed.size() / 5.0;
	total.loaded += loaded / kSeconds;
	total.maxBursting = std::max(total.maxBursting, maxBursting);
	total.seconds += kSeconds;
	total.overTwo += overTwoSeconds;
	total.near500 += near500;
	total.spawns += spawns;
	total.nearest = std::min(total.nearest, nearest);
	++total.runs;
	std::size_t sent = 0;
	std::set<std::uint32_t> sentIds;
	std::size_t sentNear = 0;
	for (const WireFrame& frame : watch.frames)
		for (const WireGroup& g : frame.groups)
			if (g.group == venge.groupSid)
				for (const WireBeam& b : g.beams)
				{
					++sent;
					sentIds.insert(b.id);
					sentNear += std::hypot(b.x - px, b.z - pz) < kView;
				}
	std::cout << "population seed " << seed << " at (" << px << ", " << pz << "): spawns " << spawns << " (" << spawns / 5.0 << "/min), within 500 u at spawn "
		<< near500 << ", loaded mean " << loaded / kSeconds << ", bursting in " << kView << " u mean " << bursting / kSeconds << " max " << maxBursting
		<< ", new in view " << viewed.size() / 5.0 << "/min, nearest bursting " << nearest << " u, frames sent " << sent << " beams (" << sentIds.size()
		<< " distinct, " << sentNear << " spawning within view), BEAMS " << watch.beamBytes / static_cast<double>(kSeconds) << " B/s, frame build "
		<< watch.sendMs / (kSeconds * 20.0) << " ms/tick\n";
	test::require(sentIds.size() * 10 >= spawns * 9, "a session inside the zone does not get every beam of the group");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		(void)argc;
		(void)argv;
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const WeatherModel model = WeatherModel::fromGameData(*gd);
		const Venge venge = findVenge(model);
		onset(gd, venge);
		reach(gd, venge, "inside", 22521.0, 15651.0, true);
		reach(gd, venge, "border", 12300.0, 10185.0, true);
		reach(gd, venge, "far", -120000.0, -120000.0, false);
		Totals total;
		for (std::uint64_t seed : {41, 42, 43, 44, 45, 46, 47, 48})
		{
			measure(total, gd, venge, seed, 22521.0, 15651.0);
			measure(total, gd, venge, seed, 24759.0, 10185.0);
		}
		const double runs = static_cast<double>(total.runs);
		std::cout << "pooled: bursting in view mean " << total.burstMean / runs << " max " << total.maxBursting << ", new in view " << total.newPerMin / runs
			<< "/min, loaded " << total.loaded / runs << ", within 500 u at spawn " << total.near500 << " of " << total.spawns << ", nearest bursting "
			<< total.nearest << " u\n";
		test::require(total.overTwo * 50 <= total.seconds, "more than 2 bursting beams in view for over 2 % of the time");
		test::require(total.burstMean / runs <= 1.0, "bursting-in-view mean is not 0 to 2");
		test::require(total.newPerMin / runs >= 1.5 && total.newPerMin / runs <= 4.0, "new beams in view per minute are not 1.5 to 4");
		test::require(total.loaded / runs >= 4.0 && total.loaded / runs <= 13.0, "loaded population is not 4 to 13");
		// Placement follows the zone, never the player (user 2026-11-19): spawns per quadrant of the zone are in proportion to its cells.
		std::size_t cells = 0;
		for (std::size_t q : total.cellsIn)
			cells += q;
		for (int q = 0; q < 4; ++q)
		{
			const double expect = static_cast<double>(total.quadSpawns) * static_cast<double>(total.cellsIn[q]) / static_cast<double>(cells);
			std::cout << "quadrant " << q << ": " << total.quad[q] << " spawns, " << expect << " expected from " << total.cellsIn[q] << " cells\n";
		}
		for (int q = 0; q < 4; ++q)
		{
			const double expect = static_cast<double>(total.quadSpawns) * static_cast<double>(total.cellsIn[q]) / static_cast<double>(cells);
			test::require(std::abs(static_cast<double>(total.quad[q]) - expect) <= 0.15 * expect + 10.0, "spawns are not area-uniform over the zone");
		}
		std::cout << "ksim_venge_population ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
