// Machine rows on the wire (kenshi-k16.5.26, approved on kenshi-k16.5.25; SIMLINK.md INTERACTABLE kind 12, BUILDING "Machines and containers"):
// a Session on a new game with the view at the South Stone Camp. Decoded by hand from the layouts in SIMLINK.md:
//   - every camp machine that is no producer entity arrives as INTERACTABLE kind 12 (kind, state, input, output; a turret's key last) with a
//     BUILDING row of the same id, whoever owns it (Slave Traders: a non-owned machine);
//   - the join: kind 12 id = BUILDING id, kind 4 (a producer entity) id = BUILDING id, kind 3 (a placed container) id = BUILDING id;
//   - a furnace is kind 12 (not kind 3), its contents an INVENTORY under its id; a fully automatic producer's BUILDING row is Working with bit 11;
//   - a turret's kind 12 key is the key the mounted gunner carries (Fighter::mount.building: the CHAR_DYNAMIC mounted tail and event 49 use it);
//   - the view moved away: event 11 for each id, and no BUILDING row for a machine outside the bounds.

#include "ksim/combat/combat.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/machines.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/world/world.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr double kCampX = 64742.0; // the South Stone Camp (ksim_machine_records)
constexpr double kCampZ = -49500.0;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	test::require(pos + sizeof(T) <= b.size(), "frame overrun");
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct Thing
{
	std::uint8_t kind = 0;
	std::uint32_t record = 0;
	std::uint8_t state = 0;
	std::uint32_t input = 0, output = 0;
	std::uint8_t machineKind = 0, machineState = 0;
	std::uint64_t turret = 0;
	bool hasTail = false;
};

struct Row
{
	std::uint64_t owner = 0;
	std::uint8_t status = 0;
	std::uint32_t flags = 0;
	float hp = 0.0f, maxHp = 0.0f, powerValue = 0.0f;
	std::uint8_t workers = 0, maxWorkers = 0;
	float maxEfficiency = 0.0f;
};

struct Seen
{
	std::map<std::uint64_t, Thing> things;
	std::map<std::uint64_t, Row> rows;
	std::set<std::uint64_t> left;
	std::set<std::uint64_t> inventories;
};

void parse(const std::vector<std::uint8_t>& bytes, Seen& seen)
{
	for (std::size_t pos = 0; pos < bytes.size();)
	{
		const std::uint32_t len = at<std::uint32_t>(bytes, pos);
		const std::uint16_t type = at<std::uint16_t>(bytes, pos + 4);
		const std::vector<std::uint8_t> body(bytes.begin() + static_cast<std::ptrdiff_t>(pos + 6), bytes.begin() + static_cast<std::ptrdiff_t>(pos + 4 + len));
		pos += 4 + len;
		if (type == ks::kInteractable)
		{
			Thing t;
			const std::uint64_t id = at<std::uint64_t>(body, 8);
			t.kind = body[16];
			t.record = at<std::uint32_t>(body, 17);
			t.state = body[45];
			if (t.kind == ks::kInteractableMachine)
			{
				// 60: the ownership tail (strid owner, u16 lockLevel, strid model, f32 yaw) = 14 bytes, then the machine tail.
				t.machineKind = body[74];
				t.machineState = body[75];
				t.input = at<std::uint32_t>(body, 76);
				t.output = at<std::uint32_t>(body, 80);
				if (t.machineKind == static_cast<std::uint8_t>(machines::Kind::Turret))
					t.turret = at<std::uint64_t>(body, 84);
				t.hasTail = true;
			}
			seen.things[id] = t;
		} else if (type == ks::kBuilding) {
			const std::uint16_t n = at<std::uint16_t>(body, 8);
			std::size_t at0 = 10;
			for (std::uint16_t i = 0; i < n; ++i)
			{
				const std::uint64_t id = at<std::uint64_t>(body, at0);
				Row r;
				r.owner = at<std::uint64_t>(body, at0 + 12);
				r.status = body[at0 + 49];
				r.flags = at<std::uint32_t>(body, at0 + 50);
				r.hp = at<float>(body, at0 + 62);
				r.maxHp = at<float>(body, at0 + 66);
				test::require(body[at0 + 70] == 0, "a machine row carries no materials");
				r.powerValue = at<float>(body, at0 + 71);
				r.workers = body[at0 + 91];
				r.maxWorkers = body[at0 + 92];
				r.maxEfficiency = at<float>(body, at0 + 97);
				seen.rows[id] = r;
				at0 += 117;
			}
			test::require(at0 == body.size(), "BUILDING: the rows end the frame");
		} else if (type == ks::kEvent) {
			if (at<std::uint16_t>(body, 8) == 11)
				seen.left.insert(at<std::uint64_t>(body, 10));
		} else if (type == ks::kInventory) {
			seen.inventories.insert(at<std::uint64_t>(body, 8));
		}
	}
}

struct Link
{
	Simulation& sim;
	ks::Session session;
	Seen seen;

