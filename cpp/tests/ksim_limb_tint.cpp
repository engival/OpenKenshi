// A severed limb's tint (kenshi-h2v.13.9; rwh42 severed limbs static section 2, SIMLINK INTERACTABLE kind 2 limb tail, save v104):
//   - at the sever the ground item carries skin = 1 - the victim's skin tone, blood = the race's blood colour (both 0..1) and scale 1 (no bone sizes);
//   - two victims of different skin tone and race get different values; an item that is not a limb (item function != 0x11) carries none;
//   - a save and a load keep them;
//   - the wire: a limb's INTERACTABLE record has the 36-byte tint after the ownership tail, every other item's has none.
// A bare world (dt27_rig) with a peeler for the sever; a new game on the Hub navmesh with a session for the wire (argv[1]: the Hub cache,
// ctest fixture knav_cache).

#include "dt27_rig.hpp"

#include "ksim/nav/navigator.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include <cstdio>
#include <cstring>

#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
using namespace dt27;
namespace ks = ksim::simlink;

struct Tint
{
	state::LimbTint tint;
	std::string item;
};

std::vector<Tint> peel(const std::string& characterSid, double skin, bool saveAndLoad)
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	rig.sim.setPeeler(site->key, true);
	const std::uint64_t id = rig.spawn(characterSid, site->cx, site->cz);
	state::Fighter& f = rig.fighterOf(id);
	f.cageBuilding = site->key;
	f.cageSlot = 0;
	f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	f.look.skin = skin;
	undress(rig, id);
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == "Left Arm" || p.name == "Right Arm" || p.name == "Left Leg" || p.name == "Right Leg")
			p.health = -(p.maxHealth - p.wear);
	}
	rig.tick(3);
	const auto collect = [](const flecs::world& ecs)
	{
		std::vector<Tint> found;
		const_cast<flecs::world&>(ecs).each([&](const state::GroundItem& g)
		{
			if (g.tint.set)
				found.push_back({g.tint, g.itemSid});
		});
		return found;
	};
	std::vector<Tint> now = collect(rig.sim.world().ecs());
	if (!saveAndLoad)
		return now;
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_limb_tint.ksav";
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	std::remove(path.c_str());
	const std::vector<Tint> kept = collect(loaded.world().ecs());
	test::require(kept.size() == now.size(), "the save keeps the tinted limbs");
	for (std::size_t i = 0; i < now.size(); ++i)
	{
		const state::LimbTint& a = now[i].tint;
		const state::LimbTint& b = kept[i].tint;
		test::require(a.skinR == b.skinR && a.skinG == b.skinG && a.skinB == b.skinB && a.bloodR == b.bloodR && a.bloodG == b.bloodG
			&& a.bloodB == b.bloodB && a.scaleX == b.scaleX && a.scaleY == b.scaleY && a.scaleZ == b.scaleZ, "a load changed a limb's tint");
	}
	return now;
}

