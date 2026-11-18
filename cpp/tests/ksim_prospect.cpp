// Prospecting (SIMLINK.md order 79 and PROSPECT 35, ksim/world/prospect.hpp; kinspect findings/prospecting.md and prosp19_static.md): the exe's
// maths as pure functions (trap, N and r for two Science levels, the deposit falloff), what every biome group resolves to in the merged
// game data, the order's answers and its cancel paths, the 1.0 s duration, the deposits, the Stone noise against kinspect's worked values and live points, and frame 35 decoded by
// hand against values the test computes from the raw records and the heightmap, with save, load and continue.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/world/prospect.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;
namespace pr = ksim::prospect;

constexpr double kEps = 1e-9;

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

struct Frame
{
	std::uint16_t type = 0;
	std::vector<std::uint8_t> payload;
};

std::vector<Frame> frames(const std::vector<std::uint8_t>& bytes)
{
	std::vector<Frame> out;
	for (std::size_t at = 0; at < bytes.size();)
	{
		std::uint32_t len = 0;
		std::memcpy(&len, &bytes[at], 4);
		Frame frame;
		std::memcpy(&frame.type, &bytes[at + 4], 2);
		frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 6), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
		out.push_back(std::move(frame));
		at += 4 + len;
	}
	return out;
}

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
	std::vector<std::uint8_t> block(std::size_t n)
	{
		test::require(at + n <= p.size(), "frame overrun");
		std::vector<std::uint8_t> out(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + n));
		at += n;
		return out;
	}
};

// PROSPECT (type 35) decoded from the layout in SIMLINK.md, by hand.
struct Wire35
{
	std::uint64_t tick = 0;
	std::uint64_t id = 0;
	float x = 0, z = 0, altitude = 0, science = 0, r = 0;
	std::uint32_t zone = 0;
	float wind = 0;
	std::vector<std::pair<std::uint8_t, float>> env;
	std::vector<std::pair<std::uint8_t, float>> res;
	std::uint16_t n = 0;
	std::vector<std::uint8_t> grey, water, fertility, stone;
	struct Dep
	{
		std::uint8_t res;
		float x, z, richness;
	};
	std::vector<Dep> deps;
};

Wire35 decode35(const Frame& frame)
{
	Cursor c{frame.payload};
	Wire35 w;
	w.tick = c.get<std::uint64_t>();
	w.id = c.get<std::uint64_t>();
	w.x = c.get<float>();
	w.z = c.get<float>();
	w.altitude = c.get<float>();
	w.science = c.get<float>();
	w.r = c.get<float>();
	w.zone = c.get<std::uint32_t>();
	w.wind = c.get<float>();
	const std::uint8_t nEnv = c.get<std::uint8_t>();
	for (std::uint8_t i = 0; i < nEnv; ++i)
	{
		const std::uint8_t cls = c.get<std::uint8_t>();
		w.env.emplace_back(cls, c.get<float>());
	}
	const std::uint8_t nRes = c.get<std::uint8_t>();
	for (std::uint8_t i = 0; i < nRes; ++i)
	{
		const std::uint8_t id = c.get<std::uint8_t>();
		w.res.emplace_back(id, c.get<float>());
	}
	w.n = c.get<std::uint16_t>();
	const std::size_t pixels = static_cast<std::size_t>(w.n) * w.n;
	w.grey = c.block(pixels);
	w.water = c.block(pixels);
	w.fertility = c.block(pixels);
	w.stone = c.block(pixels);
	const std::uint16_t nDep = c.get<std::uint16_t>();
	for (std::uint16_t i = 0; i < nDep; ++i)
	{
		Wire35::Dep d;
		d.res = c.get<std::uint8_t>();
		d.x = c.get<float>();
		d.z = c.get<float>();
		d.richness = c.get<float>();
		w.deps.push_back(d);
	}
	test::require(c.at == frame.payload.size(), "frame 35 has trailing bytes");
	return w;
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	kworld::LoadOrder order;
	ks::ServerContext context;
	Simulation sim;
	ks::Squad squad;
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;
	std::vector<Wire35> prospects; // every frame 35 since the last clear
	std::vector<std::uint64_t> finished; // the clock tick of every finished task (the sim's observer)

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		return s;
	}

	Rig(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n)
		: gd(g)
		, data(d)
		, nav(n)
		, order(o)
		, sim([&]
		{
			SpawnOptions options;
			options.seed = 7;
			return Simulation::newGame(KENSHI_DATA, o, options);
		}())
	{
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		sim.enableCombat(data, settings());
		sim.enableNavigation(nav);
		squad = ks::spawnSquad(sim, *gd, *data, context);
		sim.addProspectObserver(this, [this](const std::shared_ptr<const pr::Result>&) { finished.push_back(sim.clockTick()); });
		connect();
	}
	~Rig() { sim.removeProspectObserver(this); }

	std::vector<std::uint8_t> collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type == ks::kProspect)
				prospects.push_back(decode35(frame));
		}
		return bytes;
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

	// Sends order 79; returns "" when it was accepted, else the refusal reason. The order is not ticked.
	std::string prospectOrder(std::uint64_t id)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(79).put<std::uint64_t>(id);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type != ks::kEvent)
				continue;
			Cursor c{frame.payload};
			c.get<std::uint64_t>();
			const std::uint16_t kindOf = c.get<std::uint16_t>();
			if (kindOf == 9 || kindOf == 10)
			{
				test::require(c.get<std::uint32_t>() == seq, "the answer is for another order");
				return kindOf == 10 ? session->strings().text(c.get<std::uint32_t>()) : std::string();
			}
		}
		throw test::Failure("an order got no answer");
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

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }
	bool hasTask(std::uint64_t id) const
	{
		bool found = false;
		sim.world().ecs().each([&](const state::Id& i, const state::ProspectTask&) { found = found || i.value == id; });
		return found;
	}
};

void requireReason(const std::string& got, const std::string& want, const std::string& what)
{
	test::require(got == want, what + ": answered '" + got + "', wanted '" + want + "'");
}