	Link(Simulation& s, ks::ServerContext& context)
		: sim(s)
		, session(s, context)
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.begin(w);
		std::vector<std::uint8_t> hello;
		const auto put = [&](const auto v)
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
			hello.push_back(static_cast<std::uint8_t>(c));
		test::require(session.onFrame(ks::kHello, hello.data(), hello.size(), w), "hello refused");
		session.sendTick(w, true);
		parse(bytes, seen);
	}

	void send()
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.sendTick(w, false);
		parse(bytes, seen);
	}
};

void focus(Simulation& sim, double x, double z)
{
	const_cast<flecs::world&>(sim.world().ecs()).set<state::ViewFocus>(state::ViewFocus{x, z});
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		SpawnOptions options;
		options.seed = 12;
		options.wildernessByDensity = false;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
		const std::shared_ptr<const CombatData> combatData = CombatData::build(*gd, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		ks::ServerContext context;
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());

		// ---- pick the camp's machines: one of each shape ----
		const auto nearCamp = [](const MachineSite& s) { return std::hypot(s.x - kCampX, s.z - kCampZ) <= 1100.0; };
		const MachineSite* turret = nullptr;
		const MachineSite* converter = nullptr;
		const MachineSite* furnace = nullptr;
		const MachineSite* producer = nullptr;
		const MachineSite* automatic = nullptr;
		std::size_t machines12 = 0;
		for (const MachineSite& s : sim.world().machineSites())
		{
			if (!nearCamp(s))
				continue;
			const machines::Static& info = sim.world().machineInfo(s);
			if (s.producerId != 0)
			{
				if (producer == nullptr)
					producer = &s;
				if (info.maxWorkers == 0 && automatic == nullptr)
					automatic = &s;
				continue;
			}
			++machines12;
			if (info.kind == machines::Kind::Turret && turret == nullptr)
				turret = &s;
			else if (info.kind == machines::Kind::Converter && converter == nullptr)
				converter = &s;
			else if (info.kind == machines::Kind::Furnace && furnace == nullptr)
				furnace = &s;
		}
		test::require(turret != nullptr && converter != nullptr && furnace != nullptr && producer != nullptr, "the camp has a turret, a converter, a furnace and a producer");
		test::require(!converter->ownerFaction.empty(), "the camp's converter belongs to a faction (a non-owned machine for the player)");
		const MachineSite* farMachine = nullptr;
		for (const MachineSite& s : sim.world().machineSites())
		{
			if (s.producerId == 0 && std::hypot(s.x - kCampX, s.z - kCampZ) > 12000.0)
			{
				farMachine = &s;
				break;
			}
		}
		test::require(farMachine != nullptr, "a machine far from the camp");

		// ---- the entities the camp's bubble would hold: a producer, a furnace's box, a placed container, a manned turret ----
		const MachineSite* wired = automatic != nullptr ? automatic : producer;
		{
			const machines::Static& info = sim.world().machineInfo(*wired);
			state::Producer box;
			box.recordSid = wired->recordSid;
			box.ownerSid = wired->ownerFaction;
			box.output = info.output;
			box.cap = 10;
			box.maxOperators = static_cast<std::uint8_t>(info.maxWorkers);
			box.x = box.useX = wired->x;
			box.y = box.useY = wired->y;
			box.z = box.useZ = wired->z;
			state::Inventory stock;
			stock.version = 1;
			ecs.entity().set<state::Id>(state::Id{wired->producerId}).set<state::Producer>(std::move(box)).set<state::Inventory>(std::move(stock));
		}
		{
			state::Container box;
			box.key = box.building = 4242;
			box.recordSid = furnace->recordSid;
			box.ownerSid = furnace->ownerFaction;
			box.x = box.useX = furnace->x;
			box.y = box.useY = furnace->y;
			box.z = box.useZ = furnace->z;
			ecs.entity().set<state::Id>(state::Id{furnace->id}).set<state::Container>(std::move(box)).set<state::Inventory>(state::Inventory{});
		}
		const kdata::Record* crate = nullptr;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("BUILDING")))
		{
			if (r->name == "Metal Box")
				crate = r;
		}
		test::require(crate != nullptr, "a Metal Box BUILDING");
		const auto placeBox = [&](std::uint64_t key, double x, double z, std::uint16_t piece)
		{
			state::Container box;
			box.key = key;
			box.building = key;
			box.piece = piece;
			box.recordSid = crate->stringID;
			box.ownerSid = converter->ownerFaction;
			box.x = box.useX = x;
			box.z = box.useZ = z;
			return sim.spawnContainer(std::move(box), state::Inventory{}).get<state::Id>()->value;
		};
		const std::uint64_t boxId = placeBox(9001, kCampX + 30.0, kCampZ + 30.0, state::Container::kNoPiece);
		const std::uint64_t pieceId = placeBox(9002, kCampX + 40.0, kCampZ + 30.0, 3); // a layout piece: no BUILDING row
		const std::uint64_t farBoxId = placeBox(9003, farMachine->x, farMachine->z, state::Container::kNoPiece);
		std::uint64_t gunner = 0;
		{
			const kdata::Record* bandit = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
				if (r->name == "Dust Bandit")
					bandit = r;
			test::require(bandit != nullptr, "a Dust Bandit CHARACTER");
			std::uint64_t rng = 5;
			state::Fighter f = combatData->makeFighter(*gd, bandit->stringID, "", rng);
			f.x = turret->x;
			f.z = turret->z;
			gunner = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			const kdata::RefCategory* gun = gd->find(turret->recordSid)->findRefs("gun turret");
			test::require(gun != nullptr && !gun->entries.empty(), "the turret BUILDING names its gun");
			test::require(const_cast<CombatArena*>(sim.combat())->mountTurret(gunner, turret->key, turret->x, turret->z, 0.0,
				combatData->makeTurretGun(*gd, *gd->find(gun->entries.front().targetID))), "the gunner mounts the turret");
		}

		// ---- in the bounds ----
		focus(sim, kCampX, kCampZ);
		Link link(sim, context);
		const Seen& seen = link.seen;
		const auto joined = [&](std::uint64_t id, std::uint8_t kind, const std::string& what)
		{
			const auto thing = seen.things.find(id);
			test::require(thing != seen.things.end() && thing->second.kind == kind, what + ": an INTERACTABLE of kind " + std::to_string(kind));
			const auto row = seen.rows.find(id);
			test::require(row != seen.rows.end(), what + ": a BUILDING row with the same id");
			test::require(row->second.maxHp > 0.0f && row->second.hp == row->second.maxHp && (row->second.flags & 1u) != 0, what + ": built, condition hp / max");
			return row->second;
		};
		const Row converterRow = joined(converter->id, ks::kInteractableMachine, "converter");
		test::require(converterRow.owner != 0, "the converter's row names its owner faction although the player does not own it");
		test::require(seen.things.at(converter->id).hasTail && seen.things.at(converter->id).machineKind == static_cast<std::uint8_t>(machines::Kind::Converter),
			"the converter's kind 12 tail: machine kind 1 (converter)");
		test::require(seen.things.at(converter->id).input != 0 && seen.things.at(converter->id).output != 0, "a converter names its input and output");
		const Row furnaceRow = joined(furnace->id, ks::kInteractableMachine, "furnace");
		test::require(furnaceRow.maxWorkers == 0 && furnaceRow.status == 0 && (furnaceRow.flags & (1u << 11)) != 0, "the furnace: Workers '-' (fully automatic), Working");
		test::require(seen.inventories.count(furnace->id) != 0, "the furnace's contents are an INVENTORY under its id");
		test::require(seen.things.at(furnace->id).machineState == 1, "a furnace is running");
		const Row turretRow = joined(turret->id, ks::kInteractableMachine, "turret");
		test::require(seen.things.at(turret->id).turret == turret->key && turret->key == sim.combat()->find(gunner)->mount.building,
			"the turret's kind 12 key is the key the mounted gunner carries");
		test::require(seen.things.at(turret->id).machineState == 1 && turretRow.workers == 1, "a manned turret runs, one worker");
		test::require(turretRow.powerValue == static_cast<float>(sim.world().machineInfo(*turret).powerOutput), "the BUILDING row carries the BUILDING's power output");
		const Row producerRow = joined(wired->producerId, ks::kInteractableProducer, "producer");
		if (wired == automatic)
			test::require(producerRow.status == 0 && (producerRow.flags & (1u << 11)) != 0 && producerRow.maxWorkers == 0, "an automatic producer sends Working with bit 11");
		const Row boxRow = joined(boxId, ks::kInteractableContainer, "placed container");
		test::require(boxRow.owner != 0, "the container's row names its owner faction");
		test::require(seen.things.count(furnace->id) == 1 && seen.things.at(furnace->id).kind != ks::kInteractableContainer, "the furnace is not also a kind 3");
		test::require(seen.rows.count(pieceId) == 0, "a layout piece gets no BUILDING row");
		test::require(seen.rows.count(farBoxId) == 0 && seen.rows.count(farMachine->id) == 0 && seen.things.count(farMachine->id) == 0, "outside the bounds: no row, no record");
		std::size_t camp12 = 0;
		for (const auto& [id, thing] : seen.things)
		{
			if (thing.kind == ks::kInteractableMachine)
			{
				++camp12;
				test::require(seen.rows.count(id) != 0, "every kind 12 has its BUILDING row");
			}
		}
		test::require(camp12 >= machines12, "every camp machine without a producer entity is sent");

		// ---- the view leaves: event 11 for each id, the rows go with them ----
		focus(sim, farMachine->x, farMachine->z);
		link.send();
		for (const std::uint64_t id : {converter->id, furnace->id, turret->id, wired->producerId, boxId})
			test::require(link.seen.left.count(id) != 0, "leaving the bounds is event 11 for the id both records share");
		test::require(link.seen.rows.count(farMachine->id) != 0 && link.seen.rows.count(farBoxId) != 0, "the far machine and container arrive once the view is there");
		std::cout << "ksim_machine_wire ok (" << camp12 << " kind 12 rows at the camp)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