struct Wire
{
	std::vector<std::uint8_t> payload;
	std::size_t at = 0;
	template <typename T>
	T get()
	{
		test::require(at + sizeof(T) <= payload.size(), "frame overrun");
		T v;
		std::memcpy(&v, &payload[at], sizeof(T));
		at += sizeof(T);
		return v;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_limb_tint HUB.knav");
		// ---- the stamp ----
		Rig probe({}, 0);
		std::string other; // a CHARACTER of another race with a `severed limbs` list and another blood colour
		const std::string baseRace = probe.fighterOf(probe.spawn(probe.body, 0.0, 0.0)).raceSid;
		const RaceCombat* baseData = probe.data->race(baseRace);
		test::require(baseData != nullptr, "no race data for " + baseRace);
		std::uint64_t rng = 3;
		for (const kdata::Record* r : probe.gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			const state::Fighter f = probe.data->makeFighter(*probe.gd, r->stringID, "", rng);
			const kdata::Record* race = probe.gd->find(f.raceSid);
			const RaceCombat* data = probe.data->race(f.raceSid);
			if (data != nullptr && data->bloodColour != baseData->bloodColour && race != nullptr && race->findRefs("severed limbs") != nullptr)
			{
				other = r->stringID;
				break;
			}
		}
		test::require(!other.empty(), "no CHARACTER of another race and blood colour with severed limbs");
		const RaceCombat* otherData = probe.data->race(probe.data->makeFighter(*probe.gd, other, "", rng).raceSid);

		const std::vector<Tint> a = peel(probe.body, 0.2, true);
		const std::vector<Tint> b = peel(other, 0.7, false);
		test::require(a.size() == 4 && b.size() == 4, "four tinted limbs each: " + std::to_string(a.size()) + ", " + std::to_string(b.size()));
		const auto blood = [](std::int32_t c, int shift) { return static_cast<double>((c >> shift) & 0xff) / 255.0; };
		for (const Tint& t : a)
		{
			test::require(std::fabs(t.tint.skinR - 0.8) < 1e-9 && t.tint.skinR == t.tint.skinG && t.tint.skinG == t.tint.skinB, "skin = 1 - 0.2 on all channels");
			test::require(t.tint.bloodR == blood(baseData->bloodColour, 16) && t.tint.bloodG == blood(baseData->bloodColour, 8)
				&& t.tint.bloodB == blood(baseData->bloodColour, 0), "blood = the race's colour");
			test::require(t.tint.scaleX == 1.0 && t.tint.scaleY == 1.0 && t.tint.scaleZ == 1.0, "scale 1 (no bone sizes)");
		}
		for (const Tint& t : b)
		{
			test::require(std::fabs(t.tint.skinR - 0.3) < 1e-9, "the second victim's skin = 1 - 0.7");
			test::require(t.tint.bloodR == blood(otherData->bloodColour, 16) && t.tint.bloodG == blood(otherData->bloodColour, 8)
				&& t.tint.bloodB == blood(otherData->bloodColour, 0), "the second victim's blood = its race's colour");
		}
		test::require(a[0].tint.skinR != b[0].tint.skinR && (a[0].tint.bloodR != b[0].tint.bloodR || a[0].tint.bloodG != b[0].tint.bloodG
			|| a[0].tint.bloodB != b[0].tint.bloodB), "two victims of another skin and race carry different tints");
		std::printf("ok  stamp: skin %.2f / %.2f, blood (%.3f %.3f %.3f) / (%.3f %.3f %.3f), save/load keeps them\n", a[0].tint.skinR, b[0].tint.skinR,
			a[0].tint.bloodR, a[0].tint.bloodG, a[0].tint.bloodB, b[0].tint.bloodR, b[0].tint.bloodG, b[0].tint.bloodB);

		// ---- the wire ----
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		SpawnOptions options;
		options.seed = 7;
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		ks::ServerContext context;
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(MeshNavigator::load(argv[1]));
		ks::spawnSquad(sim, *gd, *data, context);
		const state::Fighter* lead = nullptr;
		for (const CombatArena::Member& m : sim.combat()->members())
			if (lead == nullptr)
				lead = &m.fighter;
		test::require(lead != nullptr, "no squad member");
		const kdata::Record* plain = nullptr;
		const std::string limbSid = a[0].item;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ITEM")))
		{
			if (r->intOr("item function", 0) != 0x11 && plain == nullptr)
				plain = r;
		}
		test::require(plain != nullptr, "no ordinary item");
		state::GroundItem limb;
		limb.itemSid = limbSid;
		limb.x = limb.useX = lead->x + 3.0;
		limb.y = limb.useY = lead->y;
		limb.z = limb.useZ = lead->z;
		limb.tint = a[0].tint;
		sim.spawnGroundItem(limb);
		state::GroundItem bread;
		bread.itemSid = plain->stringID;
		bread.x = bread.useX = lead->x - 3.0;
		bread.y = bread.useY = lead->y;
		bread.z = bread.useZ = lead->z;
		sim.spawnGroundItem(bread);

		ks::Session session(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.begin(w);
		std::vector<std::uint8_t> hello;
		const auto put = [&](auto v)
		{
			const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
			hello.insert(hello.end(), p, p + sizeof(v));
		};
		put(ks::kMagic);
		put(ks::kProtocol);
		put(std::uint8_t{0});
		put(context.dataHash);
		put(std::uint16_t{4});
		for (const char c : std::string("test"))
			put(static_cast<std::uint8_t>(c));
		test::require(session.onFrame(ks::kHello, hello.data(), hello.size(), w), "hello refused");
		session.sendTick(w, true);
		int limbs = 0;
		int plains = 0;
		for (std::size_t at = 0; at < bytes.size();)
		{
			std::uint32_t len = 0;
			std::uint16_t type = 0;
			std::memcpy(&len, &bytes[at], 4);
			std::memcpy(&type, &bytes[at + 4], 2);
			if (type == ks::kInteractable)
			{
				Wire r{{bytes.begin() + static_cast<std::ptrdiff_t>(at + 6), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + len)}};
				r.get<std::uint64_t>();
				r.get<std::uint64_t>();
				const std::uint8_t kind = r.get<std::uint8_t>();
				const std::string record = session.strings().text(r.get<std::uint32_t>());
				if (kind == ks::kInteractableItem)
				{
					r.at = 8 + 8 + 1 + 4 + 12 + 12 + 1 + 8 + 2 + 4; // after stateVersion: tick, id, kind, record, pos, use, state, health x2, count, version
					r.get<std::uint32_t>(); // owner
					r.get<std::uint16_t>(); // lockLevel
					r.get<std::uint32_t>(); // model
					r.get<float>(); // yaw
					if (record == limbSid)
					{
						test::require(r.payload.size() - r.at == 36, "a limb's tail is 36 bytes, got " + std::to_string(r.payload.size() - r.at));
						float v[9];
						for (float& x : v)
							x = r.get<float>();
						test::require(v[0] == static_cast<float>(a[0].tint.skinR) && v[3] == static_cast<float>(a[0].tint.bloodR)
							&& v[4] == static_cast<float>(a[0].tint.bloodG) && v[5] == static_cast<float>(a[0].tint.bloodB) && v[6] == 1.0f
							&& v[7] == 1.0f && v[8] == 1.0f, "the wire's tint differs from the stamp");
						++limbs;
					} else if (record == plain->stringID) {
						test::require(r.at == r.payload.size(), "an ordinary item has no tint tail");
						++plains;
					}
				}
			}
			at += 4 + len;
		}
		test::require(limbs == 1 && plains == 1, "the limb and the ordinary item were not both sent: " + std::to_string(limbs) + ", " + std::to_string(plains));
		std::printf("ok  wire: the limb record has the 36-byte tint after the ownership tail, an ordinary item none\n");
		std::printf("ksim_limb_tint: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_limb_tint: FAILED: " << e.what() << "\n";
		return 1;
	}
}