void requireNear(double got, double want, double eps, const std::string& what)
{
	test::require(std::fabs(got - want) <= eps, what + ": got " + std::to_string(got) + ", wanted " + std::to_string(want));
}

// The Stone noise against kinspect findings/stone26_static.md (item 47): the Python port there was checked bit-exact against the exe.
void stoneNoiseExact()
{
	// the lattice hash (the port's noise1), exact: the values are multiples of 2^-30
	struct Hash { int x, z; double want; };
	for (const Hash& h : {Hash{0, 0, -0.2817909838631749}, Hash{1, 0, -0.6364480191841722}, Hash{0, 1, -0.5843862378969789},
			Hash{-3, 7, -0.13875216338783503}, Hash{12345, -6789, 0.7025492927059531}, Hash{1000000, -1000000, -0.2322757365182042}})
		test::require(std::abs(pr::latticeHash(h.x, h.z) - h.want) < 1e-15, "lattice hash spot check (" + std::to_string(h.x) + "," + std::to_string(h.z) + ")");

	// worked values: zoom 1200 / cutoff 0.5 (record 44872) and zoom 1000 / cutoff 0.4 (fcs.def defaults); boost 3 / cap 1 and boost 1 / cap 1
	struct Worked { double x, z; double a3, a1, b3, b1; };
	const Worked worked[12] = {
		{0, 0, 0.0000000, 0.0000000, 0.0000000, 0.0000000},
		{1000, 1000, 0.0000000, 0.0000000, 0.2556951, 0.0852317},
		{-5000, 3000, 0.3598849, 0.1199616, 1.0000000, 0.4310274},
		{12345.6, -7890.1, 0.0000000, 0.0000000, 0.7192723, 0.2397574},
		{50000, 40000, 1.0000000, 0.3524718, 0.0000000, 0.0000000},
		{123456.5, 98765.25, 0.2418126, 0.0806042, 0.5111806, 0.1703935},
		{-200000, 150000, 0.3198802, 0.1066267, 0.1424193, 0.0474731},
		{7777, -31337, 0.0000000, 0.0000000, 0.7479746, 0.2493249},
		{300, -300, 0.0000000, 0.0000000, 0.0121324, 0.0040441},
		{-120000, -80000, 0.0000000, 0.0000000, 1.0000000, 0.3815653},
		{25000, 5000, 0.0000000, 0.0000000, 0.0000000, 0.0000000},
		{60000, -45000, 0.0000000, 0.0000000, 0.8076919, 0.2692306},
	};
	for (const Worked& w : worked)
	{
		const std::string at = " at (" + std::to_string(w.x) + "," + std::to_string(w.z) + ")";
		// the table prints 7 digits, so 6e-8 is the print rounding; the integer display is exact
		requireNear(pr::stoneNoise(w.x, w.z, 1200.0, 0.5), w.a3, 6e-8, "stone 1200/0.5 boost 3" + at);
		requireNear(pr::stoneNoise(w.x, w.z, 1200.0, 0.5, 1.0, 1.0), w.a1, 6e-8, "stone 1200/0.5 boost 1" + at);
		requireNear(pr::stoneNoise(w.x, w.z, 1000.0, 0.4), w.b3, 6e-8, "stone 1000/0.4 boost 3" + at);
		requireNear(pr::stoneNoise(w.x, w.z, 1000.0, 0.4, 1.0, 1.0), w.b1, 6e-8, "stone 1000/0.4 boost 1" + at);
	}
	const int display[12][2] = {{0, 0}, {0, 25}, {35, 100}, {0, 71}, {100, 0}, {24, 51}, {31, 14}, {0, 74}, {0, 1}, {0, 100}, {0, 0}, {0, 80}};
	for (int i = 0; i < 12; ++i)
	{
		test::require(static_cast<int>(static_cast<float>(pr::stoneNoise(worked[i].x, worked[i].z, 1200.0, 0.5)) * 100.0f) == display[i][0], "display 1200/0.5 row " + std::to_string(i));
		test::require(static_cast<int>(static_cast<float>(pr::stoneNoise(worked[i].x, worked[i].z, 1000.0, 0.4)) * 100.0f) == display[i][1], "display 1000/0.4 row " + std::to_string(i));
	}

	// live points, The Great Desert: zoom 1.0, cutoff 0.5, stone mult 1.1; the window's Stone text is int(value x 100), which passes 100
	pr::Env env;
	env.stoneNoiseZoom = 1.0f;
	env.stoneNoiseCutoff = 0.5f;
	env.stoneMult = 1.1f;
	const struct { double x, z; int live; } points[] = {
		{30491, -83529, 98}, {30402, -83543, 81}, {30449, -83553, 90}, {30299, -83453, 50}, {30299, -83353, 31},
		{30299, -83053, 60}, {30299, -84053, 72}, {29799, -83553, 0}, {30699, -83553, 110}};
	for (const auto& p : points)
		test::require(static_cast<int>(static_cast<float>(pr::stoneAt(env, p.x, p.z)) * 100.0f) == p.live, "live Stone point " + std::to_string(p.x) + "," + std::to_string(p.z));
	for (const double x : {30899.0, 31299.0, 32299.0})
		test::require(static_cast<int>(static_cast<float>(pr::stoneAt(env, x, -83553)) * 100.0f) == 110, "live Stone cap 1.0 x 1.1 at " + std::to_string(x));
}

// ---- the pure rules ----

void rules()
{
	// trap: 0 below min - fade, up to 1 at min, 1 on [min, max], down to 0 at max + fade
	requireNear(pr::trap(-6000, 0, 12000, 5000), 0.0, kEps, "trap far below");
	requireNear(pr::trap(-5000, 0, 12000, 5000), 0.0, kEps, "trap at min - fade");
	requireNear(pr::trap(-2500, 0, 12000, 5000), 0.5, kEps, "trap half way up the ramp");
	requireNear(pr::trap(0, 0, 12000, 5000), 1.0, kEps, "trap at min");
	requireNear(pr::trap(12000, 0, 12000, 5000), 1.0, kEps, "trap at max");
	requireNear(pr::trap(14500, 0, 12000, 5000), 0.5, kEps, "trap half way down");
	requireNear(pr::trap(17000, 0, 12000, 5000), 0.0, kEps, "trap at max + fade");
	requireNear(pr::trap(570, 0, 500, 200), 0.65, 1e-12, "trap on the Desert ramp");
	requireNear(pr::trap(501, 0, 500, 0), 0.0, kEps, "a fade of 0 is a sharp edge");
	requireNear(pr::trap(500, 0, 500, 0), 1.0, kEps, "a fade of 0 keeps the edge");

	// the live check: The Great Desert showed Water 23 and Fertility 75 (kinspect prospecting.md); its env record is 44423 "Desert":
	// water alt 0-500 fade 200, min 0.1, mult 0.2; farming alt 0-800 fade 300, min 0, mult 0.75. At altitude 570: 0.65 x 0.2 + 0.1 and 1 x 0.75.
	pr::Env desert;
	desert.waterMin = 0.1;
	desert.waterMult = 0.2;
	desert.waterAltMin = 0;
	desert.waterAltMax = 500;
	desert.waterAltFade = 200;
	desert.farmingMin = 0.0;
	desert.farmingMult = 0.75;
	desert.farmingAltMin = 0;
	desert.farmingAltMax = 800;
	desert.farmingAltFade = 300;
	requireNear(pr::waterAt(desert, 570), 0.23, 1e-12, "Desert water at 570");
	requireNear(pr::fertilityAt(desert, 570), 0.75, 1e-12, "Desert fertility at 570");
	test::require(static_cast<int>(pr::waterAt(desert, 570) * 100 + 1e-9) == 23 && static_cast<int>(pr::fertilityAt(desert, 570) * 100 + 1e-9) == 75,
		"the list prints Water 23 and Fertility 75");
	requireNear(pr::waterAt(desert, 5000), 0.1, kEps, "far above the ramp only `water min` is left");
	pr::Env rich = desert;
	rich.waterMin = 1.0;
	rich.waterMult = 1.0;
	test::require(pr::waterAt(rich, 100) > 1.0 + 1e-9 || std::fabs(pr::waterAt(rich, 100) - 2.0) < 1e-9, "Water is not clamped (it can pass 100)");

	// N and r for two Science levels (kinspect live: 12.64 -> Area Size 551, 6.66 -> 479)
	requireNear(pr::areaHalfWidth(12.64), 2000 + 60 * 12.64, 1e-9, "r at 12.64");
	test::require(static_cast<int>(0.2f * static_cast<float>(pr::areaHalfWidth(12.64))) == 551, "Area Size at Science 12.64 is 551");
	test::require(static_cast<int>(0.2f * static_cast<float>(pr::areaHalfWidth(6.66))) == 479, "Area Size at Science 6.66 is 479");
	test::require(pr::mapSize(12.64) == 37 && pr::mapSize(6.66) == 22 && pr::mapSize(12) == 35, "N = int(6 + 2.46 science): 37, 22, 35");
	test::require(pr::mapSize(0) == 6 && pr::mapSize(100) == 252 && pr::mapSize(250) == 252, "N is 6 at Science 0 and 252 at 100 (clamped)");
	requireNear(pr::areaHalfWidth(0), 2000, kEps, "r at 0");
	requireNear(pr::areaHalfWidth(100), 8000, kEps, "r at 100");

	// the deposit falloff: full inside max(25, radius), 0 at 1500, quadratic in d^2
	requireNear(pr::depositFactor(0, 0), 1.0, kEps, "a deposit at the feet");
	requireNear(pr::depositFactor(625, 0), 1.0, kEps, "full at 25 units");
	requireNear(pr::depositFactor(2250000, 0), 0.0, kEps, "nothing at 1500 units");
	requireNear(pr::depositFactor((625.0 + 2250000.0) / 2.0, 0), 0.5, 1e-12, "half at the middle of d^2");
	requireNear(pr::depositFactor(9999, 100), 1.0, kEps, "a node of radius 100 is full to 100");
	requireNear(pr::depositFactor(10000.0 + (2250000.0 - 10000.0) / 4.0, 100), 0.75, 1e-12, "a quarter of the way in d^2 for radius 100");
	requireNear(pr::depositFactor(1e9, 0), 0.0, kEps, "far past 1500 is 0");
	requireNear(pr::depositFactor(0, 5000), 1.0, kEps, "a huge radius is capped at the reach (rin2 = R2): everything is full");

	// bytes
	test::require(pr::greyOf(500) == 0 && pr::greyOf(0) == 0 && pr::greyOf(10000) == 255 && pr::greyOf(20000) == 255, "grey ends");
	test::require(pr::greyOf(5250) == static_cast<std::uint8_t>(std::lround(0.5 * 255.0)), "grey half way");
	test::require(pr::planeByte(0.01) == 0 && pr::planeByte(-3) == 0 && pr::planeByte(0.0101) == 3 && pr::planeByte(0.5) == 128 && pr::planeByte(1.0) == 255
		&& pr::planeByte(7.0) == 255, "plane bytes: 0 at or below 0.01, round(clamp(v) x 255)");
	std::cout << "rules ok\n";
}

// ---- the data ----

// The first world point (a 256 x 256 sweep of the areas map) whose zone is the named group and whose terrain is dry; false if none.
bool findPoint(const Simulation& sim, const kworld::Heightmap& terrain, const std::string& sid, double& x, double& z)
{
	for (int row = 0; row < 256; ++row)
	{
		for (int col = 0; col < 256; ++col)
		{
			const double wx = kworld::kWorldMin + (col + 0.5) * 1152.0;
			const double wz = kworld::kWorldMin + (row + 0.5) * 1152.0;
			const auto zone = sim.prospectZone(wx, wz);
			if (zone && zone->sid == sid && terrain.terrainHeight(wx, wz) >= 200.0)
			{
				x = wx;
				z = wz;
				return true;
			}
		}
	}
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_prospect HUB.knav");
		rules();
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");

		stoneNoiseExact();

		Rig r(order, gd, data, nav);
		const std::uint64_t p1 = r.squad.players[0];
		const std::uint64_t p2 = r.squad.players[1];
		const std::uint64_t enemy = r.squad.enemies[0];

		// ---- what every biome group resolves to (merged data), and the deposits ----
		{
			std::map<std::string, std::string> envOf; // group sid -> env sid ("" none)
			std::map<std::uint32_t, std::string> byColour;
			std::size_t groups = 0;
			for (const kdata::Record* g : gd->byType(kworld::itemTypeByName("BIOME_GROUP")))
			{
				++groups;
				std::string env;
				if (const kdata::RefCategory* refs = g->findRefs("resources"); refs != nullptr && !refs->entries.empty())
					env = refs->entries.front().targetID;
				envOf[g->stringID] = env;
				const auto colour = static_cast<std::uint32_t>(g->intOr("index", 0));
				test::require(byColour.emplace(colour, g->stringID).second, "two BIOME_GROUPs share the colour of " + g->name + ": the zone lookup is ambiguous");
			}
			std::size_t withEnv = 0, found = 0;
			std::cout << "zone (BIOME_GROUP) -> ENVIRONMENT_RESOURCES, " << groups << " groups:\n";
			for (const kdata::Record* g : gd->byType(kworld::itemTypeByName("BIOME_GROUP")))
			{
				double x = 0, z = 0;
				const bool on = findPoint(r.sim, terrain, g->stringID, x, z);
				found += on ? 1 : 0;
				const std::string& env = envOf[g->stringID];
				const kdata::Record* e = env.empty() ? nullptr : gd->find(env);
				withEnv += e != nullptr ? 1 : 0;
				std::cout << "  " << g->name << " [" << g->stringID << "] -> " << (e != nullptr ? e->name + " [" + e->stringID + "]" : std::string("(no resources: all 0, Environment Dead)"))
					<< (on ? "" : "  (no dry point on the areas map)") << "\n";
				if (on)
				{
					const auto zone = r.sim.prospectZone(x, z);
					test::require(zone && zone->name == g->name && zone->env.has_value() == (e != nullptr) && (e == nullptr || zone->env->sid == e->stringID),
						"the zone lookup of " + g->name + " is not its first `resources` record");
				}
			}
			test::require(found >= 60 && withEnv >= 60, "too few groups found on the areas map");
			// The Great Desert resolves through rebirth.mod: the base file alone would give it nothing (kinspect's live Water 23 / Fertility 75).
			double x = 0, z = 0;
			test::require(findPoint(r.sim, terrain, "55621-rebirth.mod", x, z), "no dry point in The Great Desert");
			const auto desert = r.sim.prospectZone(x, z);
			test::require(desert && desert->name == "The Great Desert" && desert->env && desert->env->sid == "44423-rebirth.mod", "The Great Desert is not env 44423");

			// the deposits: every placed Iron / Copper Resource (docs/formats/COLLISION_FORMAT.md: 1,241 and 94)
			std::size_t iron = 0, copper = 0;
			for (const DepositSite& d : r.sim.world().depositSites())
			{
				iron += d.resource == pr::kIron ? 1 : 0;
				copper += d.resource == pr::kCopper ? 1 : 0;
			}
			std::cout << "deposits: " << iron << " iron, " << copper << " copper placements\n";
			test::require(iron == 1241 && copper == 94, "the deposit placements are not the 1,241 Iron Resource and 94 Copper Resource");
		}

		// ---- the pure computation at far-away points (nobody walks there) ----
		{
			double x = 0, z = 0;
			test::require(findPoint(r.sim, terrain, "55621-rebirth.mod", x, z), "no dry point in The Great Desert");
			const kdata::Record* env = gd->find("44423-rebirth.mod");
			const auto result = r.sim.prospectAt(p1, x, z, 12.64);
			test::require(result != nullptr, "no result");
			const double alt = terrain.terrainHeight(x, z);
			test::require(result->zone == "The Great Desert" && result->id == p1 && result->science == 12.64f, "zone / id / science");
			requireNear(result->altitude, alt, 1e-3, "altitude is the terrain height");
			requireNear(result->r, 2000 + 60 * 12.64, 1e-2, "r");
			test::require(result->n == 37 && result->grey.size() == 37u * 37u && result->stone.size() == 37u * 37u, "N and the plane sizes at Science 12.64");
			// the Environment line: the record's fractions of the classes above 0 (Desert: Arid 1)
			test::require(result->env.size() == 1 && result->env[0].first == pr::kArid && result->env[0].second == 1.0f, "Environment of the Desert is Arid 100%");
			// the list order and the trap formula against the raw record keys
			test::require(result->resources.size() == 5 && result->resources[0].first == 5 && result->resources[1].first == 6 && result->resources[2].first == 2
				&& result->resources[3].first == 1 && result->resources[4].first == 3, "the list order is Water, Fertility, Stone, Iron, Copper");
			auto key = [&](const char* name) { return static_cast<double>(env->floatOr(name, -99.f)); };
			auto ikey = [&](const char* name) { return static_cast<double>(env->intOr(name, -99)); };
			const auto expectTrap = [&](double min, double max, double fade) -> double
			{
				if (alt < min - fade || alt > max + fade)
					return 0.0;
				if (alt < min)
					return (alt - (min - fade)) / fade;
				return alt <= max ? 1.0 : (max + fade - alt) / fade;
			};
			const double water = expectTrap(ikey("water altitude min"), ikey("water altitude max"), ikey("water altitude fade")) * key("water mult") + key("water min");
			const double fertility = expectTrap(ikey("farming altitude min"), ikey("farming altitude max"), ikey("farming altitude fade")) * key("farming mult") + key("farming min");
			requireNear(result->resources[0].second, water, 1e-6, "Water at the Desert point");
			requireNear(result->resources[1].second, fertility, 1e-6, "Fertility at the Desert point");
			const pr::Env stoneEnv = pr::Env::fromRecord(*env);
			test::require(result->resources[2].second == static_cast<float>(pr::stoneAt(stoneEnv, x, z)), "Stone at the Desert point is the noise");
			{
				const double rr = result->r, step = 2.0 * rr / result->n;
				for (std::size_t row = 0; row < result->n; ++row)
					for (std::size_t col = 0; col < result->n; ++col)
						test::require(result->stone[row * result->n + col] == pr::planeByte(pr::stoneAt(stoneEnv, x - rr + (col + 0.5) * step, z - rr + (row + 0.5) * step)),
							"the Stone plane pixel is the noise");
			}
			// a different Science: a different N and r, the same point values
			const auto small = r.sim.prospectAt(p1, x, z, 6.66);
			test::require(small->n == 22 && small->grey.size() == 22u * 22u, "N at Science 6.66");
			requireNear(small->r, 2399.6, 1e-2, "r at 6.66");
			test::require(small->resources[0].second == result->resources[0].second && small->resources[1].second == result->resources[1].second,
				"Science does not change the point samples");
			// the map pixels: the corners and the centre against the heightmap
			const double rr = 2000 + 60 * 12.64;
			for (const std::pair<int, int> px : {std::pair{0, 0}, {36, 0}, {0, 36}, {36, 36}, {18, 18}})
			{
				const double wx = x - rr + (px.first + 0.5) * 2 * rr / 37;
				const double wz = z - rr + (px.second + 0.5) * 2 * rr / 37;
				const double a = terrain.terrainHeight(wx, wz);
				const std::size_t at = static_cast<std::size_t>(px.second) * 37 + static_cast<std::size_t>(px.first);
				const std::uint8_t grey = a <= 500 ? 0 : static_cast<std::uint8_t>(std::lround(std::min((a - 500) / 9500.0, 1.0) * 255));
				test::require(result->grey[at] == grey, "grey pixel (" + std::to_string(px.first) + "," + std::to_string(px.second) + ")");
				const auto trapAt = [&](double mn, double mx, double fd)
				{
					if (a < mn - fd || a > mx + fd)
						return 0.0;
					if (a < mn)
						return (a - (mn - fd)) / fd;
					return a <= mx ? 1.0 : (mx + fd - a) / fd;
				};
				const double wv = trapAt(ikey("water altitude min"), ikey("water altitude max"), ikey("water altitude fade")) * key("water mult") + key("water min");
				const std::uint8_t wb = wv <= 0.01 ? 0 : static_cast<std::uint8_t>(std::lround(std::min(wv, 1.0) * 255));
				test::require(result->water[at] == wb, "water pixel (" + std::to_string(px.first) + "," + std::to_string(px.second) + ")");
			}
			// a group without a resources record reads 0 and "Dead" (nEnv 0)
			double dx = 0, dz = 0;
			test::require(findPoint(r.sim, terrain, "49388-rebirth.mod", dx, dz), "no dry point in the Rebirth group");
			const auto none = r.sim.prospectAt(p1, dx, dz, 12.0);
			test::require(none->zone == "Rebirth" && none->env.empty(), "a group with no `resources` has no Environment (Dead)");
			test::require(std::all_of(none->resources.begin(), none->resources.end(), [](const auto& e) { return e.second == 0.0f; }), "its resources read 0");
			test::require(none->deposits.empty(), "its map has no deposit dots");
			// outside every group (the world edge is the NONE group at colour 0, so take the zone of an unknown colour via the far corner)
			std::cout << "pure computation ok\n";
		}

		// ---- deposits: the list value and the map dots ----
		{
			// the first iron placement in a zone whose env has `iron mult` > 0
			const DepositSite* site = nullptr;
			pr::Env env;
			for (const DepositSite& d : r.sim.world().depositSites())
			{
				const auto zone = r.sim.prospectZone(d.x, d.z);
				if (d.resource == pr::kIron && zone && zone->env && zone->env->ironMult > 0.0)
				{
					site = &d;
					env = *zone->env;
					break;
				}
			}
			test::require(site != nullptr, "no iron placement in a zone with iron");
			// Stand on a clean spot: the test moves the point a little off the placement along x and finds the nearest iron the sim should use.
			const auto nearest = [&](double x, double z, std::uint8_t res)
			{
				double best = 1e30;
				for (const DepositSite& d : r.sim.world().depositSites())
				{
					if (d.resource == res)
						best = std::min(best, (d.x - x) * (d.x - x) + (d.z - z) * (d.z - z));
				}
				return best;
			};
			for (const double offset : {0.0, 20.0, 700.0, 1200.0, 1600.0})
			{
				const double x = site->x + offset;
				const double z = site->z;
				const auto zone = r.sim.prospectZone(x, z);
				const auto result = r.sim.prospectAt(p1, x, z, 10.0);
				const double d2 = nearest(x, z, pr::kIron);
				const double reach2 = 1500.0 * 1500.0;
				const double inner2 = 625.0;
				double expected = 0.0;
				if (zone && zone->env && d2 <= reach2)
					expected = zone->env->ironMult * (1.0 - std::clamp((d2 - inner2) / (reach2 - inner2), 0.0, 1.0));
				requireNear(result->resources[3].second, expected, 1e-5, "iron at offset " + std::to_string(offset));
			}
			// the dots: every placement within 1.2 r of the prospector, both kinds, with the raw richness (no falloff)
			const double x = site->x;
			const double z = site->z;
			const auto result = r.sim.prospectAt(p1, x, z, 10.0);
			const double reach = 1.2 * (2000 + 600);
			std::size_t expectedDots = 0;
			for (const DepositSite& d : r.sim.world().depositSites())
				expectedDots += (d.x - x) * (d.x - x) + (d.z - z) * (d.z - z) <= reach * reach ? 1 : 0;
			test::require(result->deposits.size() == expectedDots && expectedDots >= 1, "deposits within 1.2 r: " + std::to_string(result->deposits.size()) + " vs "
				+ std::to_string(expectedDots));
			for (const prospect::Deposit& d : result->deposits)
			{
				const auto zone = r.sim.prospectZone(x, z);
				const double mult = d.resource == pr::kIron ? zone->env->ironMult : zone->env->copperMult;
				requireNear(d.richness, mult, 1e-6, "a dot's richness is the raw `iron mult` / `copper mult`");
			}
			std::cout << "deposits ok (" << expectedDots << " dots around the first iron placement)\n";
		}

		// ---- the order: answers ----
		{
			r.fighter(p1).skills.science = 12.64;
			r.fighter(p2).skills.science = 6.66;
			requireReason(r.prospectOrder(987654321), "unknown_id", "an unknown id");
			requireReason(r.prospectOrder(enemy), "not_player_controlled", "an enemy");
			{
				state::Fighter& f = r.fighter(p1);
				f.building = 77;
				requireReason(r.prospectOrder(p1), "not_allowed", "indoors");
				f.building = 0;
				f.carriedBy = p2;
				requireReason(r.prospectOrder(p1), "not_allowed", "carried");
				f.carriedBy = 0;
				f.down = true;
				requireReason(r.prospectOrder(p1), "not_allowed", "down");
				f.down = false;
				f.unconscious = true;
				requireReason(r.prospectOrder(p1), "not_allowed", "knocked out");
				f.unconscious = false;
				f.target = enemy;
				requireReason(r.prospectOrder(p1), "not_allowed", "in combat");
				f.target = 0;
				const double wasX = f.x, wasZ = f.z;
				// not on solid ground: a point of the world whose terrain is under the water level
				double wx = 0, wz = 0;
				bool wet = false;
				for (int row = 0; row < 256 && !wet; ++row)
				{
					for (int col = 0; col < 256 && !wet; ++col)
					{
						wx = kworld::kWorldMin + (col + 0.5) * 1152.0;
						wz = kworld::kWorldMin + (row + 0.5) * 1152.0;
						wet = terrain.terrainHeight(wx, wz) < pr::kWaterLevel - 20.0;
					}
				}
				test::require(wet, "no water on the heightmap");
				f.x = wx;
				f.z = wz;
				requireReason(r.prospectOrder(p1), "not_allowed", "not on solid ground");
				f.x = wasX;
				f.z = wasZ;
				// a dead character
				r.fighter(p2).dead = true;
				requireReason(r.prospectOrder(p2), "dead", "a dead character");
				r.fighter(p2).dead = false;
				// an enemy that is down is not a fight
				r.fighter(enemy).unconscious = true;
				f.target = enemy;
				requireReason(r.prospectOrder(p1), "", "a downed foe is not a fight");
				f.target = 0;
				r.fighter(enemy).unconscious = false;
			}
			std::cout << "order answers ok\n";
		}

		// ---- the duration: 1.0 sim-second, no skill term; the frame ----
		{
			// two prospectors with different Science finish in the same tick
			r.fighter(p1).skills.science = 12.64;
			r.fighter(p2).skills.science = 100.0; // N 252, r 8000: the largest frame (the Hub itself has no placement within 9600: the far prospect below draws the dots)
			r.finished.clear();
			r.prospects.clear();
			requireReason(r.prospectOrder(p1), "", "prospect p1");
			requireReason(r.prospectOrder(p2), "", "prospect p2");
			test::require(!r.hasTask(p1), "the task starts at the next tick, not at the session");
			r.tick(1);
			const std::uint64_t applied = r.sim.clockTick();
			test::require(r.hasTask(p1) && r.hasTask(p2), "the order did not start a task");
			r.tick(18);
			test::require(r.finished.empty() && r.hasTask(p1), "the task ended before 1.0 s (tick " + std::to_string(r.sim.clockTick() - applied + 1) + ")");
			r.tick(1);
			test::require(r.finished.size() == 2 && !r.hasTask(p1) && !r.hasTask(p2), "the tasks did not end after 20 ticks (1.0 s)");
			test::require(r.finished[0] == applied + 19 && r.finished[1] == applied + 19, "the tasks ended at a different tick than 20 after applying");
			test::require(static_cast<double>(pr::kTicks) * Simulation::kTickSeconds == 1.0, "kTicks x tick is 1.0 sim-second");
			// two prospectors finishing together: both frames are sent (one per finisher); a client keeps the later one. The session keeps the latest only.
			test::require(r.prospects.size() == 1, "the session sends the latest finisher only (a singleton on the client): " + std::to_string(r.prospects.size()));
			const Wire35& w = r.prospects[0];
			test::require(w.id == p2 || w.id == p1, "the frame names a prospector");
			const state::Fighter& f = *r.sim.combat()->find(w.id);
			const double science = w.id == p1 ? 12.64 : 100.0;
			const kdata::Record* envRecord = nullptr;
			const auto zone = r.sim.prospectZone(f.x, f.z);
			std::cout << "Hub prospect: zone '" << (zone ? zone->name : std::string("?")) << "' env " << (zone && zone->env ? zone->env->sid : std::string("-")) << " at ("
				<< f.x << ", " << f.z << ") altitude " << w.altitude << " wind " << w.wind << " science " << science << " N " << w.n << "\n";
			test::require(zone.has_value(), "no zone at the Hub");
			if (zone->env)
				envRecord = gd->find(zone->env->sid);
			// header
			requireNear(w.x, f.x, 1e-2, "x");
			requireNear(w.z, f.z, 1e-2, "z");
			requireNear(w.altitude, terrain.terrainHeight(f.x, f.z), 1e-2, "altitude");
			requireNear(w.science, science, 1e-5, "science");
			requireNear(w.r, 2000 + 60 * science, 1e-2, "r");
			test::require(w.zone != 0 && r.session->strings().text(w.zone) == zone->name && r.session->strings().entry(w.zone).kind == 2, "zone strid is the group's name (kind 2)");
			// wind: lerp(`wind speed min`, `wind speed max`, strength) of the region's current weather, read from the raw records
			double wind = 0.0;
			for (const state::WeatherRegion& region : r.sim.world().ecs().get<state::WeatherRegions>()->regions)
			{
				if (region.group == zone->sid && region.started)
				{
					const kdata::Record* weather = gd->find(region.weather);
					test::require(weather != nullptr, "the region's weather record");
					const double lo = weather->floatOr("wind speed min", 0.f), hi = weather->floatOr("wind speed max", 0.f);
					wind = lo + (hi - lo) * region.strength;
				}
			}
			requireNear(w.wind, wind, 1e-4, "wind");
			// environment
			std::vector<std::pair<std::uint8_t, float>> env;
			if (envRecord != nullptr)
			{
				const char* keys[3] = {"green", "arid", "swamp"};
				for (std::uint8_t k = 0; k < 3; ++k)
				{
					const float v = envRecord->floatOr(keys[k], 0.f);
					if (v > 0.f)
						env.emplace_back(static_cast<std::uint8_t>(k + 1), v);
				}
			}
			test::require(w.env == env, "the Environment classes");
			// resources
			test::require(w.res.size() == 5 && w.res[0].first == 5 && w.res[1].first == 6 && w.res[2].first == 2 && w.res[3].first == 1 && w.res[4].first == 3,
				"the resource ids and their order");
			if (envRecord != nullptr)
			{
				const double a = w.altitude;
				const auto t = [&](const char* mn, const char* mx, const char* fd)
				{
					const double lo = envRecord->intOr(mn, 0), hi = envRecord->intOr(mx, 0), fade = envRecord->intOr(fd, 0);
					if (a >= lo && a <= hi)
						return 1.0;
					if (fade <= 0 || a < lo - fade || a > hi + fade)
						return 0.0;
					return a < lo ? (a - (lo - fade)) / fade : (hi + fade - a) / fade;
				};
				requireNear(w.res[0].second, t("water altitude min", "water altitude max", "water altitude fade") * envRecord->floatOr("water mult", 0.f) + envRecord->floatOr("water min", 0.f),
					1e-5, "wire Water");
				requireNear(w.res[1].second, t("farming altitude min", "farming altitude max", "farming altitude fade") * envRecord->floatOr("farming mult", 0.f)
					+ envRecord->floatOr("farming min", 0.f), 1e-5, "wire Fertility");
			}
			// the map
			test::require(w.n == pr::mapSize(science) && w.grey.size() == static_cast<std::size_t>(w.n) * w.n && w.stone.size() == w.grey.size(), "N and the plane sizes");
			test::require(w.n == (w.id == p1 ? 37 : 252), "N for Science 12.64 / 100");
			const double rr = w.r;
			const std::size_t centre = static_cast<std::size_t>(w.n / 2) * w.n + w.n / 2;
			const double cx = f.x - rr + (w.n / 2 + 0.5) * 2 * rr / w.n;
			const double cz = f.z - rr + (w.n / 2 + 0.5) * 2 * rr / w.n;
			const double ca = terrain.terrainHeight(cx, cz);
			test::require(w.grey[centre] == (ca <= 500 ? 0 : static_cast<std::uint8_t>(std::lround(std::min((ca - 500) / 9500.0, 1.0) * 255))), "the centre pixel's grey");
			if (envRecord != nullptr)
			{
				const pr::Env stoneEnv = pr::Env::fromRecord(*envRecord);
				const double step = 2.0 * rr / w.n;
				test::require(w.res[2].second == static_cast<float>(pr::stoneAt(stoneEnv, f.x, f.z)), "wire Stone is the noise at the prospector");
				for (std::size_t row = 0; row < w.n; ++row)
					for (std::size_t col = 0; col < w.n; ++col)
						test::require(w.stone[row * w.n + col] == pr::planeByte(pr::stoneAt(stoneEnv, f.x - rr + (col + 0.5) * step, f.z - rr + (row + 0.5) * step)),
							"the wire Stone plane pixel is the noise");
			}
			// deposits: same set as the sim's, within 1.2 r, raw richness
			std::size_t dots = 0;
			for (const DepositSite& d : r.sim.world().depositSites())
				dots += envRecord != nullptr && (d.x - f.x) * (d.x - f.x) + (d.z - f.z) * (d.z - f.z) <= 1.44 * rr * rr ? 1 : 0;
			test::require(w.deps.size() == dots, "the deposit dot count on the wire");
			// the frame was built by hand above; the second finisher's frame was replaced (the session's latest wins)
			std::cout << "duration and frame 35 ok (frame " << w.n << "x" << w.n << ", " << w.deps.size() << " deposits, " << w.env.size() << " environment classes)\n";
		}

		// ---- a prospector far from the Hub: deposits on the wire (the navmesh covers only the Hub, so the character is put there after
		// the order starts; a task reads the position where it ends) ----
		{
			Rig g(order, gd, data, nav);
			const std::uint64_t id = g.squad.players[0];
			// the Hub has no placement within reach: a prospector there would draw no dots
			const DepositSite* site = nullptr;
			pr::Env env;
			for (const DepositSite& d : g.sim.world().depositSites())
			{
				const auto zone = g.sim.prospectZone(d.x, d.z);
				if (d.resource == pr::kCopper && zone && zone->env && zone->env->copperMult > 0.0 && terrain.terrainHeight(d.x, d.z) >= 200.0)
				{
					site = &d;
					env = *zone->env;
					break;
				}
			}
			test::require(site != nullptr, "no copper placement on dry ground");
			g.fighter(id).skills.science = 30.0;
			requireReason(g.prospectOrder(id), "", "prospect");
			g.tick(2);
			state::Fighter& f = g.fighter(id);
			f.x = site->x + 100.0;
			f.z = site->z;
			g.finished.clear();
			g.prospects.clear();
			g.tick(18);
			test::require(g.finished.size() == 1 && g.prospects.size() == 1, "the far prospect did not finish");
			const Wire35& w = g.prospects[0];
			requireNear(w.x, f.x, 1.0, "far x");
			test::require(w.deps.size() >= 1, "no deposit dots at a copper placement");
			bool seen = false;
			for (const Wire35::Dep& d : w.deps)
			{
				const auto zone = g.sim.prospectZone(site->x + 100.0, site->z);
				const double mult = d.res == pr::kIron ? zone->env->ironMult : zone->env->copperMult;
				requireNear(d.richness, mult, 1e-6, "dot richness");
				seen = seen || (d.res == pr::kCopper && std::fabs(d.x - site->x) < 1e-2 && std::fabs(d.z - site->z) < 1e-2);
			}
			test::require(seen, "the copper placement is not among the dots");
			// the Copper line: the nearest copper placement within 1500, 100 units away: full inside 25, then the quadratic falloff
			const auto zone = g.sim.prospectZone(f.x, f.z);
			double best = 1e30;
			for (const DepositSite& d : g.sim.world().depositSites())
			{
				if (d.resource == pr::kCopper)
					best = std::min(best, (d.x - f.x) * (d.x - f.x) + (d.z - f.z) * (d.z - f.z));
			}
			const double expected = zone->env->copperMult * (1.0 - (best - 625.0) / (2250000.0 - 625.0));
			requireNear(w.res[4].second, expected, 1e-5, "far Copper");
			requireNear(w.r, 2000 + 60 * 30.0, 1e-2, "far r");
			std::cout << "far prospect ok (" << w.deps.size() << " dots, copper " << w.res[4].second << ")\n";
		}

		// ---- cancel paths: no result ----
		{
			const auto restart = [&](std::uint64_t who)
			{
				r.finished.clear();
				r.prospects.clear();
				requireReason(r.prospectOrder(who), "", "prospect");
				r.tick(6);
				test::require(r.hasTask(who) && r.finished.empty(), "the task is running");
			};
			const auto noResult = [&](const char* what, std::uint64_t who = 0)
			{
				r.tick(40);
				test::require(r.finished.empty() && r.prospects.empty() && !r.hasTask(who != 0 ? who : p1), std::string("a result came out after ") + what);
			};
			// a replacing order (a stop; any order but sneak / block / drop / run)
			restart(p1);
			r.sim.enqueue(Command::fighterStop(state::Id{p1}, false));
			noResult("a replacing order");
			// a follow order
			restart(p1);
			r.sim.enqueue(Command::fighterFollow(state::Id{p1}, state::Id{p2}));
			noResult("a follow order");
			// a gait toggle is not a replacing order
			restart(p1);
			r.sim.enqueue(Command::fighterRun(state::Id{p1}, false));
			r.tick(1);
			test::require(r.hasTask(p1), "a gait toggle replaced the prospect");
			r.tick(30);
			test::require(r.finished.size() == 1 && r.prospects.size() == 1, "the prospect did not finish after a gait toggle");
			// a KO
			r.fighter(p1).followTarget = 0;
			restart(p1);
			r.fighter(p1).koTimer = 100.0; // the medical step derives `unconscious` from the timer every tick
			noResult("a KO");
			r.fighter(p1).koTimer = 0.0;
			r.tick(3);
			test::require(!r.fighter(p1).unconscious, "p1 is still down after the KO test");
			// carried (the carry step frees a pair that does not agree, so both ends are set)
			restart(p1);
			r.fighter(p1).carriedBy = p2;
			r.fighter(p2).carrying = p1;
			noResult("being picked up");
			r.fighter(p1).carriedBy = 0;
			r.fighter(p2).carrying = 0;
			// the order is checked again when it applies: indoors by then = dropped, no task
			r.finished.clear();
			requireReason(r.prospectOrder(p1), "", "accepted at the session");
			r.fighter(p1).building = 5;
			r.tick(3);
			test::require(!r.hasTask(p1), "an order that became not_allowed before it applied started a task");
			r.fighter(p1).building = 0;
			// death (latched, so it goes last and uses p2)
			restart(p2);
			r.fighter(p2).dead = true;
			noResult("death", p2);
			std::cout << "cancel paths ok\n";
		}

		// ---- save, load, continue: the task and Science are saved ----
		{
			const auto script = [&](Rig& g)
			{
				g.fighter(g.squad.players[0]).skills.science = 20.0;
				requireReason(g.prospectOrder(g.squad.players[0]), "", "prospect before save");
				g.tick(8);
			};
			Rig straight(order, gd, data, nav);
			script(straight);
			Rig first(order, gd, data, nav);
			script(first);
			const std::uint64_t id = first.squad.players[0];
			test::require(first.hasTask(id), "the task is running before the save");
			std::ostringstream saved;
			first.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, first.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == first.sim.stateHash(), "the hash changed across save and load");
			test::require(loaded.combat()->find(id)->skills.science == 20.0, "Science was not saved");
			std::size_t done = 0;
			std::uint64_t doneAt = 0;
			loaded.addProspectObserver(&done, [&](const std::shared_ptr<const pr::Result>& result)
			{
				++done;
				doneAt = loaded.clockTick();
				test::require(result->id == id && result->n == static_cast<std::uint16_t>(6 + 2.46 * 20.0), "the loaded result");
			});
			straight.finished.clear();
			loaded.run(30);
			straight.sim.run(30);
			test::require(done == 1 && doneAt != 0, "the loaded simulation did not finish the saved task");
			test::require(loaded.stateHash() == straight.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			loaded.removeProspectObserver(&done);
			std::cout << "save ok, hash " << std::hex << straight.sim.stateHash() << std::dec << "\n";
		}
		std::cout << "ksim_prospect ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_prospect FAILED: " << e.what() << "\n";
		return 1;
	}
}
