// Interactables and the `interact` order (docs/formats/SIMLINK.md section 13), end to end through a SIMLINK Session on the Hub
// navmesh cache (no sockets: the frames are built and parsed here): the INTERACTABLE records of the duel scenario's doors
// and ground items, opening and closing a door, picking up an item (event 7, the item's id leaves with event 11, the stub
// inventory), loot and first aid stubs on a downed character, every rejection (bad_target, not_allowed, unsupported,
// bad_value, no_route), an interrupted order, and save + load mid-walk giving the uninterrupted state hash.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/law/crime.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "knav/walk.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <optional>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct Inter
{
	std::uint64_t id = 0;
	std::uint8_t kind = 0;
	std::string record;
	float pos[3] = {};
	float use[3] = {};
	std::uint8_t state = 0;
	float health = 0;
	float maxHealth = 0;
	std::uint16_t count = 0;
	std::uint32_t version = 0;
	std::string owner; // the ownership tail
	std::uint16_t lockLevel = 0;
	std::string model;
	float yaw = 0;
	std::uint64_t building = 0; // kind 3 tail
	std::uint16_t piece = 0xFFFF;
};

struct Reply
{
	bool applied = false;
	bool rejected = false;
	std::string reason;
};

// A client of the session: builds order frames, parses what comes back.
struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	std::unique_ptr<ks::Session> session;
	ks::Duel duel;
	std::uint64_t second = 0; // the squad scenario's second player character
	std::uint32_t seq = 0;

	struct InvEntry
	{
		std::string item;
		std::uint32_t count;
		std::uint8_t slot;
		std::string maker; // ITEM_QUALITY (27), set when one followed the INVENTORY ("" = 0)
		std::string model;
		float level;
		std::uint8_t grade;
	};
	struct Inv
	{
		std::uint32_t version = 0;
		std::vector<InvEntry> items;
		std::size_t records = 0; // INVENTORY records received for this character
	};
	std::map<std::uint64_t, Inv> inventories; // INVENTORY (25), by character
	std::map<std::uint64_t, std::size_t> gridRecords; // ITEM_GRID (28) count, by owner
	std::map<std::uint64_t, std::size_t> medicalRecords; // CHAR_MEDICAL (22) count, by character
	std::map<std::uint64_t, float> medicalBlood;
	std::size_t bleedDrips = 0; // event 2
	std::map<std::uint64_t, std::string> names; // CHAR_APPEARANCE name
	std::map<std::uint64_t, std::uint32_t> flags; // CHAR_DYNAMIC flags
	std::map<std::uint64_t, std::uint64_t> carriedBy; // CHAR_DYNAMIC carriedBy
	struct Dropped
	{
		std::uint64_t who;
		std::string item;
	};
	std::vector<Dropped> dropped; // event 6
	std::map<std::uint64_t, Inter> inters; // what the client knows
	std::vector<std::uint64_t> left; // event 11 ids, in order
	struct Picked
	{
		std::uint64_t who;
		std::string item;
	};
	std::vector<Picked> picked; // event 7
	struct Theft
	{
		std::uint64_t who, container;
		std::string owner;
	};
	std::vector<Theft> thefts; // event 14
	struct Done
	{
		std::uint64_t who, target;
		std::uint8_t action, result;
	};
	std::vector<Done> done; // event 13
	std::map<std::uint32_t, Reply> replies;
	// The navmesh tile stream as received (SIMLINK.md section 12).
	struct NavParams
	{
		std::uint32_t meshId = 0, detourVersion = 0, maxTiles = 0, maxPolys = 0;
		float origin[3] = {}, tileWidth = 0, tileHeight = 0;
	};
	std::vector<NavParams> navParams;
	std::map<std::pair<int, int>, std::vector<std::uint8_t>> navTiles;
	std::vector<std::pair<int, int>> navRemoved; // size-0 records
	std::vector<std::pair<int, int>> navOrder; // arrival order of new tiles
	std::map<std::pair<int, int>, std::uint32_t> navHeld; // the version of each tile the client holds
	struct PatchSeen
	{
		std::pair<int, int> tile;
		std::uint32_t version = 0;
		bool applied = false;
		std::size_t polys = 0;
	};
	std::vector<PatchSeen> navPatches; // every NAV_POLY_FLAGS, in arrival order
	std::size_t navBytesLastParse = 0, navMaxParse = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order, bool squad = false)
		: gd(g)
		, data(d)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g); // as ksim_server does: door lock levels, faction kinds and relations, equip all read the records
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		if (squad)
		{
			const ks::Squad spawned = ks::spawnSquad(*sim, *gd, *data, context);
			duel.player = spawned.players.at(0);
			second = spawned.players.at(1);
			duel.enemy = spawned.enemies.at(0);
		} else {
			duel = ks::spawnDuel(*sim, *gd, *data, context);
		}
		openSession();
	}

	// MQ13: a standing actor reaches a ground item from 42 units, and the scenario's Katana and Bread lie 17 and 22 from the player. The tests of
	// queued orders and of a save in the middle of a pick-up walk need a real walk, so they move both items to walkable ground at least `minDist` from
	// the player first (the near case is pinned on its own). The client hears of it through the items' versions.
	void farItems(double minDist = 80.0, bool refresh = true)
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		const state::Fighter& who = player();
		static const double dirs[8][2] = {{1.0, 0.6}, {-1.0, 0.8}, {0.6, -1.0}, {-0.8, -1.0}, {1.0, -0.3}, {-1.0, 0.3}, {0.0, 1.0}, {0.0, -1.0}};
		std::vector<flecs::entity> items;
		ecs.each([&](flecs::entity e, const state::GroundItem&) { items.push_back(e); });
		std::size_t first = 0;
		for (flecs::entity e : items)
		{
			bool placed = false;
			for (std::size_t k = 0; k < 8 && !placed; ++k)
			{
				const double* d = dirs[(first + k) % 8];
				const double norm = std::hypot(d[0], d[1]);
				const auto spot = sim->navigation()->walker().snap({who.x + d[0] / norm * (minDist + 30.0), who.y, who.z + d[1] / norm * (minDist + 30.0)}, 30.0);
				if (!spot || std::hypot(spot->x - who.x, spot->z - who.z) < minDist)
					continue;
				if (!sim->navigation()->routeFrom({who.x, who.y, who.z}, spot->x, spot->z, spot->y).found)
					continue;
				state::GroundItem g = *e.get<state::GroundItem>();
				g.x = g.useX = spot->x;
				g.y = g.useY = spot->y;
				g.z = g.useZ = spot->z;
				++g.version;
				e.set<state::GroundItem>(std::move(g));
				placed = true;
				first = (first + k + 3) % 8;
			}
			test::require(placed, "no walkable spot " + std::to_string(minDist) + " units from the player for a ground item");
		}
		if (refresh)
			run(2);
	}

	void openSession()
	{
		session = std::make_unique<ks::Session>(*sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session->onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session->sendTick(w, true);
		parse(bytes);
	}

	void parse(const std::vector<std::uint8_t>& bytes)
	{
		navBytesLastParse = 0;
		for (std::size_t p = 0; p < bytes.size();)
		{
			const std::uint32_t len = at<std::uint32_t>(bytes, p);
			const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
			const std::vector<std::uint8_t> body(bytes.begin() + static_cast<long>(p) + 6, bytes.begin() + static_cast<long>(p) + 4 + len);
			p += 4 + len;
			if (type == ks::kNavParams)
			{
				test::require(body.size() == 36, "NAV_PARAMS length");
				NavParams n;
				n.meshId = at<std::uint32_t>(body, 0);
				n.detourVersion = at<std::uint32_t>(body, 4);
				for (int i = 0; i < 3; ++i)
					n.origin[i] = at<float>(body, 8 + 4 * static_cast<std::size_t>(i));
				n.tileWidth = at<float>(body, 20);
				n.tileHeight = at<float>(body, 24);
				n.maxTiles = at<std::uint32_t>(body, 28);
				n.maxPolys = at<std::uint32_t>(body, 32);
				navParams.push_back(n);
			} else if (type == ks::kNavTile) {
				test::require(!navParams.empty() && at<std::uint32_t>(body, 0) == navParams.back().meshId, "NAV_TILE before NAV_PARAMS or from another mesh");
				const std::pair<int, int> key{at<std::int32_t>(body, 4), at<std::int32_t>(body, 8)};
				const std::uint32_t size = at<std::uint32_t>(body, 20);
				test::require(body.size() == 24 + size && at<std::int32_t>(body, 12) == 0, "NAV_TILE length");
				if (size == 0)
				{
					navTiles.erase(key);
					navHeld.erase(key);
					navRemoved.push_back(key);
				} else {
					navTiles[key] = std::vector<std::uint8_t>(body.begin() + 24, body.end());
					navHeld[key] = at<std::uint32_t>(body, 16);
					navOrder.push_back(key);
					navBytesLastParse += size;
					navMaxParse = std::max(navMaxParse, navBytesLastParse);
				}
			} else if (type == ks::kInventory) {
				Inv& inv = inventories[at<std::uint64_t>(body, 8)];
				inv.version = at<std::uint32_t>(body, 16);
				const std::uint16_t n = at<std::uint16_t>(body, 20);
				test::require(body.size() == 22u + 9u * n, "INVENTORY length");
				inv.items.clear();
				for (std::size_t i = 0; i < n; ++i)
					inv.items.push_back({session->strings().text(at<std::uint32_t>(body, 22 + 9 * i)), at<std::uint32_t>(body, 26 + 9 * i), body[30 + 9 * i],
						{}, {}, -1.0f, 255});
				++inv.records;
			} else if (type == ks::kItemGrid) {
				test::require(inventories[at<std::uint64_t>(body, 8)].version == at<std::uint32_t>(body, 16), "ITEM_GRID does not follow an INVENTORY of its version");
				++gridRecords[at<std::uint64_t>(body, 8)];
			} else if (type == ks::kItemQuality) {
				// ITEM_QUALITY follows its INVENTORY record: tick u64, id u64, version u32, n u16, n x (index u16, maker, model u32, level f32, grade u8).
				Inv& inv = inventories[at<std::uint64_t>(body, 8)];
				test::require(inv.version == at<std::uint32_t>(body, 16), "ITEM_QUALITY does not follow an INVENTORY of its version");
				const std::uint16_t n = at<std::uint16_t>(body, 20);
				test::require(n > 0 && body.size() == 22u + 15u * n, "ITEM_QUALITY length");
				const auto text = [&](std::size_t at32) { const std::uint32_t id = at<std::uint32_t>(body, at32); return id == 0 ? std::string() : session->strings().text(id); };
				for (std::size_t i = 0; i < n; ++i)
				{
					InvEntry& entry = inv.items.at(at<std::uint16_t>(body, 22 + 15 * i));
					entry.maker = text(24 + 15 * i);
					entry.model = text(28 + 15 * i);
					entry.level = at<float>(body, 32 + 15 * i);
					entry.grade = body[36 + 15 * i];
				}
			} else if (type == ks::kMedical) {
				const std::uint8_t n = body[20];
				test::require(body.size() == 23u + 12u * n + 14u + 13u * n && n > 0, "CHAR_MEDICAL length (with the trailing severed u16 and the panel tail)");
				++medicalRecords[at<std::uint64_t>(body, 8)];
				medicalBlood[at<std::uint64_t>(body, 8)] = at<float>(body, 16);
				for (std::size_t i = 0; i < n; ++i)
					test::require(!session->strings().text(at<std::uint32_t>(body, 21 + 12 * i)).empty(), "CHAR_MEDICAL part name");
			} else if (type == ks::kAppearance) {
				names[at<std::uint64_t>(body, 8)] = session->strings().text(at<std::uint32_t>(body, body.size() - 8)); // name, then the protocol 5 portraitSerial
			} else if (type == ks::kDynamic) {
				const std::uint32_t dynFlags = at<std::uint32_t>(body, 8 + 8 + 12 + 4 + 4 + 4);
				flags[at<std::uint64_t>(body, 8)] = dynFlags;
				carriedBy[at<std::uint64_t>(body, 8)] = at<std::uint64_t>(body, 8 + 8 + 12 + 4 + 4 + 4 + 4);
				// The rest tail (task BG): f32 restYaw after actionFlags while flag bit 5 (prone) is set and bits 15 / 16 are
				// clear. The aim tail (task aim-tail): f32 aimYaw at the very end while bit 20 is set, after the rest tail.
				// Otherwise actionFlags is the last byte.
				const bool restTail = (dynFlags & (1u << 5)) != 0 && (dynFlags & ((1u << 15) | (1u << 16))) == 0;
				const bool aimTail = (dynFlags & (1u << 20)) != 0;
				const std::size_t tail = aimTail ? 4u : 0u;
				if (aimTail)
				{
					const float yaw = at<float>(body, body.size() - 4);
					test::require(yaw > -3.141593f && yaw <= 3.141593f, "CHAR_DYNAMIC aimYaw is not in (-pi, pi]");
				}
				if (restTail)
				{
					const float yaw = at<float>(body, body.size() - 4 - tail);
					test::require(yaw >= 0.0f && yaw < 6.283186f, "CHAR_DYNAMIC restYaw is not in [0, 2 pi)");
					test::require(body[body.size() - 5 - tail] == 0, "CHAR_DYNAMIC actionFlags: only the operate action loops (no producer here)");
				} else {
					test::require(body[body.size() - 1 - tail] == 0, "CHAR_DYNAMIC actionFlags: only the operate action loops (no producer here)");
				}
			} else if (type == ks::kNavPolyFlags) {
				test::require(!navParams.empty() && at<std::uint32_t>(body, 0) == navParams.back().meshId, "NAV_POLY_FLAGS before NAV_PARAMS or from another mesh");
				const std::pair<int, int> key{at<std::int32_t>(body, 4), at<std::int32_t>(body, 8)};
				const std::uint32_t version = at<std::uint32_t>(body, 16);
				const std::uint16_t n = at<std::uint16_t>(body, 20);
				test::require(body.size() == 22u + 5u * n && at<std::int32_t>(body, 12) == 0 && n > 0, "NAV_POLY_FLAGS length");
				PatchSeen seen{key, version, false, n};
				// The client applies a patch only on exactly version - 1; anything else waits for a NAV_TILE.
				if (navHeld.count(key) != 0 && navHeld[key] + 1 == version)
				{
					std::vector<std::uint8_t>& blob = navTiles[key];
					const auto* header = reinterpret_cast<const dtMeshHeader*>(blob.data());
					const std::size_t polys = static_cast<std::size_t>(dtAlign4(sizeof(dtMeshHeader)))
						+ static_cast<std::size_t>(dtAlign4(sizeof(float) * 3 * static_cast<std::size_t>(header->vertCount)));
					for (std::size_t i = 0; i < n; ++i)
					{
						const std::size_t item = 22 + 5 * i;
						const std::uint16_t poly = at<std::uint16_t>(body, item);
						test::require(poly < header->polyCount, "a patched polygon is not in the tile");
						unsigned char* at4 = blob.data() + polys + static_cast<std::size_t>(poly) * sizeof(dtPoly);
						std::memcpy(at4 + offsetof(dtPoly, flags), &body[item + 2], 2);
						std::memcpy(at4 + offsetof(dtPoly, areaAndtype), &body[item + 4], 1);
					}
					navHeld[key] = version;
					seen.applied = true;
				}
				navPatches.push_back(seen);
			} else if (type == ks::kInteractable) {
				// A bed (kind 5) carries its own tail after the ownership tail: ksim_player_sleep's record, ignored here.
				const bool bed = body[16] == ks::kInteractableBed;
				const bool box = body[16] == ks::kInteractableContainer;
				test::require(body.size() == (bed ? 94u : box ? 84u : 74u), "INTERACTABLE length " + std::to_string(body.size()));
				if (bed)
					continue;
				Inter r;
				r.id = at<std::uint64_t>(body, 8);
				r.kind = body[16];
				r.record = session->strings().text(at<std::uint32_t>(body, 17));
				for (int i = 0; i < 3; ++i)
				{
					r.pos[i] = at<float>(body, 21 + 4 * static_cast<std::size_t>(i));
					r.use[i] = at<float>(body, 33 + 4 * static_cast<std::size_t>(i));
				}
				r.state = body[45];
				r.health = at<float>(body, 46);
				r.maxHealth = at<float>(body, 50);
				r.count = at<std::uint16_t>(body, 54);
				r.version = at<std::uint32_t>(body, 56);
				const std::uint32_t owner = at<std::uint32_t>(body, 60);
				r.owner = owner == 0 ? std::string() : session->strings().text(owner);
				r.lockLevel = at<std::uint16_t>(body, 64);
				const std::uint32_t model = at<std::uint32_t>(body, 66);
				r.model = model == 0 ? std::string() : session->strings().text(model);
				r.yaw = at<float>(body, 70);
				if (box)
				{
					r.building = at<std::uint64_t>(body, 74);
					r.piece = at<std::uint16_t>(body, 82);
				}
				inters[r.id] = r;
			} else if (type == ks::kEvent) {
				const std::uint16_t kind = at<std::uint16_t>(body, 8);
				if (kind == 9)
					replies[at<std::uint32_t>(body, 10)].applied = true;
				else if (kind == 10)
				{
					Reply& r = replies[at<std::uint32_t>(body, 10)];
					r.rejected = true;
					r.reason = session->strings().text(at<std::uint32_t>(body, 14));
				} else if (kind == 2) {
					test::require(body.size() == 10 + 12 + 4 && at<float>(body, 22) > 0.0f, "event 2 bleed drip");
					++bleedDrips;
				} else if (kind == 11) {
					const std::uint64_t id = at<std::uint64_t>(body, 10);
					left.push_back(id);
					inters.erase(id);
				} else if (kind == 6) {
					test::require(body.size() == 10 + 8 + 4 + 12, "event 6 length");
					dropped.push_back({at<std::uint64_t>(body, 10), session->strings().text(at<std::uint32_t>(body, 18))});
				} else if (kind == 7) {
					test::require(body.size() == 10 + 8 + 4 + 12, "event 7 length");
					picked.push_back({at<std::uint64_t>(body, 10), session->strings().text(at<std::uint32_t>(body, 18))});
				} else if (kind == 14) {
					test::require(body.size() == 10 + 8 + 8 + 4, "event 14 length");
					thefts.push_back({at<std::uint64_t>(body, 10), at<std::uint64_t>(body, 18), session->strings().text(at<std::uint32_t>(body, 26))});
				} else if (kind == 13) {
					test::require(body.size() == 10 + 8 + 8 + 2, "event 13 length");
					done.push_back({at<std::uint64_t>(body, 10), at<std::uint64_t>(body, 18), body[26], body[27]});
				}
			}
		}
	}

	void tick()
	{
		sim->tick();
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->sendTick(w, false);
		parse(bytes);
	}
	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
			tick();
	}

	// One order frame through the session; returns its clientSeq.
	std::uint32_t order(std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", kind);
		body(w);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "an order kicked the client (kind " + std::to_string(kind) + ")");
		parse(out);
		return seq;
	}

	std::uint32_t interact(std::uint64_t actor, std::uint64_t target, std::uint8_t action, std::uint8_t queue = 0)
	{
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", actor);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", queue);
		});
	}

	// Order 60 with the trailing flags byte (SIMLINK.md section 13): loot open is bit 0.
	std::uint32_t interactFlags(std::uint64_t actor, std::uint64_t target, std::uint8_t action, std::uint8_t flags)
	{
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", actor);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", 0);
			w.u8("flags", flags);
		});
	}

	// A group interact order: every id walks to the target.
	std::uint32_t interactGroup(const std::vector<std::uint64_t>& actors, std::uint64_t target, std::uint8_t action, std::uint8_t queue)
	{
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", static_cast<std::uint16_t>(actors.size()));
			for (const std::uint64_t id : actors)
				w.u64("id", id);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", queue);
		});
	}

	std::uint32_t moveToFlags(const std::vector<std::uint64_t>& actors, double x, double z, std::uint8_t flags)
	{
		return order(1, [&](ks::BinaryWriter& w)
		{
			w.u16("n", static_cast<std::uint16_t>(actors.size()));
			for (const std::uint64_t id : actors)
				w.u64("id", id);
			w.f32("x", static_cast<float>(x));
			w.f32("z", static_cast<float>(z));
			w.f32("y", 0.0f);
			w.u8("flags", flags);
		});
	}

	std::size_t queued(std::uint64_t id)
	{
		std::size_t n = 0;
		sim->world().ecs().each([&](const state::Id& i, const state::OrderQueue& q) { if (i.value == id) n = q.orders.size(); });
		return n;
	}

	// The event 13 records of one actor for one target, in order.
	std::vector<std::uint8_t> resultsOf(std::uint64_t who, std::uint64_t target) const
	{
		std::vector<std::uint8_t> out;
		for (const Done& d : done)
		{
			if (d.who == who && d.target == target)
				out.push_back(d.result);
		}
		return out;
	}

	std::uint32_t drop(std::uint64_t actor, const std::string& item, std::uint16_t count)
	{
		return order(61, [&](ks::BinaryWriter& w)
		{
			w.u64("id", actor);
			w.str("item", item);
			w.u16("count", count);
		});
	}

	// Order 67: equip / unequip the entry `index` of the INVENTORY the client holds (its version).
	std::uint32_t equipEntry(std::uint64_t actor, std::uint16_t index, bool on)
	{
		const std::uint32_t version = inventories[actor].version;
		return order(67, [&](ks::BinaryWriter& w)
		{
			w.u64("id", actor);
			w.u32("version", version);
			w.u16("index", index);
			w.u8("equip", on ? 1 : 0);
		});
	}

	std::uint32_t equip(std::uint64_t actor, const std::string& item, bool on)
	{
		return order(62, [&](ks::BinaryWriter& w)
		{
			w.u64("id", actor);
			w.str("item", item);
			w.u8("equip", on ? 1 : 0);
		});
	}

	std::uint32_t transfer(std::uint64_t actor, std::uint64_t container, const std::string& item, std::uint16_t count, bool take)
	{
		return order(63, [&](ks::BinaryWriter& w)
		{
			w.u64("id", actor);
			w.u64("container", container);
			w.str("item", item);
			w.u16("count", count);
			w.u8("take", take ? 1 : 0);
		});
	}

	const state::Inventory* inventoryOf(std::uint64_t id) const
	{
		const state::Inventory* found = nullptr;
		sim->world().ecs().each([&](const state::Id& i, const state::Inventory& v) { if (i.value == id) found = &v; });
		return found;
	}

	std::uint32_t moveTo(std::uint64_t actor, double x, double z)
	{
		return order(1, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", actor);
			w.f32("x", static_cast<float>(x));
			w.f32("z", static_cast<float>(z));
			w.f32("y", 0.0f);
			w.u8("flags", 0);
		});
	}

	// The order must be answered with event 10 and this reason.
	void expectReject(std::uint32_t s, const std::string& reason)
	{
		test::require(replies[s].rejected && replies[s].reason == reason,
			"expected '" + reason + "', got " + (replies[s].rejected ? replies[s].reason : std::string(replies[s].applied ? "applied" : "nothing")));
	}
	void expectApplied(std::uint32_t s)
	{
		test::require(replies[s].applied && !replies[s].rejected, "order not applied: " + replies[s].reason);
	}

	const state::Fighter& player() const { return *sim->combat()->find(duel.player); }
	const state::Fighter& enemy() const { return *sim->combat()->find(duel.enemy); }
	state::Fighter& mutableFighter(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }

	// The sim's door entity with this id.
	const state::Door* door(std::uint64_t id) const
	{
		const state::Door* found = nullptr;
		sim->world().ecs().each([&](const state::Id& i, const state::Door& d) { if (i.value == id) found = &d; });
		return found;
	}
	bool exists(std::uint64_t id) const { return findInteractTarget(sim->world().ecs(), sim->combat(), id).kind != InteractKind::None; }
	std::optional<Done> doneFor(std::uint64_t target, std::size_t from = 0) const
	{
		for (std::size_t i = from; i < done.size(); ++i)
		{
			if (done[i].target == target)
				return done[i];
		}
		return std::nullopt;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_interact HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		Rig rig(gd, data, nav, order);

		// ---- the scenario's interactables reach the client ----
		std::uint64_t katana = 0;
		std::uint64_t bread = 0;
		std::vector<Inter> doors;
		for (const auto& [id, r] : rig.inters)
		{
			if (r.kind == ks::kInteractableItem && r.count == 1)
				katana = id;
			if (r.kind == ks::kInteractableItem && r.count == 3)
				bread = id;
			if (r.kind == ks::kInteractableDoor)
				doors.push_back(r);
		}
		test::require(katana != 0 && bread != 0, "the duel's ground items are missing from the snapshot");
		test::require(rig.inters[katana].record != rig.inters[bread].record, "the two items share a record");
		test::require(!doors.empty(), "no door in the detail bounds of the Hub");
		for (const Inter& d : doors)
		{
			// Gates start open (the Hub is not sealed in); every other door starts closed or locked.
			bool gate = false;
			rig.sim->world().ecs().each([&](const state::Id& id, const state::Door& door) { if (id.value == d.id) gate = door.gate; });
			test::require(d.version == 1 && (gate ? d.state == 0 : d.state == 1 || d.state == 2) && d.maxHealth > 0
				&& d.health == d.maxHealth && !d.record.empty(), "a door record is wrong");
			test::require(d.lockLevel > 0 && d.model.empty() && d.yaw == 0.0f, "the ownership tail of a door: its lock level, no model, no yaw");
		}
		const std::string katanaRecord = rig.inters[katana].record;
		// The quality the duel scenario's ground Katana was generated with (grade 2: rollQuality's fixed rolls 0.75 / 0).
		const state::ItemQuality katanaQuality = data->rollQuality(*gd, *gd->find(katanaRecord), 0.75, 0.0);
		const std::string breadRecord = rig.inters[bread].record;
		// The ownership tail of the ground items: nobody's (dropped), the Katana's quality model, a rest yaw in [0, 2 pi) fixed by the Id.
		{
			const Inter& k = rig.inters[katana];
			const Inter& b = rig.inters[bread];
			test::require(k.owner.empty() && b.owner.empty() && k.lockLevel == 0 && k.model == katanaQuality.model && !k.model.empty()
				&& b.model.empty() && k.yaw >= 0.0f && k.yaw < 6.2832f && b.yaw >= 0.0f && b.yaw < 6.2832f && k.yaw != b.yaw,
				"the ground items' ownership tail (owner none, the Katana's model, a rest yaw)");
		}
		// INVENTORY: the player's character only, with its worn armour as equipped entries; names and the run flag.
		{
			test::require(rig.inventories.count(rig.duel.player) == 1 && rig.inventories.count(rig.duel.enemy) == 0,
				"INVENTORY must be sent for the player's characters only");
			const Rig::Inv& inv = rig.inventories[rig.duel.player];
			test::require(inv.version == 1 && inv.records == 1 && inv.items.size() == 1, "the player's first INVENTORY record is wrong");
			test::require(inv.items[0].slot >= state::Inventory::kArmourBase && inv.items[0].count == 1 && !inv.items[0].item.empty(),
				"the starting armour is not an equipped entry");
			test::require(rig.names[rig.duel.player] == "Engival" && rig.names[rig.duel.enemy] == "Escaped Servant",
				"CHAR_APPEARANCE names: " + rig.names[rig.duel.player] + " / " + rig.names[rig.duel.enemy]);
			test::require((rig.flags[rig.duel.player] & ks::kFlagRun) != 0, "a new character is not running");
			rig.order(10, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", rig.duel.player); w.u8("which", 2); w.u8("value", 0); });
			rig.run(1);
			test::require((rig.flags[rig.duel.player] & ks::kFlagRun) == 0 && rig.player().moveMode == 0, "toggle 2 value 0 did not clear the run flag");
			rig.order(10, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", rig.duel.player); w.u8("which", 2); w.u8("value", 1); });
			rig.run(1);
			test::require((rig.flags[rig.duel.player] & ks::kFlagRun) != 0, "toggle 2 value 1 did not set the run flag");
			// Flag bit 13: order 4 hold = 1 sets it, hold = 0 (a plain stop) clears it.
			test::require((rig.flags[rig.duel.player] & ks::kFlagHold) == 0, "a new character is in hold");
			rig.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", rig.duel.player); w.u8("hold", 1); });
			rig.run(1);
			test::require((rig.flags[rig.duel.player] & ks::kFlagHold) != 0 && rig.player().hold, "order 4 hold 1 did not set the hold flag");
			rig.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", rig.duel.player); w.u8("hold", 0); });
			rig.run(1);
			test::require((rig.flags[rig.duel.player] & ks::kFlagHold) == 0 && !rig.player().hold, "order 4 hold 0 did not clear the hold flag");
			// Flag bit 14 hostile: the duel's servant (another team) has it, the player never; every other character follows enemy().
			test::require((rig.flags[rig.duel.enemy] & ks::kFlagHostile) != 0, "the duel's enemy is not flagged hostile");
			test::require((rig.flags[rig.duel.player] & ks::kFlagHostile) == 0, "the player's own character is flagged hostile");
			for (const auto& [id, flags] : rig.flags)
			{
				const state::Fighter* f = rig.sim->combat()->find(id);
				if (f == nullptr)
					continue;
				const bool hostile = !f->playerControlled && !f->dead && f->team != rig.player().team;
				test::require(((flags & ks::kFlagHostile) != 0) == hostile, "flag bit 14 disagrees with enemy() for " + std::to_string(id));
			}
		}
		// CHAR_MEDICAL (tick, id, blood, n, n x {part, health, stun}): every character in view gets one on first sight.
		test::require(rig.medicalRecords[rig.duel.player] >= 1 && rig.medicalRecords[rig.duel.enemy] >= 1, "no CHAR_MEDICAL for the duel");
		test::require(rig.medicalBlood[rig.duel.player] > 50.0f, "CHAR_MEDICAL blood is not a blood amount");
		std::cout << rig.inters.size() << " interactables in the first snapshot (" << doors.size() << " doors)\n";
		// A second tick sends nothing new for them (records only on change).
		const std::size_t before = rig.inters.size();
		rig.inters.clear();
		rig.tick();
		test::require(rig.inters.empty(), "an unchanged interactable was sent again");
		rig.inters.clear();
		(void) before;

		// ---- the navmesh tile stream: a client-side mesh from the received tiles steps like the server's ----
		{
			rig.run(3);
			test::require(rig.navParams.size() == 1 && rig.navParams[0].detourVersion != 0, "NAV_PARAMS once, with a Detour version");
			test::require(!rig.navTiles.empty() && rig.navRemoved.empty(), "no tiles streamed");
			test::require(rig.navMaxParse <= (256u << 10) + 65536u, "a tick sent more than the 256 KiB tile budget");
			const Rig::NavParams& p = rig.navParams[0];
			const knav::NavConfig& serverCfg = nav->mesh().config();
			test::require(p.tileWidth == serverCfg.tileSize && p.origin[0] == static_cast<float>(serverCfg.originX), "NAV_PARAMS differ from the server's mesh");
			// The first tile sent is the one nearest the first detail box's centre (the view zone is absent: the player's box).
			std::vector<knav::BakedTile> tiles;
			knav::NavConfig cfg = serverCfg;
			cfg.originX = p.origin[0];
			cfg.originZ = p.origin[2];
			cfg.tileSize = p.tileWidth;
			cfg.window = knav::TileRect{1 << 20, 1 << 20, -(1 << 20), -(1 << 20)};
			int x0 = 1 << 20, z0 = 1 << 20, x1 = -(1 << 20), z1 = -(1 << 20);
			for (const auto& [key, data] : rig.navTiles)
			{
				knav::BakedTile t;
				t.x = key.first;
				t.z = key.second;
				t.data = data;
				tiles.push_back(std::move(t));
				x0 = std::min(x0, key.first);
				x1 = std::max(x1, key.first);
				z0 = std::min(z0, key.second);
				z1 = std::max(z1, key.second);
			}
			int doorPaintedTiles = 0;
			std::sort(tiles.begin(), tiles.end(), [](const knav::BakedTile& a, const knav::BakedTile& b)
				{ return knav::TileCoord{a.x, a.z} < knav::TileCoord{b.x, b.z}; });
			cfg.window = knav::TileRect{x0, z0, x1, z1};
			for (const knav::BakedTile& t : tiles) // byte-identical to the server's tiles
			{
				const knav::BakedTile* server = nav->mesh().tile(t.x, t.z);
				test::require(server != nullptr && nav->mesh().tileBlob(t.x, t.z) == t.data, "a streamed tile differs from the server's");
				test::require(rig.navHeld[{t.x, t.z}] == nav->mesh().tileVersion(t.x, t.z), "a streamed tile has the wrong version");
				doorPaintedTiles += server->data != t.data; // the closed and locked doors are painted on it
			}
			test::require(doorPaintedTiles > 0, "no streamed tile carries a door's paint");
			knav::NavMesh::DetourParams detour;
			for (int i = 0; i < 3; ++i)
				detour.origin[i] = p.origin[i];
			detour.tileWidth = p.tileWidth;
			detour.tileHeight = p.tileHeight;
			detour.maxTiles = p.maxTiles;
			detour.maxPolys = p.maxPolys;
			detour.version = p.detourVersion;
			(void) cfg;
			knav::NavMesh clientMesh = knav::NavMesh::fromParams(detour, std::move(tiles));
			// setPolyFlags: repaint one door polygon on the client's mesh and read it back.
			{
				knav::PolyLoc loc;
				bool have = false;
				for (const knav::DoorKey key : nav->mesh().doorKeys())
				{
					for (const knav::PolyLoc& candidate : nav->mesh().doorPolys(key))
					{
						if (!have && rig.navTiles.count({candidate.tileX, candidate.tileZ}) != 0)
						{
							loc = candidate;
							have = true;
						}
					}
				}
				test::require(have, "no door polygon on a streamed tile");
				const dtPoly& poly = clientMesh.detour().getTileAt(loc.tileX, loc.tileZ, 0)->polys[loc.poly];
				const std::uint16_t flags = poly.flags;
				const std::uint8_t area = poly.getArea();
				test::require(clientMesh.setPolyFlags(loc.tileX, loc.tileZ, 0, loc.poly, 0x1234, 3), "setPolyFlags refused a door polygon");
				test::require(poly.flags == 0x1234 && poly.getArea() == 3, "setPolyFlags did not repaint the polygon");
				test::require(clientMesh.setPolyFlags(loc.tileX, loc.tileZ, 0, loc.poly, flags, area), "setPolyFlags could not restore");
				test::require(!clientMesh.setPolyFlags(loc.tileX, loc.tileZ, 0, 100000, 0, 0) && !clientMesh.setPolyFlags(1 << 18, 0, 0, 0, 0, 0),
					"setPolyFlags accepted a polygon that is not there");
			}
			const knav::Walker clientWalker(clientMesh);
			const knav::Walker& serverWalker = nav->walker();
			const state::Fighter& pl = rig.player();
			int compared = 0;
			int moved = 0;
			for (int k = 0; k < 64; ++k)
			{
				const double a = k * 0.0981748;
				const double d = 1.0 + 3.0 * (k % 7);
				const knav::Position from{pl.x + 5.0 * std::sin(a * 3.0), pl.y, pl.z + 5.0 * std::cos(a * 3.0)};
				const knav::StepResult s = serverWalker.step(from, d * std::sin(a), d * std::cos(a));
				const knav::StepResult c = clientWalker.step(from, d * std::sin(a), d * std::cos(a));
				test::require(s.onMesh == c.onMesh && s.blocked == c.blocked && s.at.x == c.at.x && s.at.y == c.at.y && s.at.z == c.at.z,
					"a Walker step on the streamed mesh differs from the server's, step " + std::to_string(k));
				++compared;
				moved += s.at.x != from.x || s.at.z != from.z;
			}
			test::require(moved > 8, "the steps barely moved; the comparison is empty");
			std::cout << "nav stream: " << rig.navTiles.size() << " tiles, " << compared << " steps match\n";
			rig.navRemoved.clear();
		}

		// ---- rejections ----
		const std::uint64_t self = rig.duel.player;
		rig.expectReject(rig.interact(self, 987654321, 1), "bad_target");
		rig.expectReject(rig.interact(self, katana, 1), "bad_target"); // open an item
		rig.expectReject(rig.interact(self, doors.front().id, 7), "bad_target"); // pick up a door
		rig.expectReject(rig.interact(self, rig.duel.enemy, 5), "not_allowed"); // loot a standing character
		rig.expectReject(rig.interact(self, rig.duel.enemy, 10), "not_allowed"); // first aid on an unhurt one
		rig.expectReject(rig.interact(self, self, 5), "bad_target"); // loot yourself
		rig.expectReject(rig.interact(self, doors.front().id, 4), "bad_target"); // use works producers only (ksim_producers); a door is none
		rig.expectReject(rig.interact(self, doors.front().id, 6), "bad_target"); // talk (trade.hpp) is for characters: a door is no one to talk to (was `unsupported` before M4)
		rig.expectReject(rig.interact(self, rig.duel.enemy, 8), "not_allowed"); // carry (lift) a standing character
		rig.expectReject(rig.interact(self, doors.front().id, 8), "bad_target"); // lift a door
		rig.expectReject(rig.interact(self, self, 8), "bad_target"); // lift yourself
		rig.expectReject(rig.interact(self, rig.duel.enemy, 13), "not_allowed"); // put down somebody who is not carried
		rig.expectReject(rig.interact(self, doors.front().id, 13), "bad_target");
		rig.expectReject(rig.interact(self, doors.front().id, 21), "bad_value"); // above the last action (20, the cage orders)
		rig.expectReject(rig.interact(self, doors.front().id, 0), "bad_value");
		rig.expectReject(rig.interact(self, doors.front().id, 99), "bad_value");
		rig.expectReject(rig.interact(self, doors.front().id, 1, 2), "bad_value"); // queue above 1
		rig.expectReject(rig.interact(rig.duel.enemy, katana, 7), "not_player_controlled");
		rig.expectReject(rig.interact(555555, katana, 7), "unknown_id");
		rig.expectReject(rig.interact(self, doors.front().id, 2), "not_allowed"); // close a shut door
		// a locked door refuses to open
		const Inter* locked = nullptr;
		for (const Inter& d : doors)
		{
			if (d.state == 2)
				locked = &d;
		}
		if (locked != nullptr)
			rig.expectReject(rig.interact(self, locked->id, 1), "not_allowed");
		else
			std::cout << "(no locked door in view; the locked refusal is checked on a door set locked below)\n";
		test::require(rig.sim->pendingCommands() == 0, "a rejected order queued a command");

		// ---- pick up an item ----
		// An owned one (a placement item's case): taking it is theft (event 14), the item as the target.
		rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::GroundItem&)
		{
			if (i.value == katana)
				e.get_mut<state::GroundItem>()->ownerSid = "someone-else";
		});
		const std::size_t doneBefore = rig.done.size();
		rig.expectApplied(rig.interact(self, katana, 7));
		rig.run(1);
		test::require(rig.player().routeVersion != 0 || !rig.player().walking(), "no route for the pick-up");
		int ticks = 0;
		while (rig.exists(katana) && ticks++ < 600)
			rig.tick();
		test::require(!rig.exists(katana), "the item was never picked up");
		rig.run(2);
		test::require(rig.picked.size() == 1 && rig.picked[0].who == self && rig.picked[0].item == katanaRecord, "event 7 is wrong");
		test::require(std::find(rig.left.begin(), rig.left.end(), katana) != rig.left.end(), "the picked item's id never left (event 11)");
		const auto got = rig.doneFor(katana, doneBefore);
		test::require(got && got->who == self && got->action == 7 && got->result == 0, "event 13 for the pick-up");
		test::require(rig.thefts.size() == 1 && rig.thefts[0].who == self && rig.thefts[0].container == katana && rig.thefts[0].owner == "someone-else",
			"picking up an owned ground item is theft (event 14)");
		{
			const flecs::world& ecs = rig.sim->world().ecs();
			const state::Inventory* inv = nullptr;
			ecs.each([&](const state::Id& i, const state::Inventory& v) { if (i.value == self) inv = &v; });
			test::require(inv != nullptr && inv->carried(katanaRecord) == 1 && inv->version == 2,
				"the inventory does not hold the Katana (version 2)");
			const Rig::Inv& client = rig.inventories[self];
			test::require(client.version == 2 && client.records == 2, "the client was not sent the changed inventory exactly once");
			bool found = false;
			for (const Rig::InvEntry& entry : client.items)
				found = found || (entry.item == katanaRecord && entry.count == 1 && entry.slot == state::Inventory::kCarried);
			test::require(found, "the client's inventory lacks the picked-up Katana");
			bool quality = false;
			for (const state::Inventory::Entry& entry : inv->items)
				quality = quality || (entry.itemSid == katanaRecord && entry.quality == katanaQuality);
			test::require(quality, "the picked-up Katana lost its generated quality");
		}
		std::cout << "picked up in " << ticks << " ticks\n";

		// The bread: three, merged into one stack in the inventory; a second pick-up of the same id is bad_target.
		rig.expectApplied(rig.interact(self, bread, 7));
		for (ticks = 0; rig.exists(bread) && ticks < 600; ++ticks)
			rig.tick();
		rig.run(2);
		rig.expectReject(rig.interact(self, bread, 7), "bad_target");

		// ---- open and close a door ----
		// The nearest shut, unlocked door the player can route to.
		std::sort(doors.begin(), doors.end(), [&](const Inter& a, const Inter& b)
		{
			return std::hypot(a.pos[0] - rig.player().x, a.pos[2] - rig.player().z) < std::hypot(b.pos[0] - rig.player().x, b.pos[2] - rig.player().z);
		});
		std::uint64_t target = 0;
		std::uint32_t s = 0;
		// The nearest door the player can walk to; a walk that ends short of its door (event 13 result 2) tries the next one.
		// Which door that is depends on where the duel left the player, so the combat timing moves it between runs.
		for (const Inter& d : doors)
		{
			if (d.state != 1)
				continue;
			s = rig.interact(self, d.id, 1);
			if (!rig.replies[s].applied)
			{
				test::require(rig.replies[s].reason == "no_route", "an open order was refused with " + rig.replies[s].reason);
				continue;
			}
			rig.inters.clear();
			ticks = 0;
			while (rig.door(d.id)->state != 0 && ticks++ < 4000 && !(rig.doneFor(d.id) && rig.doneFor(d.id)->result != 0))
				rig.tick();
			if (rig.door(d.id)->state == 0)
			{
				target = d.id;
				break;
			}
			std::cout << "door " << d.id << " not reached (walk ended short), trying the next\n";
		}
		test::require(target != 0, "no door the player can walk to");
		test::require(rig.door(target)->state == 0, "the door never opened");
		// The state flips at openness 0.6; the order ends (event 13) when the slab is fully open, 1 / door move speed in all.
		for (int more = 0; more < 400 && !rig.doneFor(target); ++more)
			rig.tick();
		rig.run(2);
		test::require(rig.inters.count(target) == 1 && rig.inters[target].state == 0 && rig.inters[target].version == 2, "the open door was not sent");
		test::require(std::hypot(rig.player().x - rig.door(target)->x, rig.player().z - rig.door(target)->z) <= kInteractReach + 0.5,
			"the door opened from too far");
		const auto opened = rig.doneFor(target);
		test::require(opened && opened->action == 1 && opened->result == 0, "event 13 for the open");
		std::cout << "opened door " << target << " after " << ticks << " ticks\n";
		rig.expectReject(rig.interact(self, target, 1), "not_allowed"); // already open
		rig.expectApplied(rig.interact(self, target, 2));
		rig.run(80); // standing at the door: the close is a timed step (1 / door move speed s: 2 s here)
		test::require(rig.door(target)->state == 1 && rig.door(target)->version == 3, "the door did not close");

		// ---- NAV_POLY_FLAGS: the open and the close reached the client as patches, one version step each ----
		{
			rig.run(2);
			const knav::NavMesh& mesh = nav->mesh();
			std::size_t checked = 0;
			for (const knav::PolyLoc& loc : mesh.doorPolys(rig.door(target)->key))
			{
				const std::pair<int, int> tile{loc.tileX, loc.tileZ};
				if (rig.navHeld.count(tile) == 0)
					continue;
				++checked;
				test::require(rig.navHeld[tile] == mesh.tileVersion(tile.first, tile.second), "the client's tile version is not the server's");
				test::require(rig.navTiles[tile] == mesh.tileBlob(tile.first, tile.second), "the patched client tile differs from the server's");
				std::uint32_t last = 0;
				std::size_t seen = 0;
				for (const Rig::PatchSeen& p : rig.navPatches)
				{
					if (p.tile != tile)
						continue;
					test::require(p.applied, "a patch arrived out of sequence");
					test::require(last == 0 || p.version == last + 1, "tile versions do not step by one");
					last = p.version;
					++seen;
				}
				test::require(seen >= 2 && last == mesh.tileVersion(tile.first, tile.second), "the open and the close sent no patches");
			}
			test::require(checked > 0, "the door's tile is not on the client");
			std::cout << "nav patches: " << rig.navPatches.size() << " sent, tile versions in step\n";
		}

		// A damaged door still opens (CURSORS_LIVE.md): health below max is no obstacle.
		{
			flecs::entity e;
			rig.sim->world().ecs().each([&](flecs::entity entity, const state::Id& i, const state::Door&) { if (i.value == target) e = entity; });
			const double door16 = e.get<state::Door>()->maxHealth;
			// Stand at the door (the timed open and close should not include a walk), and come back to where the block found us.
			const state::Fighter before = rig.player();
			rig.mutableFighter(self).x = e.get<state::Door>()->x + 3.0;
			rig.mutableFighter(self).y = e.get<state::Door>()->y;
			rig.mutableFighter(self).z = e.get<state::Door>()->z;
			const_cast<state::Door*>(e.get<state::Door>())->health = 10.0;
			rig.expectApplied(rig.interact(self, target, 1));
			rig.run(80);
			test::require(rig.door(target)->state == 0, "a damaged door refused to open: state " + std::to_string(rig.door(target)->state) + " speed " + std::to_string(rig.door(target)->moveSpeed) + " steps " + std::to_string(rig.sim->world().ecs().count<state::DoorStep>()) + [&]{ std::string o; rig.sim->world().ecs().each([&](const state::Id& i, const state::DoorStep& st){ o += " [" + std::to_string(i.value) + " door " + std::to_string(st.door) + " phase " + std::to_string(st.phase) + " rem " + std::to_string(st.remaining) + " route " + std::to_string(st.route.size()) + "]"; }); return o; }());
			rig.expectApplied(rig.interact(self, target, 2));
			rig.run(80);
			rig.mutableFighter(self).x = before.x;
			rig.mutableFighter(self).y = before.y;
			rig.mutableFighter(self).z = before.z;
			// Locked: refuses, and open from the order's point of view stays refused.
			const_cast<state::Door*>(e.get<state::Door>())->state = 2;
			rig.expectReject(rig.interact(self, target, 1), "not_allowed");
			rig.expectReject(rig.interact(self, target, 2), "not_allowed");
			const_cast<state::Door*>(e.get<state::Door>())->state = 1;
			const_cast<state::Door*>(e.get<state::Door>())->health = door16;
		}

		// ---- doors and pathing: closed doors are passable (the walker opens them), locked doors block ----
		{
			const knav::NavMesh& mesh = nav->mesh();
			struct DoorRef
			{
				flecs::entity entity;
				std::uint64_t id;
				knav::Point centre;
				std::uint8_t state;
			};
			std::vector<DoorRef> all;
			rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Door& d)
				{ all.push_back({e, i.value, {static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)}, d.state}); });
			const auto setState = [&](const DoorRef& d, std::uint8_t s)
			{
				state::Door* door = const_cast<state::Door*>(d.entity.get<state::Door>());
				door->state = s;
				++door->version;
				rig.tick();
			};
			for (const DoorRef& d : all)
				setState(d, 1);
			// True when the route walks over one of the door's polygons (sampled every unit along its legs).
			const knav::NavQuery probe(mesh, 64);
			const auto through = [&](const Route& route, const DoorRef& d)
			{
				const std::vector<knav::PolyLoc> polys = mesh.doorPolys(rig.door(d.id)->key);
				for (std::size_t i = 1; i < route.points.size(); ++i)
				{
					const double dx = route.points[i].x - route.points[i - 1].x, dz = route.points[i].z - route.points[i - 1].z;
					const int steps = static_cast<int>(std::hypot(dx, dz)) + 1;
					for (int k = 0; k <= steps; ++k)
					{
						const double t = static_cast<double>(k) / steps;
						const knav::Point at{static_cast<float>(route.points[i - 1].x + t * dx),
							static_cast<float>(route.heights[i - 1] + t * (route.heights[i] - route.heights[i - 1])),
							static_cast<float>(route.points[i - 1].z + t * dz)};
						knav::PolyLoc loc;
						if (!probe.locate(at, {0.3f, 3.0f, 0.3f}, loc))
							continue;
						for (const knav::PolyLoc& p : polys)
						{
							if (p.tileX == loc.tileX && p.tileZ == loc.tileZ && p.poly == loc.poly)
								return true;
						}
					}
				}
				return false;
			};
			const auto length = [](const Route& route)
			{
				double sum = 0.0;
				for (std::size_t i = 1; i < route.points.size(); ++i)
					sum += std::hypot(route.points[i].x - route.points[i - 1].x, route.points[i].z - route.points[i - 1].z);
				return sum;
			};
			// A destination whose cheapest route runs through a door; then lock the door.
			// Stand still first: the duel's enemy would draw the idle player into a fight.
			rig.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", self); w.u8("hold", 1); });
			rig.run(2);
			const state::Fighter& pl = rig.player();
			// The move order below routes at the character's clearance (NAVMESH_CACHE.md "Clearance"), so the goal has to be
			// chosen the same way: a route Detour finds at radius 0 can cross a gap narrower than the body.
			const double clearance = rig.sim->pathRadius(pl);
			int blocked = 0;
			int detoured = 0;
			bool walkedThrough = false;
			for (const DoorRef& d : all)
			{
				for (const double r : {25.0, 45.0, 70.0})
				{
					for (int k = 0; k < 12; ++k)
					{
						const double a = k * 0.5235988;
						const double gx = d.centre[0] + std::sin(a) * r;
						const double gz = d.centre[2] + std::cos(a) * r;
						// The player's own spot at this instant: an earlier iteration's applied order can still be walking it, and the
						// server routes from where it stands when the order arrives, not from where the loop started.
						const state::Fighter& live = rig.player();
						// The server's own click goal (simlink move_to: clickGoal, kenshi-k16.8.10), so the test expects what the server routes.
						const knav::Position from{live.x, live.y, live.z};
						const Route closed = nav->routeFrom(from, gx, gz, std::nullopt, false, clearance, true, true);
						if (!closed.found || !through(closed, d))
							continue;
						setState(d, 2);
						const Route shut = nav->routeFrom(from, gx, gz, std::nullopt, false, clearance, true, true);
						if (!shut.found)
						{
							++blocked;
							rig.expectReject(rig.moveTo(self, gx, gz), "no_route");
						} else if (!through(shut, d)) {
							++detoured;
							// (Detour costs polygon to polygon, so the string-pulled lengths of the two routes are not ordered; only the avoidance is claimed.)
						} else {
							throw test::Failure("a route still runs through a locked door");
						}
						// The locked paint reached the client as a patch.
						rig.run(1);
						for (const knav::PolyLoc& loc : mesh.doorPolys(rig.door(d.id)->key))
						{
							const std::pair<int, int> tile{loc.tileX, loc.tileZ};
							if (rig.navHeld.count(tile) != 0)
								test::require(rig.navTiles[tile] == mesh.tileBlob(tile.first, tile.second), "locked door: the client's tile differs");
						}
						// Open again: the route is not much longer than the closed one (open costs less than a closed door; Detour costs
						// polygon to polygon, so the string-pulled lengths are not strictly ordered: 10 % + 5 units of slack).
						setState(d, 0);
						const Route open = nav->routeFrom(from, gx, gz, std::nullopt, false, clearance, true, true);
						test::require(open.found && length(open) <= length(closed) * 1.10 + 5.0, "an open door made the route longer");
						// Closed: the walker takes it and opens the door on the way.
						setState(d, 1);
						if (!walkedThrough && through(nav->routeFrom(from, gx, gz, std::nullopt, false, clearance, true, true), d))
						{
							rig.expectApplied(rig.moveTo(self, gx, gz));
							int walk = 0;
							rig.run(2);
							test::require(rig.player().walking() || rig.sim->world().ecs().count<state::DoorStep>() > 0, "the move order did not start a walk");
							while ((rig.player().walking() || rig.sim->world().ecs().count<state::DoorStep>() > 0) && walk++ < 6000)
							{
								rig.tick();
								if (std::getenv("KDOOR_TRACE") != nullptr && walk % 5 == 0)
								{
									std::string st;
									rig.sim->world().ecs().each([&](const state::Id&, const state::DoorStep& s2) { st += " step door " + std::to_string(s2.door) + " ph " + std::to_string(s2.phase); });
									std::fprintf(stderr, "w %d at %.1f %.1f next %u/%zu%s\n", walk, rig.player().x, rig.player().z, rig.player().routeNext, rig.player().route.size(), st.c_str());
								}
							}
							test::require(rig.door(d.id)->state == 0, "the walker did not open the closed door on its way: door " + std::to_string(d.id) + " state " + std::to_string(rig.door(d.id)->state) + " dist " + std::to_string(std::hypot(rig.player().x - rig.door(d.id)->x, rig.player().z - rig.door(d.id)->z)) + " blocked " + std::to_string(rig.sim->combat()->routesBlocked()) + " reroutes " + std::to_string(rig.sim->combat()->reroutes()) + " enemy " + std::to_string(std::hypot(rig.player().x - rig.enemy().x, rig.player().z - rig.enemy().z)) + [&]{ std::string o; for (const DoorRef& q : all) o += " [door " + std::to_string(q.id) + " st " + std::to_string(rig.door(q.id)->state) + " d " + std::to_string((int) std::hypot(rig.player().x - rig.door(q.id)->x, rig.player().z - rig.door(q.id)->z)) + "]"; return o; }() + " walk ticks " + std::to_string(walk) + " goal " + std::to_string(gx) + "," + std::to_string(gz) + " player " + std::to_string(rig.player().x) + "," + std::to_string(rig.player().z));
							walkedThrough = true;
							setState(d, 1);
							// Back to where the searches start from.
							state::Fighter& back = rig.mutableFighter(self);
							back.x = from.x;
							back.y = from.y;
							back.z = from.z;
							rig.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", self); w.u8("hold", 1); });
							rig.run(2);
						}
					}
				}
			}
			std::cout << "doors and pathing: " << blocked << " locked doors gave no_route, " << detoured << " gave a longer detour, walked through "
				<< walkedThrough << "\n";
			test::require(blocked + detoured > 0, "no route in the Hub runs through a door");
			test::require(walkedThrough, "no walk through a closed door was tested");
			for (const DoorRef& d : all)
				setState(d, d.state);
		}

		// Every door of the sim but the one opened above, nearest the player first. The snapshot's detail bounds hold one door
		// since the Hub's ruins lost theirs (7d6ac14), so the walks below go to doors outside them.
		const auto otherDoors = [&]
		{
			std::vector<std::pair<double, std::uint64_t>> near;
			rig.sim->world().ecs().each([&](const state::Id& i, const state::Door& d)
			{
				if (i.value != target)
					near.push_back({std::hypot(d.x - rig.player().x, d.z - rig.player().z), i.value});
			});
			std::sort(near.begin(), near.end());
			std::vector<std::uint64_t> ids;
			for (const auto& [far, id] : near)
				ids.push_back(id);
			return ids;
		};

		// ---- a door that locks while the character walks to it refuses on arrival ----
		{
			std::uint64_t farId = 0;
			for (const std::uint64_t id : otherDoors())
			{
				if (rig.door(id)->state == 1)
				{
					const std::uint32_t t = rig.interact(self, id, 1);
					if (rig.replies[t].applied)
					{
						farId = id;
						break;
					}
				}
			}
			test::require(farId != 0, "no second door to walk to");
			flecs::entity e;
			rig.sim->world().ecs().each([&](flecs::entity entity, const state::Id& i, const state::Door&) { if (i.value == farId) e = entity; });
			const_cast<state::Door*>(e.get<state::Door>())->state = 2;
			const std::size_t from = rig.done.size();
			for (ticks = 0; !rig.doneFor(farId, from) && ticks < 6000; ++ticks)
				rig.tick();
			const auto refused = rig.doneFor(farId, from);
			test::require(refused && refused->result == InteractEvent::kRefused, "arrival at a door that locked meanwhile did not report 'refused'");
			test::require(rig.door(farId)->state == 2, "a locked door changed");
			const_cast<state::Door*>(e.get<state::Door>())->state = 1;
		}

		// ---- an interrupted order does nothing ----
		{
			std::uint64_t farDoor = 0;
			rig.run(2); // the previous block unlocked a door under the player: let the paint catch up before routing
			// The stage needs a walk: a closed door within reach is acted on at once and starts none. Step back 60 u from a
			// closed door that routes back to it (nearest first), and drop the player's own route so the walk the stage checks is
			// the one the order starts. No distance filter: the stages before this one leave the player wherever they end, and
			// the world's closed doors are few and far apart, so a few units of sim change can leave none within 40 u.
			for (const std::uint64_t id : otherDoors())
			{
				const state::Door* door = rig.door(id);
				if (door->state != 1)
					continue;
				bool stepped = false;
				for (int k = 0; k < 12 && !stepped; ++k)
				{
					const double a = k * 0.5235988;
					const auto spot = nav->walker().snap({door->x + std::sin(a) * 60.0, door->y, door->z + std::cos(a) * 60.0}, 80.0);
					if (!spot)
						continue;
					const Route back = nav->routeFrom({spot->x, spot->y, spot->z}, door->x, door->z, std::nullopt, true, rig.sim->pathRadius(rig.player()));
					if (!back.found || back.approach)
						continue;
					state::Fighter& player = rig.mutableFighter(self);
					player.x = spot->x;
					player.y = spot->y;
					player.z = spot->z;
					player.clearRoute();
					rig.run(2);
					stepped = true;
				}
				if (stepped)
					break;
			}
			for (const std::uint64_t id : otherDoors())
			{
				if (rig.door(id)->state == 1)
				{
					const std::uint32_t t = rig.interact(self, id, 1);
					rig.run(3);
					if (rig.replies[t].applied && rig.player().walking())
					{
						farDoor = id;
						break;
					}
				}
			}
			test::require(farDoor != 0, "no door to interrupt a walk to");
			const std::size_t from = rig.done.size();
			rig.run(5);
			rig.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", self); w.u8("hold", 0); });
			rig.run(1500);
			// The interrupted order acts on nothing and says so once: event 13 result 5 (cancelled; before the order queue it sent none).
			const auto interrupted = rig.doneFor(farDoor, from);
			test::require(interrupted && interrupted->result == InteractEvent::kCancelled && rig.door(farDoor)->state == 1
				&& std::count_if(rig.done.begin() + static_cast<long>(from), rig.done.end(), [&](const Rig::Done& d) { return d.target == farDoor; }) == 1,"an interrupted interact order still acted, or did not report 'cancelled' once");
		}

		// ---- loot a downed character: its items move to the looter, the body is stripped; first aid stays a stub ----
		{
			// The body also carries two Bread (the servant carries nothing in the data).
			flecs::entity victim;
			rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == rig.duel.enemy) victim = e; });
			state::Inventory loot = *victim.get<state::Inventory>();
			loot.add(breadRecord, 2);
			// ... and a Katana of another quality than the one the looter picked up earlier (the stacks must not merge).
			const state::ItemQuality bodyKatana = data->rollQuality(*gd, *gd->find(katanaRecord), 0.05, 0.9);
			loot.add(katanaRecord, 1, state::Inventory::kCarried, bodyKatana);
			victim.set<state::Inventory>(loot);
			// The looter's pack is the 8 x 10 grid: its two 7 x 1 weapons and three Bread leave no 4 x 6 hole for the body's armour,
			// so the loot would (correctly) be partial. This case is about what moves, so the pack gets room (ksim_inventory_grid
			// tests the partial loot).
			{
				flecs::entity looter;
				rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == self) looter = e; });
				state::Inventory roomy = *looter.get<state::Inventory>();
				roomy.gridH = 30;
				looter.set<state::Inventory>(std::move(roomy));
			}
			const state::Inventory mine = *rig.inventoryOf(self);
			const std::size_t worn = static_cast<std::size_t>(std::count_if(loot.items.begin(), loot.items.end(),
				[](const state::Inventory::Entry& e) { return e.slot != state::Inventory::kCarried; }));
			state::Fighter& body = rig.mutableFighter(rig.duel.enemy);
			body.unconscious = true;
			body.koTimer = 1e6;
			body.hold = true;
			body.parts.front().health = body.parts.front().maxHealth - 10.0;
			const std::size_t from = rig.done.size();
			rig.expectApplied(rig.interact(self, rig.duel.enemy, 5));
			for (ticks = 0; !rig.doneFor(rig.duel.enemy, from) && ticks < 2000; ++ticks)
				rig.tick();
			const auto looted = rig.doneFor(rig.duel.enemy, from);
			test::require(looted && looted->action == 5 && looted->result == 0, "loot did not complete");
			rig.run(2);
			const state::Inventory* after = rig.inventoryOf(self);
			test::require(after->carried(breadRecord) == mine.carried(breadRecord) + 2, "the looted Bread did not arrive");
			for (const state::Inventory::Entry& e : loot.items)
			{
				test::require(after->carried(e.itemSid) >= mine.carried(e.itemSid) + e.count, "a looted item did not arrive: " + e.itemSid);
				// ... each as an instance of the same quality (worn pieces included).
				bool same = false;
				for (const state::Inventory::Entry& got : after->items)
					same = same || (got.slot == state::Inventory::kCarried && got.itemSid == e.itemSid && got.quality == e.quality);
				test::require(same, "a looted item lost its quality: " + e.itemSid);
			}
			{
				std::size_t katanas = 0;
				for (const state::Inventory::Entry& got : after->items)
					katanas += got.slot == state::Inventory::kCarried && got.itemSid == katanaRecord;
				test::require(katanas == 2 && after->carried(katanaRecord) == 2, "the looted Katana merged with the one of another quality");
			}
			test::require(rig.inventoryOf(rig.duel.enemy)->items.empty(), "the body still holds items");
			test::require(rig.enemy().armour.empty() && rig.enemy().weapon.weaponType == 5, "the looted body is still armed or armoured");
			test::require(rig.inventories[self].version == after->version, "the looter's client inventory is stale");
			std::cout << "looted " << loot.items.size() << " stacks (" << worn << " worn)\n";
			rig.expectApplied(rig.interact(self, rig.duel.enemy, 10));
			const std::size_t from2 = rig.done.size();
			rig.run(3);
			const auto aided = rig.doneFor(rig.duel.enemy, from2);
			test::require(aided && aided->action == 10 && aided->result == 0, "first aid did not complete");
		}

		// ---- drop: carried items go to the ground as an INTERACTABLE; order 61 is validated ----
		{
			rig.expectReject(rig.drop(self, "no-such-item", 1), "not_allowed");
			rig.expectReject(rig.drop(self, breadRecord, 0), "bad_value");
			rig.expectReject(rig.drop(self, breadRecord, 100), "not_allowed");
			rig.expectReject(rig.drop(rig.duel.enemy, breadRecord, 1), "not_player_controlled");
			const std::uint32_t before = rig.inventoryOf(self)->carried(breadRecord);
			const std::uint32_t versionBefore = rig.inventoryOf(self)->version;
			rig.inters.clear();
			rig.dropped.clear();
			rig.expectApplied(rig.drop(self, breadRecord, 2));
			rig.run(2);
			test::require(rig.inventoryOf(self)->carried(breadRecord) == before - 2 && rig.inventoryOf(self)->version == versionBefore + 1, "the drop did not leave the inventory");
			test::require(rig.dropped.size() == 1 && rig.dropped[0].who == self && rig.dropped[0].item == breadRecord, "event 6 is wrong");
			std::uint64_t ground = 0;
			for (const auto& [id, r] : rig.inters)
			{
				if (r.kind == ks::kInteractableItem && r.record == breadRecord && r.count == 2)
					ground = id;
			}
			test::require(ground != 0, "the dropped Bread is not an INTERACTABLE");
			test::require(rig.inventories[self].version == rig.inventoryOf(self)->version, "the client's inventory is stale after the drop");
			// Pick it up again: back in the pack.
			rig.expectApplied(rig.interact(self, ground, 7));
			for (ticks = 0; rig.exists(ground) && ticks < 600; ++ticks)
				rig.tick();
			rig.run(2);
			test::require(rig.inventoryOf(self)->carried(breadRecord) == before, "the dropped Bread did not come back");
		}

		// ---- MQ13, the near case: a standing actor reaches a ground item from 42 units, so the scenario's Katana (17 away) is taken at once ----
		{
			Rig n(gd, data, nav, order, true);
			std::uint64_t kat = 0;
			for (const auto& [id, i] : n.inters)
			{
				if (i.kind == ks::kInteractableItem && i.count == 1)
					kat = id;
			}
			test::require(kat != 0, "the squad scenario's Katana is missing");
			const state::Fighter& me = n.player();
			const InteractTarget thing = findInteractTarget(const_cast<flecs::world&>(n.sim->world().ecs()), n.sim->combat(), kat);
			const double away = thing.distanceTo(me.x, me.z);
			test::require(away > kReachGroundItem * 0.5 && away < 2.0 * kReachGroundItem, "the Katana is not 17-22 units from the player: " + std::to_string(away));
			n.expectApplied(n.interact(n.duel.player, kat, 7, 0));
			n.run(3);
			test::require(n.resultsOf(n.duel.player, kat) == std::vector<std::uint8_t>{InteractEvent::kDone} && !n.player().walking(),
				"a standing actor did not pick the near Katana up at once");
			test::require(thing.reachFor(n.player()) == 2.0 * kReachGroundItem, "a standing actor's ground item reach is 42");
			std::cout << "reach: a standing actor takes an item 17 units away at once\n";
		}

		// ---- the order queue (SIMLINK.md section 13), kenshi-0f's live case: two selected characters, pick up the Katana
		// (queue 0), then the Bread (queue 1) ----
		{
			const auto itemsOf = [](Rig& r, std::uint64_t& kat, std::uint64_t& bre)
			{
				for (const auto& [id, i] : r.inters)
				{
					if (i.kind == ks::kInteractableItem && i.count == 1)
						kat = id;
					if (i.kind == ks::kInteractableItem && i.count == 3)
						bre = id;
				}
				test::require(kat != 0 && bre != 0, "the squad scenario's items are missing");
			};
			Rig q(gd, data, nav, order, true);
			q.farItems();
			const std::vector<std::uint64_t> both{q.duel.player, q.second};
			std::uint64_t kat = 0;
			std::uint64_t bre = 0;
			itemsOf(q, kat, bre);
			q.expectApplied(q.interactGroup(both, kat, 7, 0));
			q.expectApplied(q.interactGroup(both, bre, 7, 1));
			q.run(2);
			for (const std::uint64_t id : both)
				test::require(q.queued(id) == 1, "the Bread order did not wait in the queue");
			const auto finished = [&]
			{
				for (const std::uint64_t id : both)
				{
					if (q.resultsOf(id, kat).empty() || q.resultsOf(id, bre).empty())
						return false;
				}
				return true;
			};
			int waited = 0;
			while (!finished() && waited++ < 6000)
				q.tick();
			test::require(finished(), "the two characters did not both finish the Katana and the Bread orders");
			q.run(5);
			int tookKatana = 0;
			int tookBread = 0;
			for (const std::uint64_t id : both)
			{
				// Exactly one event 13 per order and character: done, or the item was gone (the other got there first).
				const auto k = q.resultsOf(id, kat);
				const auto b = q.resultsOf(id, bre);
				test::require(k.size() == 1 && b.size() == 1, "an interact order did not get exactly one event 13");
				test::require(k[0] == InteractEvent::kDone || k[0] == InteractEvent::kGone, "the Katana order ended with " + std::to_string(k[0]));
				test::require(b[0] == InteractEvent::kDone || b[0] == InteractEvent::kGone, "the Bread order ended with " + std::to_string(b[0]));
				tookKatana += k[0] == InteractEvent::kDone;
				tookBread += b[0] == InteractEvent::kDone;
				// In order: the Katana's event comes before the Bread's.
				std::size_t katIndex = 0;
				std::size_t breIndex = 0;
				for (std::size_t i = 0; i < q.done.size(); ++i)
				{
					if (q.done[i].who == id && q.done[i].target == kat)
						katIndex = i;
					if (q.done[i].who == id && q.done[i].target == bre)
						breIndex = i;
				}
				test::require(katIndex < breIndex, "the queued Bread order finished before the Katana order");
				test::require(q.queued(id) == 0,"the queue was not emptied");
			}
			test::require(tookKatana == 1 && tookBread == 1, "each item must be taken by exactly one of the two characters");
			std::cout << "queue: Katana then Bread for two characters, " << tookKatana << " + " << tookBread << " taken, the others got target_gone\n";
		}
		// A queue that is not a queue: an ordinary order cancels the running interact and every queued one, once each.
		{
			Rig c(gd, data, nav, order, true);
			c.farItems();
			std::uint64_t kat = 0;
			std::uint64_t bre = 0;
			for (const auto& [id, i] : c.inters)
			{
				if (i.kind == ks::kInteractableItem && i.count == 1)
					kat = id;
				if (i.kind == ks::kInteractableItem && i.count == 3)
					bre = id;
			}
			const std::uint64_t who = c.duel.player;
			c.expectApplied(c.interact(who, kat, 7, 0));
			c.expectApplied(c.interact(who, bre, 7, 1));
			c.expectApplied(c.moveToFlags({who}, c.player().x - 40.0, c.player().z - 40.0, 1)); // a queued move too
			c.run(3);
			test::require(c.queued(who) == 2, "the queue should hold the Bread and the move");
			c.expectApplied(c.moveTo(who, c.player().x - 30.0, c.player().z));
			c.run(2);
			test::require(c.queued(who) == 0, "an ordinary order did not clear the queue");
			test::require(c.resultsOf(who, kat) == std::vector<std::uint8_t>{InteractEvent::kCancelled}, "the running interact did not get 'cancelled'");
			test::require(c.resultsOf(who, bre) == std::vector<std::uint8_t>{InteractEvent::kCancelled}, "the queued interact did not get 'cancelled'");
			c.run(1500);
			test::require(c.done.size() == 2, "a cancelled order sent another event 13");
			test::require(c.exists(kat) && c.exists(bre), "a cancelled pick-up still took the item");
			// KO'd with orders queued: the running one reports 4, the queued one 5.
			Rig d(gd, data, nav, order, true);
			d.farItems();
			std::uint64_t k2 = 0;
			std::uint64_t b2 = 0;
			for (const auto& [id, i] : d.inters)
			{
				if (i.kind == ks::kInteractableItem && i.count == 1)
					k2 = id;
				if (i.kind == ks::kInteractableItem && i.count == 3)
					b2 = id;
			}
			d.expectApplied(d.interact(d.duel.player, k2, 7, 0));
			d.expectApplied(d.interact(d.duel.player, b2, 7, 1));
			d.run(3);
			d.mutableFighter(d.duel.player).unconscious = true;
			d.mutableFighter(d.duel.player).koTimer = 1e6;
			d.run(3);
			test::require(d.resultsOf(d.duel.player, k2) == std::vector<std::uint8_t>{InteractEvent::kIncapacitated}
				&& d.resultsOf(d.duel.player, b2) == std::vector<std::uint8_t>{InteractEvent::kCancelled} && d.queued(d.duel.player) == 0,
				"a character knocked out with a queue did not report 4 and 5");
			std::cout << "queue: an ordinary order cancels the running and the queued interact once each; a KO reports 4 and 5\n";
		}
		// A queued move runs after the pick-up, and a queue survives a save.
		{
			Rig m(gd, data, nav, order, true);
			m.farItems();
			std::uint64_t kat = 0;
			for (const auto& [id, i] : m.inters)
			{
				if (i.kind == ks::kInteractableItem && i.count == 1)
					kat = id;
			}
			const std::uint64_t who = m.duel.player;
			const double goalX = m.player().x - 25.0;
			const double goalZ = m.player().z + 5.0;
			m.expectApplied(m.interact(who, kat, 7, 0));
			m.expectApplied(m.moveToFlags({who}, goalX, goalZ, 1));
			m.run(2);
			test::require(m.queued(who) == 1, "the move did not wait for the pick-up");
			int waited = 0;
			while ((m.queued(who) != 0 || m.player().walking() || m.resultsOf(who, kat).empty()) && waited++ < 6000)
				m.tick();
			test::require(m.resultsOf(who, kat) == std::vector<std::uint8_t>{InteractEvent::kDone}, "the pick-up before the queued move failed");
			test::require(std::hypot(m.player().x - goalX, m.player().z - goalZ) < 6.0, "the queued move did not walk to its goal: "
				+ std::to_string(std::hypot(m.player().x - goalX, m.player().z - goalZ)) + " units short");

			const auto script = [&](Rig& r)
			{
				std::uint64_t k = 0;
				std::uint64_t b = 0;
				for (const auto& [id, i] : r.inters)
				{
					if (i.kind == ks::kInteractableItem && i.count == 1)
						k = id;
					if (i.kind == ks::kInteractableItem && i.count == 3)
						b = id;
				}
				r.interactGroup({r.duel.player, r.second}, k, 7, 0);
				r.interactGroup({r.duel.player, r.second}, b, 7, 1);
			};
			Rig whole(gd, data, nav, order, true);
			whole.farItems();
			script(whole);
			whole.run(160);
			Rig half(gd, data, nav, order, true);
			half.farItems();
			script(half);
			half.run(3);
			test::require(half.queued(half.duel.player) == 1, "no queue to save");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_interact_queue.save";
			half.sim->saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			CombatSettings settings;
			settings.substeps = 2;
			loaded.enableCombat(data, settings);
			loaded.enableNavigation(nav);
			loaded.run(157);
			test::require(loaded.stateHash() == whole.sim->stateHash(), "save with a queued order + load + run differs from the uninterrupted run");
			std::remove(path.c_str());
			std::cout << "queue: a queued move runs after the pick-up; save/load mid-queue continues identically\n";
		}

		// ---- doors as the original does them (DOORS.md section 6) ----
		{
			Rig d(gd, data, nav, order, true);
			d.sim->setGameData(gd); // the door steps read faction relations
			flecs::world& ecs = const_cast<flecs::world&>(d.sim->world().ecs());
			const std::uint64_t walkerId = d.second;
			d.mutableFighter(walkerId).playerControlled = false; // the planner decides for it: it may unlock or bash
			// The walker holds still while the scene is set up; the squad's bandits are 700 units away.
			const auto entityOf = [&](std::uint64_t id)
			{
				flecs::entity found;
				ecs.each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
				return found;
			};
			const auto doorEntity = [&](std::uint64_t id)
			{
				flecs::entity found;
				ecs.each([&](flecs::entity e, const state::Id& i, const state::Door&) { if (i.value == id) found = e; });
				return found;
			};
			const auto setAllegiance = [&](const std::string& sid) { entityOf(walkerId).set<state::Allegiance>(state::Allegiance{sid}); };
			const auto minDistance = [](const Route& route, double x, double z)
			{
				double best = 1e9;
				for (std::size_t i = 1; i < route.points.size(); ++i)
				{
					const double dx = route.points[i].x - route.points[i - 1].x, dz = route.points[i].z - route.points[i - 1].z;
					const double len2 = dx * dx + dz * dz;
					const double t = len2 > 0 ? std::clamp(((x - route.points[i - 1].x) * dx + (z - route.points[i - 1].z) * dz) / len2, 0.0, 1.0) : 0.0;
					best = std::min(best, std::hypot(x - route.points[i - 1].x - t * dx, z - route.points[i - 1].z - t * dz));
				}
				return best;
			};
			// A door and a goal such that the planned route from the walker runs through the door.
			// (The walker stands where the first character does: the searches of the doors-and-pathing block start there.)
			d.mutableFighter(walkerId).x = d.player().x;
			d.mutableFighter(walkerId).y = d.player().y;
			d.mutableFighter(walkerId).z = d.player().z;
			const state::Fighter& w0 = *d.sim->combat()->find(walkerId);
			const knav::Position from{w0.x, w0.y, w0.z};
			std::uint64_t doorId = 0;
			double goalX = 0, goalZ = 0;
			Route planned;
			// This rig's doors, nearest the walker first (the snapshot's detail bounds hold one door since the Hub's ruins lost theirs).
			std::vector<std::pair<double, std::uint64_t>> candidates;
			ecs.each([&](const state::Id& i, const state::Door& cd) { candidates.push_back({std::hypot(cd.x - from.x, cd.z - from.z), i.value}); });
			std::sort(candidates.begin(), candidates.end());
			for (const auto& [far, candId] : candidates)
			{
				if (doorId != 0 || far > 400.0)
					break;
				const state::Door* cd = d.door(candId);
				for (const double r : {25.0, 45.0, 70.0})
				{
					for (int k = 0; k < 12 && doorId == 0; ++k)
					{
						const double a = k * 0.5235988;
						const double gx = cd->x + std::sin(a) * r, gz = cd->z + std::cos(a) * r;
						const Route route = nav->routeFrom(from, gx, gz, std::nullopt, true);
						if (route.found && !route.approach && minDistance(route, cd->x, cd->z) < kDoorPassReach - 2.0 && std::hypot(from.x - cd->x, from.z - cd->z) > 25.0)
						{
							doorId = candId;
							goalX = gx;
							goalZ = gz;
							planned = route;
						}
					}
				}
			}
			test::require(doorId != 0, "no door lies on a planned route from the walker");
			flecs::entity doorE = doorEntity(doorId);
			state::Door* door = const_cast<state::Door*>(doorE.get<state::Door>());
			const double maxHp = door->maxHealth;
			const std::string walkerFaction = "walker-faction";
			// Runs the walker along `planned` (a command, as an AI plan) and reports what happened at the door.
			struct Trace
			{
				int stepStart = -1, unlocked = -1, opened = -1, broken = -1, arrived = -1;
				std::uint32_t versionAtStart = 0;
			};
			const auto walk = [&](Rig& r, state::Door* dr, int limit)
			{
				Trace trace;
				trace.versionAtStart = dr->version;
				std::vector<state::GroundPoint> points = planned.points;
				r.sim->enqueue(Command::fighterMoveTo(state::Id{walkerId}, planned.points, planned.heights, 900, false, planned.approach));
				int prev = dr->state;
				for (int t = 0; t < limit; ++t)
				{
					r.tick();
					if (std::getenv("KDOOR_TRACE") != nullptr && t % 10 == 0 && t < 600)
						std::fprintf(stderr, "t %d walker %.1f %.1f dist %.1f walking %d next %u/%zu state %d\n", t, r.sim->combat()->find(walkerId)->x, r.sim->combat()->find(walkerId)->z,
							std::hypot(r.sim->combat()->find(walkerId)->x - dr->x, r.sim->combat()->find(walkerId)->z - dr->z), (int) r.sim->combat()->find(walkerId)->walking(),
							r.sim->combat()->find(walkerId)->routeNext, r.sim->combat()->find(walkerId)->route.size(), dr->state);
					if (trace.stepStart < 0 && entityOf(walkerId).has<state::DoorStep>())
						trace.stepStart = t;
					if (dr->state != prev)
					{
						if (prev == 2 && dr->state == 1)
							trace.unlocked = t;
						if (dr->state == 0)
							trace.opened = t;
						if (dr->state == 3)
							trace.broken = t;
						prev = dr->state;
					}
					if (trace.opened >= 0 || trace.broken >= 0)
					{
						if (!r.sim->combat()->find(walkerId)->walking() && !entityOf(walkerId).has<state::DoorStep>()
							&& std::hypot(r.sim->combat()->find(walkerId)->x - goalX, r.sim->combat()->find(walkerId)->z - goalZ) < 6.0)
						{
							trace.arrived = t;
							break;
						}
					}
				}
				return trace;
			};
			const auto unlockTicks = static_cast<int>(std::round(kUnlockSeconds / kTickSeconds));
			const auto openTicks = static_cast<int>(std::ceil(kDoorToggleOpenness / std::max(door->moveSpeed, 0.1) / kTickSeconds));

			// (1) The owner: unlock about 3.3 s, then open, then on to the goal.
			door->state = 2;
			door->ownerSid = walkerFaction;
			++door->version;
			setAllegiance(walkerFaction);
			d.run(2);
			{
				const Trace t = walk(d, door, 4000);
				test::require(t.stepStart >= 0 && t.unlocked >= 0 && t.opened >= 0 && t.arrived >= 0, "the owner did not unlock, open and walk through: step " + std::to_string(t.stepStart) + " unlocked " + std::to_string(t.unlocked) + " opened " + std::to_string(t.opened) + " arrived " + std::to_string(t.arrived) + " door state " + std::to_string(door->state) + " walker " + std::to_string(std::hypot(d.sim->combat()->find(walkerId)->x - door->x, d.sim->combat()->find(walkerId)->z - door->z)) + " from door, walking " + std::to_string(d.sim->combat()->find(walkerId)->walking()));
				test::require(std::abs((t.unlocked - t.stepStart) - unlockTicks) <= 2, "the unlock took " + std::to_string((t.unlocked - t.stepStart) * kTickSeconds) + " s, not about 3.3");
				test::require(std::abs((t.opened - t.unlocked) - openTicks) <= 2, "the open after the unlock took " + std::to_string((t.opened - t.unlocked) * kTickSeconds) + " s");
				std::cout << "doors: owner unlock " << (t.unlocked - t.stepStart) * kTickSeconds << " s, open " << (t.opened - t.unlocked) * kTickSeconds
					<< " s (door move speed " << door->moveSpeed << "), then walked on\n";
			}
			// (2) A closed unlocked door: anyone opens it on the way, in the open time.
			{
				state::Fighter& w = d.mutableFighter(walkerId);
				w.x = from.x; w.y = from.y; w.z = from.z;
				door->state = 1;
				++door->version;
				setAllegiance("");
				d.run(2);
				const Trace t = walk(d, door, 4000);
				test::require(t.stepStart >= 0 && t.unlocked < 0 && t.opened >= 0 && t.arrived >= 0, "a stranger did not open the closed door on its way");
				test::require(std::abs((t.opened - t.stepStart) - openTicks) <= 2, "the plain open took " + std::to_string((t.opened - t.stepStart) * kTickSeconds) + " s");
			}
			// (3) A non-owner that is not hostile waits at a locked door; nobody gets a route through.
			{
				state::Fighter& w = d.mutableFighter(walkerId);
				w.x = from.x; w.y = from.y; w.z = from.z;
				door->state = 2;
				door->ownerSid = "someone-else";
				door->lockLevel = 90; // level - Lockpicking >= 41.7: the pick is out as well, so the planner waits
				++door->version;
				setAllegiance(walkerFaction);
				d.run(2);
				const Trace t = walk(d, door, 700); // the door is ~25 s away; then ~10 s of waiting (the cap is 60 s)
				test::require(t.stepStart >= 0 && door->state == 2 && t.unlocked < 0 && t.broken < 0, "a non-owner got through a locked door: step " + std::to_string(t.stepStart) + " state " + std::to_string(door->state));
				const state::DoorStep* step = entityOf(walkerId).get<state::DoorStep>();
				test::require(step != nullptr && step->phase == 4, "a non-owner that may not bash does not wait at the locked door");
				// The player is refused a route outright, owner or not.
				d.mutableFighter(walkerId).playerControlled = true;
				d.mutableFighter(walkerId).x = from.x; d.mutableFighter(walkerId).y = from.y; d.mutableFighter(walkerId).z = from.z;
				entityOf(walkerId).remove<state::DoorStep>();
				d.run(2);
				const Route plain = nav->routeFrom(from, goalX, goalZ);
				test::require(!plain.found || minDistance(plain, door->x, door->z) >= 2.0, "a player's route runs through a locked door");
				d.mutableFighter(walkerId).playerControlled = false;
			}
			// (4) A hostile non-owner bashes: hp / (0.015 clamp(2 blunt + 0.5 cut, 20, 60)) hits, one per 2.7 s, then the door is broken.
			{
				// The Rig's world has no faction instances: make two from the data. The owner is a FACTION record whose default relation
				// is at or below the hostile threshold (-30), the basher's faction is the player's.
				std::string a;
				std::string b;
				ecs.each([&](const state::Id& i, const state::Allegiance& al) { if (i.value == d.duel.player) a = al.factionSid; });
				const std::int32_t factionType = gd->find(a)->type;
				for (const kdata::Record& record : gd->records())
				{
					if (b.empty() && record.type == factionType && record.intOr("default relation", 0) <= -30)
						b = record.stringID;
				}
				test::require(!b.empty(), "no FACTION record with a hostile default relation");
				ecs.entity().set<state::FactionState>(state::FactionState{a});
				ecs.entity().set<state::FactionState>(state::FactionState{b});
				{
					flecs::entity owner;
					flecs::entity other;
					ecs.each([&](flecs::entity e, const state::FactionState& f) { if (f.templateSid == b) owner = e; if (f.templateSid == a) other = e; });
					test::require(factionRelation(*d.sim->gameData(), owner, other) <= -30.0, "the bandits' faction is not hostile to the player's by default: "
						+ std::to_string(factionRelation(*d.sim->gameData(), owner, other)));
				}
				state::Fighter& w = d.mutableFighter(walkerId);
				w.x = from.x; w.y = from.y; w.z = from.z;
				entityOf(walkerId).remove<state::DoorStep>();
				door->state = 2;
				door->health = maxHp;
				door->ownerSid = b;
				++door->version;
				setAllegiance(a);
				// Empty hands and a Katana in the pack: BASH_DOOR needs a weapon in hand, so the planner adds an equip step first.
				{
					state::Inventory pack = *entityOf(walkerId).get<state::Inventory>();
					pack.items.erase(std::remove_if(pack.items.begin(), pack.items.end(), [](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kWeapon; }),
						pack.items.end());
					pack.add(katanaRecord, 1, state::Inventory::kCarried, katanaQuality);
					entityOf(walkerId).set<state::Inventory>(std::move(pack));
					w.weapon = data->unarmed;
				}
				d.run(2);
				const state::FighterWeapon katanaInHand = data->buildWeapon(*gd, *gd->find(katanaRecord), katanaQuality);
				const double hit = bashDamage(katanaInHand.blunt1, katanaInHand.cut1);
				const int hits = static_cast<int>(std::ceil(maxHp / hit - 1e-9));
				const Trace t = walk(d, door, static_cast<int>((hits + 4) * kBashInterval / kTickSeconds) + 1200);
				test::require(t.broken >= 0 && door->state == 3, "the bashers did not break the door");
				// Every hit bumps the version; the break is the last one.
				const std::uint32_t hitCount = door->version - t.versionAtStart;
				test::require(static_cast<int>(hitCount) == hits, "the door broke after " + std::to_string(hitCount) + " hits, expected " + std::to_string(hits));
				const double seconds = (t.broken - t.stepStart) * kTickSeconds;
				test::require(std::abs(seconds - hits * kBashInterval) <= 0.2, "bashing took " + std::to_string(seconds) + " s, expected " + std::to_string(hits * kBashInterval));
				test::require(t.arrived >= 0, "the bashers did not walk on through the broken door");
				test::require(w.weapon.sid == katanaRecord && entityOf(walkerId).get<state::Inventory>()->carried(katanaRecord) == 0,
					"the planner's equip step did not put the Katana in hand");
				std::cout << "doors: bash " << hits << " hits of " << hit << " hp (door hp " << maxHp << ") broke it in " << seconds << " s\n";
				test::require(door->health == 0.0, "a broken door keeps hp");
				// Broken is open for ever: it cannot be closed or unlocked, and it stays passable.
				d.mutableFighter(d.duel.player).x = door->x;
				d.mutableFighter(d.duel.player).z = door->z;
				d.expectReject(d.interact(d.duel.player, doorId, 2), "not_allowed");
				d.expectReject(d.interact(d.duel.player, doorId, 11), "not_allowed");
			}
			// (4b) The faction kinds (FACTION `fundamental type`): a non-hostile owner's door is bashed only by a law, military,
			// slave, slaver or bandit owner's faction; a trader's is waited at. The walker holds a Katana (so the pick is out) and the
			// lock is out of reach (90) as well.
			{
				const std::int32_t factionType = kworld::itemTypeByName("FACTION");
				const auto factionNamed = [&](const char* name)
				{
					for (const kdata::Record& record : gd->records())
					{
						if (record.type == factionType && record.name == name)
							return record.stringID;
					}
					throw std::runtime_error(std::string("no FACTION ") + name);
				};
				const std::string military = factionNamed("Mercenary Guild"); // type 2
				const std::string trader = factionNamed("Trade Ninjas"); // type 3
				test::require(gd->find(military)->intOr("fundamental type", 4) == 2 && gd->find(trader)->intOr("fundamental type", 4) == 3,
					"the faction kinds of the Mercenary Guild / Trade Ninjas are not 2 / 3 in the data");
				const auto reset = [&](const std::string& owner)
				{
					state::Fighter& w = d.mutableFighter(walkerId);
					w.x = from.x; w.y = from.y; w.z = from.z;
					entityOf(walkerId).remove<state::DoorStep>();
					door->state = 2;
					door->health = maxHp;
					door->lockLevel = 90;
					door->ownerSid = owner;
					++door->version;
					setAllegiance("walker-faction");
					state::Inventory pack = *entityOf(walkerId).get<state::Inventory>();
					pack.items.erase(std::remove_if(pack.items.begin(), pack.items.end(), [&](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kWeapon || e.itemSid == katanaRecord; }),
						pack.items.end());
					pack.add(katanaRecord, 1, state::Inventory::kWeapon, katanaQuality);
					entityOf(walkerId).set<state::Inventory>(std::move(pack));
					w.weapon = data->buildWeapon(*gd, *gd->find(katanaRecord), katanaQuality);
					d.run(2);
				};
				reset(military);
				const Trace bashed = walk(d, door, 4600);
				test::require(bashed.broken >= 0, "a military owner's door was not bashed by a stranger holding a Katana");
				reset(trader);
				const Trace waited = walk(d, door, 800);
				const state::DoorStep* step = entityOf(walkerId).get<state::DoorStep>();
				test::require(waited.broken < 0 && step != nullptr && step->phase == 4, "a trader owner's door was bashed (kind 3 scores 0)");
				std::cout << "doors: bash for a military owner, wait for a trader owner (faction kinds)\n";
			}
			// (4c) Empty hands, nothing carried, a lock within reach: the AI picks it (PICK_LOCK 0.1), then opens the door and walks on.
			{
				state::Fighter& w = d.mutableFighter(walkerId);
				w.x = from.x; w.y = from.y; w.z = from.z;
				w.weapon = data->unarmed;
				w.skills.lockpicking = 1.0;
				entityOf(walkerId).remove<state::DoorStep>();
				state::Inventory pack = *entityOf(walkerId).get<state::Inventory>();
				pack.items.erase(std::remove_if(pack.items.begin(), pack.items.end(), [&](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kWeapon || e.itemSid == katanaRecord; }),
					pack.items.end());
				entityOf(walkerId).set<state::Inventory>(std::move(pack));
				door->state = 2;
				door->lockLevel = 22; // the Vagrants thug's lock
				door->ownerSid = "someone-else";
				++door->version;
				d.run(2);
				bool picking = false;
				const Trace t = [&]
				{
					Trace trace = walk(d, door, 4000);
					return trace;
				}();
				(void) picking;
				test::require(t.stepStart >= 0 && t.unlocked >= 0 && t.opened >= 0 && t.arrived >= 0, "the empty-handed stranger did not pick the lock, open the door and walk through");
				const double seconds = (t.unlocked - t.stepStart) * kTickSeconds;
				// The first attempt comes after 5 s and each failure re-rolls 5..20 s (item 19): at chance 0.21 the wait is
				// 5 s at the least, about 55 s on average, and the walk's 4000 ticks (200 s) bound the test.
				test::require(seconds >= kPickFirstInterval - 0.1 && seconds < 200.0, "picking took " + std::to_string(seconds) + " s");
				std::cout << "doors: AI picked a level-22 lock (skill 1, chance " << pickChance(22, 1) << ") in " << seconds << " s (first attempt at 5 s, then 5-20 s re-rolls)\n";
			}
			// (5) The player unlocks a door of its own faction (interact 11); a stranger's is not_allowed. Close re-locks an `always locks` door.
			{
				const std::uint64_t pid = d.duel.player;
				std::string sid;
				ecs.each([&](const state::Id& i, const state::Allegiance& al) { if (i.value == pid) sid = al.factionSid; });
				test::require(!sid.empty(), "the player has no allegiance");
				door->state = 2;
				door->health = maxHp;
				door->ownerSid = "someone-else";
				++door->version;
				d.run(2);
				d.expectReject(d.interact(pid, doorId, 11), "not_allowed");
				door->ownerSid = sid;
				const std::size_t from2 = d.done.size();
				d.mutableFighter(pid).x = door->x + 3.0;
				d.mutableFighter(pid).y = door->y;
				d.mutableFighter(pid).z = door->z;
				d.expectApplied(d.interact(pid, doorId, 11));
				int ticks = 0;
				while (!d.doneFor(doorId, from2) && ticks++ < 400)
					d.tick();
				const auto unlocked = d.doneFor(doorId, from2);
				test::require(unlocked && unlocked->action == 11 && unlocked->result == 0 && door->state == 1, "the owner's unlock order did not unlock the door");
				test::require(std::abs(ticks * kTickSeconds - kUnlockSeconds) < 0.4, "the unlock order took " + std::to_string(ticks * kTickSeconds) + " s");
				// Open, then close an `always locks` door: closing locks it again.
				door->alwaysLocks = true;
				d.expectApplied(d.interact(pid, doorId, 1));
				d.run(80);
				test::require(door->state == 0, "the unlocked door did not open");
				d.expectApplied(d.interact(pid, doorId, 2));
				d.run(80);
				test::require(door->state == 2, "closing an `always locks` door did not lock it");
				door->alwaysLocks = false;
				// (6) LOCK_DOOR (goap row 77): the owner walks to a shut door of its faction and locks it; a stranger's is refused.
				door->state = 1;
				door->ownerSid = "someone-else";
				++door->version;
				d.run(2);
				test::require(!d.sim->orderLockDoor(pid, doorId, DoorSide::Near), "a lock order on a stranger's door was accepted");
				door->ownerSid = sid;
				d.mutableFighter(pid).x = door->x + 60.0;
				d.mutableFighter(pid).z = door->z;
				const std::size_t from3 = d.done.size();
				test::require(d.sim->orderLockDoor(pid, doorId, DoorSide::Near), "the lock order on an own shut door was refused");
				int lockTicks = 0;
				while (!d.doneFor(doorId, from3) && lockTicks++ < 800)
					d.tick();
				test::require(d.doneFor(doorId, from3) && door->state == 2, "the owner did not walk to the door and lock it");
				std::cout << "doors: lock order, walk and lock in " << lockTicks * kTickSeconds << " s\n";
				door->state = 1;
				++door->version;
				std::cout << "doors: unlock order " << ticks * kTickSeconds << " s, close re-locks an always-locks door\n";
			}
		}

		// ---- lockpicking by order (interact action 3) ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			std::uint64_t doorId = 0;
			d.sim->world().ecs().each([&](const state::Id& i, const state::Door&) { if (doorId == 0) doorId = i.value; });
			const state::Door* door = d.door(doorId);
			state::Door* writable = const_cast<state::Door*>(door);
			writable->ownerSid = "someone-else"; // not the player's: no unlock for it
			writable->state = 2;
			writable->lockLevel = 22;
			++writable->version;
			state::Fighter& p = d.mutableFighter(pid);
			p.skills.lockpicking = 1.0;
			p.x = door->x + 3.0;
			p.y = door->y;
			p.z = door->z;
			d.run(2);
			d.expectReject(d.interact(pid, katana, 3), "bad_target");
			d.expectApplied(d.interact(pid, doorId, 3));
			const std::size_t from = d.done.size();
			int ticks = 0;
			const double skill0 = p.skills.lockpicking;
			// The lock's timer is on the door: the first attempt comes after 5 game-s, every failure re-rolls the interval.
			double firstAttempt = -1.0;
			while (!d.doneFor(doorId, from) && ticks++ < 8000)
			{
				d.tick();
				if (firstAttempt < 0.0 && writable->lockAttempts > 0)
					firstAttempt = ticks * kTickSeconds;
			}
			const auto result = d.doneFor(doorId, from);
			test::require(result && result->action == 3 && result->result == 0 && door->state == 1, "the lockpick order did not unlock the door");
			test::require(firstAttempt >= kPickFirstInterval - 0.15 && firstAttempt <= kPickFirstInterval + 0.15,
				"the first attempt came after " + std::to_string(firstAttempt) + " s, expected 5");
			test::require(writable->lockInterval >= kPickIntervalMin && writable->lockInterval < kPickIntervalMax, "the lock's interval left [5, 20)");
			test::require(p.skills.lockpicking > skill0, "an attempt gave no Lockpicking XP");
			std::cout << "lockpick: order, level 22 skill 1 (chance " << pickChance(22, 1) << "): unlocked in " << ticks * kTickSeconds
				<< " s after " << writable->lockAttempts << " attempts, first at " << firstAttempt << " s, skill " << skill0 << " -> " << p.skills.lockpicking << "\n";
			d.expectReject(d.interact(pid, doorId, 3), "not_allowed"); // no longer locked
			// Interrupted and resumed: the elapsed time stays on the lock. Re-lock, pick 3 s, cancel (any new order does), pick again:
			// the first attempt of the second pick comes at 5 s of shared time, not 5 s after it starts.
			writable->state = 2;
			writable->lockLevel = p.skills.lockpicking + 39.0; // chance 6%, above the 5% floor under which the order refuses (checkInteract)
			writable->lockElapsed = 0.0;
			writable->lockInterval = kPickFirstInterval;
			writable->lockAttempts = 0;
			++writable->version;
			d.expectApplied(d.interact(pid, doorId, 3));
			d.run(60); // 3 s
			d.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", pid); w.u8("hold", 0); });
			d.run(2);
			test::require(writable->lockAttempts == 0 && writable->lockElapsed > 2.5 && writable->lockElapsed < 3.5,
				"an interrupted pick lost its elapsed time: " + std::to_string(writable->lockElapsed));
			d.expectApplied(d.interact(pid, doorId, 3));
			d.run(30); // 1.5 s more: 4.6 s in all, no attempt yet
			test::require(writable->lockAttempts == 0, "the resumed pick rolled before the shared 5 s was up");
			d.run(20); // past 5 s: the attempt is made
			test::require(writable->lockAttempts == 1, "the resumed pick did not roll at 5 s of shared time");
			std::cout << "lockpick: an interrupted pick keeps its elapsed time on the lock\n";
		}

		// ---- 290 / 226: the player's Smash order (interact action 12) on a locked foreign gate (dt40_tasks_289_290 section 4) ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			std::uint64_t doorId = 0;
			d.sim->world().ecs().each([&](const state::Id& i, const state::Door&) { if (doorId == 0) doorId = i.value; });
			const state::Door* door = d.door(doorId);
			state::Door* writable = const_cast<state::Door*>(door);
			writable->ownerSid = "someone-else"; // a foreign gate, locked: the menu's Smash row (an own or unlocked one is 226's)
			writable->gate = true;
			writable->state = 2;
			++writable->version;
			state::Fighter& p = d.mutableFighter(pid);
			p.x = door->x + 3.0;
			p.y = door->y;
			p.z = door->z;
			d.run(2);
			d.expectApplied(d.interact(pid, doorId, 12));
			const std::size_t from = d.done.size();
			int ticks = 0;
			while (!d.doneFor(doorId, from) && ticks++ < 8000)
				d.tick();
			const auto result = d.doneFor(doorId, from);
			test::require(result && result->action == 12 && result->result == 0 && door->state == 3, "the Smash order did not break the locked gate");
			// Task_BashDoor::vf2 (140348c10 L61): burglary against the door's faction (the record may have lapsed by the break; the bounty counts too).
			{
				const state::CrimeRecord* crimeRecord = nullptr;
				d.sim->world().ecs().each([&](const state::Id& i, const state::CrimeRecord& held) { if (i.value == pid) crimeRecord = &held; });
				test::require(crimeRecord != nullptr && (crimeRecord->crime == crime::kBurglary || !crimeRecord->bounties.empty()), "bashing a foreign gate recorded no burglary");
			}
			std::cout << "smash: the player's order broke a locked foreign gate in " << ticks * kTickSeconds << " s\n";
		}

		// ---- save / load in the middle of a pick: the lock's timer is saved ----
		{
			const auto setup = [&](Rig& r)
			{
				const std::uint64_t pid = r.duel.player;
				std::uint64_t doorId = 0;
				r.sim->world().ecs().each([&](const state::Id& i, const state::Door&) { if (doorId == 0) doorId = i.value; });
				state::Door* door = const_cast<state::Door*>(r.door(doorId));
				door->ownerSid = "someone-else";
				door->state = 2;
				door->lockLevel = 30; // chance 0.12 at skill 1: failures re-roll the interval
				++door->version;
				state::Fighter& p = r.mutableFighter(pid);
				p.x = door->x + 3.0;
				p.y = door->y;
				p.z = door->z;
				r.run(2);
				r.expectApplied(r.interact(pid, doorId, 3));
				return door;
			};
			Rig whole(gd, data, nav, order, true);
			const state::Door* wholeDoor = setup(whole);
			whole.run(400);
			Rig half(gd, data, nav, order, true);
			const state::Door* halfDoor = setup(half);
			half.run(130); // 6.5 s: past the first attempt (5 s), inside the re-rolled wait
			test::require(halfDoor->lockAttempts >= 1 && halfDoor->state == 2 && halfDoor->lockElapsed > 0.0, "the save point is not mid-pick");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_interact_pick.save";
			half.sim->saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			CombatSettings settings;
			settings.substeps = 2;
			loaded.enableCombat(data, settings);
			loaded.enableNavigation(nav);
			loaded.run(270);
			test::require(loaded.stateHash() == whole.sim->stateHash(), "save mid-pick + load + run differs from the uninterrupted run");
			std::remove(path.c_str());
			std::cout << "lockpick: save / load mid-pick continues identically (" << wholeDoor->lockAttempts << " attempts in 20 s)\n";
		}

		// ---- door records: max HP, move speed; lockpick the Defensive Gate by order (the client's kenshi-0f report) ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			std::map<std::string, std::vector<std::uint64_t>> byName;
			d.sim->world().ecs().each([&](const state::Id& i, const state::Door& dr)
			{
				const kdata::Record* record = gd->find(dr.recordSid);
				test::require(record != nullptr, "a door without a record");
				byName[record->name].push_back(i.value);
				// Max HP is the record's Σ construction, else build materials; open / close take 1 / door move speed.
				test::require(dr.maxHealth == doorMaxHealth(*record) && dr.health == dr.maxHealth, "a door's HP is not its record's");
				test::require(dr.moveSpeed == doorMoveSpeed(*record), "a door's move speed is not its record's");
				test::require(dr.lockLevel >= 1 && dr.lockLevel <= 100, "a lock level outside 1..100");
			});
			// The HP of the records (the Hub's Defensive Gate and storm houses are ruins since 7d6ac14: no door entities; the gate
			// lockpick walk moved to ksim_carry, on Stack).
			const auto hp = [&](const std::string& name)
			{
				const kdata::Record* found = nullptr;
				for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("BUILDING")))
					if (r->name == name)
						found = r;
				test::require(found != nullptr, "no door record " + name);
				return doorMaxHealth(*found);
			};
			test::require(hp("Defensive Gate level 4 Door") == 90.0, "the Defensive Gate's door is not 90 HP");
			test::require(hp("Storm House Door A") == 16.0 && hp("Storm Shack Door") == 12.0, "storm house / shack door HP");
			// Open and close take 1 / door move speed; the state (and the navmesh) flips at 0.6 of that.
			// Two door records of the Hub rig with different speeds (was Storm House Door A and B, ruins now).
			std::vector<std::string> timed;
			for (const auto& [name, ids] : byName)
			{
				const double speed = d.door(ids.front())->moveSpeed;
				if (!d.door(ids.front())->gate && (timed.empty() || speed != d.door(byName.at(timed.front()).front())->moveSpeed) && timed.size() < 2)
					timed.push_back(name);
			}
			test::require(timed.size() == 2, "fewer than two door speeds in the Hub rig");
			for (const std::string& name : timed)
			{
				const std::uint64_t id = byName.at(name).front();
				state::Door* writable = const_cast<state::Door*>(d.door(id));
				const double seconds = 1.0 / writable->moveSpeed;
				state::Fighter& f = d.mutableFighter(pid);
				f.x = writable->x + 3.0;
				f.y = writable->y;
				f.z = writable->z;
				f.clearRoute();
				d.run(2);
				writable->state = 1;
				++writable->version;
				const std::size_t from = d.done.size();
				d.expectApplied(d.interact(pid, id, 1));
				int flipped = -1, ended = -1;
				for (int t = 1; t < 400 && ended < 0; ++t)
				{
					d.tick();
					if (flipped < 0 && writable->state == 0)
						flipped = t;
					if (d.doneFor(id, from))
						ended = t;
				}
				test::require(std::abs(flipped * kTickSeconds - 0.6 * seconds) <= 0.15 && std::abs(ended * kTickSeconds - seconds) <= 0.15,
					name + ": open flipped at " + std::to_string(flipped * kTickSeconds) + " s and ended at " + std::to_string(ended * kTickSeconds) + " s, speed " + std::to_string(writable->moveSpeed));
				std::cout << "door open: " << name << " (speed " << writable->moveSpeed << ") flips at " << flipped * kTickSeconds << " s, done at " << ended * kTickSeconds << " s\n";
			}
		}

		// ---- equip and unequip (order 62) ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			state::Fighter& p = d.mutableFighter(pid);
			const auto entityOf = [&](std::uint64_t id)
			{
				flecs::entity found;
				d.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
				return found;
			};
			test::require(p.weapon.weaponType == 5, "the duel's player was expected to start with fists");
			const state::Inventory::Entry worn = d.inventoryOf(pid)->items[0];
			test::require(worn.slot >= state::Inventory::kArmourBase, "the player's first entry is not worn armour");
			d.run(2);
			const std::uint32_t version0 = d.inventoryOf(pid)->version;
			d.expectReject(d.equip(pid, katanaRecord, true), "not_allowed"); // not carried yet
			d.expectReject(d.equip(pid, breadRecord, true), "bad_target"); // an ITEM
			d.expectReject(d.equip(pid, "no-such-record", true), "bad_target");
			d.expectReject(d.equip(pid, worn.itemSid, true), "not_allowed"); // worn, not carried
			// A Katana in the pack: equip it, and the fighter's weapon follows from the record.
			{
				state::Inventory pack = *d.inventoryOf(pid);
				pack.add(katanaRecord, 1, state::Inventory::kCarried, katanaQuality);
				entityOf(pid).set<state::Inventory>(std::move(pack));
			}
			d.expectApplied(d.equip(pid, katanaRecord, true));
			d.run(2);
			const state::FighterWeapon want = data->buildWeapon(*gd, *gd->find(katanaRecord), katanaQuality);
			test::require(p.weapon.sid == katanaRecord && p.weapon.weaponType == want.weaponType && p.weapon.cutMult == want.cutMult
				&& p.weapon.cut1 == want.cut1 && p.weapon.weight == want.weight, "the fighter's weapon is not the equipped Katana");
			test::require(d.inventoryOf(pid)->carried(katanaRecord) == 0, "the equipped Katana is still carried");
			{
				bool wielded = false;
				for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
					wielded = wielded || (e.itemSid == katanaRecord && e.slot == state::Inventory::kWeapon && e.count == 1);
				test::require(wielded, "the Katana is not in the weapon slot");
				bool seen = false;
				for (const Rig::InvEntry& e : d.inventories[pid].items)
					seen = seen || (e.item == katanaRecord && e.slot == state::Inventory::kWeapon);
				test::require(seen && d.inventories[pid].version == d.inventoryOf(pid)->version && d.inventoryOf(pid)->version > version0,
					"the client's INVENTORY does not show the wielded Katana (version bump)");
			}
			d.expectReject(d.equip(pid, katanaRecord, true), "not_allowed"); // already wielded
			// Unequip: back to the pack, fists again.
			d.expectApplied(d.equip(pid, katanaRecord, false));
			d.run(2);
			test::require(p.weapon.weaponType == 5 && d.inventoryOf(pid)->carried(katanaRecord) == 1, "the unequipped Katana is not a carried item with fists in hand");
			// Armour: unequip the worn piece, equip it again; it is rebuilt from its instance quality, to exactly the same numbers.
			const std::size_t armourCount = p.armour.size();
			const auto wornPiece = [&]
			{
				for (const state::FighterArmour& piece : p.armour)
				{
					if (piece.sid == worn.itemSid)
						return piece;
				}
				throw std::runtime_error("the worn piece is gone");
			};
			const state::FighterArmour wornBefore = wornPiece();
			test::require(wornBefore.quality == worn.quality && worn.quality.set(), "the worn piece's inventory instance does not carry the fighter's quality");
			d.expectApplied(d.equip(pid, worn.itemSid, false));
			d.run(2);
			test::require(p.armour.size() == armourCount - 1 && d.inventoryOf(pid)->carried(worn.itemSid) == 1, "the unequipped armour is still worn");
			d.expectApplied(d.equip(pid, worn.itemSid, true));
			d.run(2);
			test::require(p.armour.size() == armourCount && d.inventoryOf(pid)->carried(worn.itemSid) == 0, "the equipped armour is not worn");
			{
				const state::FighterArmour expect = data->buildArmour(*gd->find(worn.itemSid), worn.quality);
				bool found = false;
				for (const state::FighterArmour& piece : p.armour)
					found = found || (piece.sid == worn.itemSid && piece.cut == expect.cut && piece.blunt == expect.blunt && piece.pierce == expect.pierce
						&& piece.slot == expect.slot && piece.coverage.size() == expect.coverage.size());
				test::require(found, "the re-equipped armour piece does not carry the numbers buildArmour gives");
				const state::FighterArmour wornAfter = wornPiece();
				test::require(wornAfter.cut == wornBefore.cut && wornAfter.blunt == wornBefore.blunt && wornAfter.pierce == wornBefore.pierce
					&& wornAfter.cutIntoStun == wornBefore.cutIntoStun && wornAfter.combatSpeedMult == wornBefore.combatSpeedMult
					&& wornAfter.quality == wornBefore.quality, "the re-equipped armour piece's numbers changed across unequip and equip");
				bool worn2 = false;
				for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
					worn2 = worn2 || (e.itemSid == worn.itemSid && e.slot == worn.slot);
				test::require(worn2, "the re-equipped armour is not in its slot");
			}
			// ---- item instances: a weapon keeps its generated quality across equip, unequip, drop and pick up ----
			{
				const kdata::Record& katanaRec = *gd->find(katanaRecord);
				const state::ItemQuality other = data->rollQuality(*gd, katanaRec, 0.05, 0.9);
				test::require(katanaQuality.set() && katanaQuality.grade == 2 && other.set() && other.grade == 0 && !(other == katanaQuality)
					&& !katanaQuality.maker.empty() && !katanaQuality.model.empty(), "the two test qualities are not two distinct graded weapon qualities");
				// The builder merges unplaced entries of equal quality; the grid then caps the stacks: weapons never stack (one per entry),
				// so what was one Katana stack of 3 per quality is 3 entries. (Changed: the test assumed stacks of weapons by quality.)
				{
					state::Inventory probe;
					probe.add(katanaRecord, 1, state::Inventory::kCarried, katanaQuality);
					probe.add(katanaRecord, 1, state::Inventory::kCarried, other);
					probe.add(katanaRecord, 2, state::Inventory::kCarried, katanaQuality);
					probe.add(breadRecord, 1);
					probe.add(breadRecord, 1);
					test::require(probe.items.size() == 3 && probe.items[0].count == 3 && probe.items[1].count == 1 && probe.items[2].count == 2
						&& probe.carried(katanaRecord) == 4, "stacks of different quality merged, or equal ones did not");
					grid::settle(probe, grid::Rules(*gd), 1);
					std::size_t katanaEntriesSeen = 0;
					for (const state::Inventory::Entry& e : probe.items)
						katanaEntriesSeen += e.itemSid == katanaRecord ? (e.count == 1 ? 1 : 100) : 0;
					test::require(katanaEntriesSeen == 4 && probe.carried(katanaRecord) == 4, "weapons did not split into one entry each");
				}
				const auto numbers = [](const state::FighterWeapon& w)
				{
					return std::vector<double>{w.cutMult, w.bluntMult, w.bleedMult, w.pierceMult, w.cut1, w.blunt1, w.humanMult, w.animalMult, w.robotMult,
						w.armourPenetration, w.length, w.weight, w.attackMod, w.defenceMod};
				};
				const auto katanaEntries = [&](std::uint8_t slot)
				{
					std::vector<state::Inventory::Entry> found;
					for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
					{
						if (e.itemSid == katanaRecord && e.slot == slot)
							found.push_back(e);
					}
					return found;
				};
				// Pack: the Katana from before (quality katanaQuality) plus one more of it and one of the other quality.
				{
					state::Inventory pack = *d.inventoryOf(pid);
					pack.add(katanaRecord, 1, state::Inventory::kCarried, other);
					pack.add(katanaRecord, 1, state::Inventory::kCarried, katanaQuality);
					entityOf(pid).set<state::Inventory>(std::move(pack));
				}
				{
					const auto packed = katanaEntries(state::Inventory::kCarried);
					test::require(packed.size() == 3 && std::count_if(packed.begin(), packed.end(), [&](const state::Inventory::Entry& e) { return e.count == 1 && e.quality == katanaQuality; }) == 2
						&& std::count_if(packed.begin(), packed.end(), [&](const state::Inventory::Entry& e) { return e.count == 1 && e.quality == other; }) == 1,
						"the pack does not hold three Katana entries (two of one quality, one of the other)");
				}
				// Equip, unequip, equip: exactly the same combat numbers, and the instance keeps its quality.
				d.expectApplied(d.equip(pid, katanaRecord, true));
				d.run(2);
				const std::vector<double> first = numbers(p.weapon);
				test::require(first == numbers(want) && p.weapon.quality == katanaQuality, "the equipped Katana is not built from its instance quality");
				{
					const auto held = katanaEntries(state::Inventory::kWeapon);
					test::require(held.size() == 1 && held[0].quality == katanaQuality, "the wielded entry lost its quality");
					bool wire = false;
					for (const Rig::InvEntry& e : d.inventories[pid].items)
						wire = wire || (e.item == katanaRecord && e.slot == state::Inventory::kWeapon && e.maker == katanaQuality.maker && e.model == katanaQuality.model
							&& e.level == static_cast<float>(katanaQuality.level) && e.grade == katanaQuality.grade);
					test::require(wire,"the client's INVENTORY lacks the wielded Katana's manufacturer, model, level and grade");
				}
				d.expectApplied(d.equip(pid, katanaRecord, false));
				d.run(2);
				{
					const auto packed = katanaEntries(state::Inventory::kCarried);
					test::require(packed.size() == 3 && std::count_if(packed.begin(), packed.end(), [&](const state::Inventory::Entry& e) { return e.quality == katanaQuality; }) == 2,
						"the unequipped Katana did not come back with its quality");
				}
				// Several Katana in the pack: order 62 names only the record (the first entry), so the wielded quality is addressed by entry (order 67).
				{
					const auto& items = d.inventoryOf(pid)->items;
					std::size_t at = items.size();
					for (std::size_t i = 0; i < items.size(); ++i)
					{
						if (items[i].itemSid == katanaRecord && items[i].slot == state::Inventory::kCarried && items[i].quality == katanaQuality)
						{
							at = i;
							break;
						}
					}
					test::require(at < items.size(), "no carried Katana of the wielded quality");
					d.expectApplied(d.equipEntry(pid, static_cast<std::uint16_t>(at), true));
				}
				d.run(2);
				test::require(numbers(p.weapon) == first && p.weapon.quality == katanaQuality, "a re-equipped weapon's combat numbers changed");
				// Drop both carried Katana (one per quality) and pick them up again.
				d.inters.clear();
				std::set<std::uint64_t> lyingBefore; // the scenario's own ground Katana
				d.sim->world().ecs().each([&](const state::Id& i, const state::GroundItem&) { lyingBefore.insert(i.value); });
				d.expectApplied(d.drop(pid, katanaRecord, 2));
				d.run(2);
				// The first two pack entries go, whatever their qualities; equal ones lie together as one pile.
				test::require(katanaEntries(state::Inventory::kCarried).empty() && d.dropped.size() >= 1, "the drop of two Katana did not empty the pack");
				std::vector<std::pair<std::uint64_t, state::ItemQuality>> lying;
				d.sim->world().ecs().each([&](const state::Id& i, const state::GroundItem& g) { if (g.itemSid == katanaRecord && lyingBefore.count(i.value) == 0) lying.push_back({i.value, g.quality}); });
				std::sort(lying.begin(), lying.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				test::require(!lying.empty() && lying.size() <= 2, "the dropped Katana lay in no or too many piles");
				for (const auto& [id, quality] : lying)
					test::require(quality == katanaQuality || quality == other, "a dropped Katana lost its quality on the ground");
				for (const auto& [id, quality] : lying)
				{
					(void) quality;
					d.expectApplied(d.interact(pid, id, 7));
					for (int waited = 0; d.exists(id) && waited < 600; ++waited)
						d.tick();
					test::require(!d.exists(id), "a dropped Katana was not picked up");
				}
				d.run(2);
				{
					const auto packed = katanaEntries(state::Inventory::kCarried);
					test::require(packed.size() == 2 && std::count_if(packed.begin(), packed.end(), [&](const state::Inventory::Entry& e) { return e.count == 1 && e.quality == katanaQuality; }) == 1
						&& std::count_if(packed.begin(), packed.end(), [&](const state::Inventory::Entry& e) { return e.count == 1 && e.quality == other; }) == 1,
						"drop + pick up did not give back the two qualities");
				}
				// Swap: drop one Katana from the pack (the first, of the wielded quality) and equip the rest: the other quality comes
				// into hand, the old one goes back to the pack with its own quality, and the numbers follow the instance.
				// The pack holds two Katana (one of the wielded quality, one other); drop the first and equip the rest. Which one is left
				// depends on the pack order, the wielded Katana goes back to the pack with its own quality either way.
				const auto before2 = katanaEntries(state::Inventory::kCarried);
				test::require(before2.size() == 2, "the pack does not hold two Katana");
				d.expectApplied(d.drop(pid, katanaRecord, 1));
				d.run(2);
				const state::ItemQuality left = before2.at(1).quality; // the second entry stays
				d.expectApplied(d.equip(pid, katanaRecord, true));
				d.run(2);
				const state::FighterWeapon wantLeft = data->buildWeapon(*gd, katanaRec, left);
				test::require(numbers(p.weapon) == numbers(wantLeft) && p.weapon.quality == left && (left == katanaQuality || numbers(p.weapon) != first),
					"swapping to the remaining Katana did not rebuild the numbers from its instance");
				{
					const auto packed = katanaEntries(state::Inventory::kCarried);
					const auto held = katanaEntries(state::Inventory::kWeapon);
					test::require(packed.size() == 1 && packed[0].quality == katanaQuality && held.size() == 1 && held[0].quality == left,
						"the swapped-out Katana lost its quality");
				}
				// Save + load mid-scenario: the instances (and the fighter's weapon quality) come back and the hash matches.
				const std::string savePath = std::string(KSIM_BINARY_DIR) + "/ksim_interact_instances.save";
				d.sim->saveGame(savePath);
				const Simulation reloaded = Simulation::loadGame(KENSHI_DATA, order, savePath);
				test::require(reloaded.stateHash() == d.sim->stateHash(), "save + load of item instances changed the state hash");
				std::remove(savePath.c_str());
			}
			// The numbers follow into combat: the player armed with the Katana hits harder than bare-handed.
			std::cout << "equip: Katana in and out of hand, armour off and on; weapon cut mult " << want.cutMult << ", inventory version " << version0 << " -> " << d.inventoryOf(pid)->version << "\n";
		}

		// ---- containers (INTERACTABLE kind 3, INVENTORY under the container's id, order 63 transfer, loot, theft) ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			std::vector<std::uint64_t> boxes;
			for (const auto& [id, r] : d.inters)
			{
				if (r.kind == ks::kInteractableContainer)
					boxes.push_back(id);
			}
			test::require(!boxes.empty(), "the Hub scenario sent no container");
			// The kind 3 tail (k16.7.29): a layout box names its building and a data index, any other box 0xFFFF; a scenario stand-in (key 0) 0 + 0xFFFF.
			std::size_t layoutBoxes = 0;
			std::size_t standIns = 0;
			for (const std::uint64_t id : boxes)
			{
				flecs::entity e;
				d.sim->world().ecs().each([&](flecs::entity f, const state::Id& i, const state::Container&) { if (i.value == id) e = f; });
				const state::Container* held = e.get<state::Container>();
				const Inter& wire = d.inters[id];
				test::require(wire.building == held->building && wire.piece == held->piece, "a container's wire tail is not its state");
				if (held->key == 0)
				{
					test::require(wire.building == 0 && wire.piece == 0xFFFF, "a scenario crate does not send 0 + 0xFFFF");
					++standIns;
				} else if (wire.piece != 0xFFFF) {
					test::require(wire.building != 0, "a layout box names no building");
					++layoutBoxes;
				} else {
					test::require(wire.building == held->key, "a placed box does not send its placement key");
				}
			}
			std::cout << "container tails: " << layoutBoxes << " layout boxes, " << standIns << " scenario crates\n";
			std::uint64_t full = 0;
			for (const std::uint64_t id : boxes)
			{
				test::require(d.inters[id].record.size() > 0 && d.inventories.count(id) == 1, "a container has no record or no INVENTORY under its id");
				if (full == 0 && !d.inventories[id].items.empty())
					full = id;
			}
			test::require(full != 0, "no container has contents");
			const auto container = [&](std::uint64_t id)
			{
				flecs::entity found;
				d.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Container&) { if (i.value == id) found = e; });
				return found;
			};
			std::string mine;
			d.sim->world().ecs().each([&](const state::Id& i, const state::Allegiance& al) { if (i.value == pid) mine = al.factionSid; });
			const auto stand = [&](std::uint64_t id, double dx)
			{
				const state::Container* box = container(id).get<state::Container>();
				state::Fighter& f = d.mutableFighter(pid);
				f.x = box->useX + dx;
				f.y = box->useY;
				f.z = box->useZ;
				d.run(2);
			};
			const state::Inventory* held = container(full).get<state::Inventory>();
			const std::string item = held->items[0].itemSid;
			const std::uint32_t count0 = held->carried(item); // every unit of the record (stacks are capped, so it may lie in several entries)
			// Too far away: out_of_reach (task AX). Unknown container: bad_target.
			stand(full, 80.0);
			d.expectReject(d.transfer(pid, full, item, 1, true), "out_of_reach");
			d.expectReject(d.transfer(pid, katana, item, 1, true), "bad_target");
			stand(full, 2.0);
			d.expectReject(d.transfer(pid, full, item, static_cast<std::uint16_t>(count0 + 1), true), "not_allowed");
			d.expectReject(d.transfer(pid, full, item, 1, false), "not_allowed"); // the player carries none of it
			// An unowned container: taking is not theft.
			const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid.clear();
			const std::uint32_t before = d.inventoryOf(pid)->carried(item);
			const std::uint32_t boxVersion = held->version;
			d.expectApplied(d.transfer(pid, full, item, 1, true));
			d.run(2);
			test::require(d.inventoryOf(pid)->carried(item) == before + 1 && container(full).get<state::Inventory>()->carried(item) == count0 - 1,
				"a transfer did not move the item");
			test::require(d.inventories[full].version > boxVersion && d.inventories[pid].version == d.inventoryOf(pid)->version,
				"the INVENTORY records did not follow the transfer");
			test::require(d.thefts.empty(), "taking from an unowned container is not theft");
			// Put it back.
			d.expectApplied(d.transfer(pid, full, item, 1, false));
			d.run(2);
			test::require(container(full).get<state::Inventory>()->carried(item) == count0 && d.inventoryOf(pid)->carried(item) == before, "putting back did not move the item");
			// A foreign container: taking is theft (event 14), putting in is not.
			const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid = "someone-else";
			d.mutableFighter(pid).skills.thievery = 100.0; // a sure take (steal chance 127%); the roll itself is ksim_steal_chance's
			d.expectApplied(d.transfer(pid, full, item, 1, true));
			d.run(2);
			test::require(d.thefts.size() == 1 && d.thefts[0].who == pid && d.thefts[0].container == full && d.thefts[0].owner == "someone-else",
				"taking from a foreign container sent no theft event");
			d.expectApplied(d.transfer(pid, full, item, 1, false));
			d.run(2);
			test::require(d.thefts.size() == 1, "putting an item into a foreign container is not theft");
			// Own faction's container: not theft.
			const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid = mine;
			d.expectApplied(d.transfer(pid, full, item, 1, true));
			d.run(2);
			test::require(d.thefts.size() == 1, "taking from the own faction's container is not theft");
			d.expectApplied(d.transfer(pid, full, item, 1, false));
			d.run(2);
			// Quality round trip: two Katana of different quality in, then out; the container and the pack keep both as instances.
			{
				const std::string katanaSid = [&]
				{
					for (const kdata::Record& r : gd->records())
					{
						if (r.type == kworld::itemTypeByName("WEAPON") && r.name == "Katana")
							return r.stringID;
					}
					throw std::runtime_error("no Katana");
				}();
				const state::ItemQuality qa = data->rollQuality(*gd, *gd->find(katanaSid), 0.75, 0.0);
				const state::ItemQuality qb = data->rollQuality(*gd, *gd->find(katanaSid), 0.05, 0.9);
				state::Inventory pack = *d.inventoryOf(pid);
				pack.add(katanaSid, 1, state::Inventory::kCarried, qa);
				pack.add(katanaSid, 1, state::Inventory::kCarried, qb);
				flecs::entity player;
				d.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == pid) player = e; });
				player.set<state::Inventory>(std::move(pack));
				d.expectApplied(d.transfer(pid, full, katanaSid, 2, false));
				d.run(2);
				const auto instances = [&](const state::Inventory* inventory)
				{
					std::vector<state::ItemQuality> found;
					for (const state::Inventory::Entry& e : inventory->items)
					{
						if (e.itemSid == katanaSid && e.slot == state::Inventory::kCarried)
							found.push_back(e.quality);
					}
					return found;
				};
				const std::vector<state::ItemQuality> want{qa, qb};
				test::require(instances(container(full).get<state::Inventory>()) == want && instances(d.inventoryOf(pid)).empty(),
					"a transfer into a container lost the Katana's quality");
				d.expectApplied(d.transfer(pid, full, katanaSid, 2, true));
				d.run(2);
				// A take into a character is the quick move (order 63 as 68): with the weapon slot free the first Katana is wielded, the other
				// is carried. Both instances keep their quality wherever they went.
				const auto everywhere = [&](const state::Inventory* inventory)
				{
					std::vector<state::ItemQuality> found = instances(inventory);
					for (const state::Inventory::Entry& e : inventory->items)
					{
						if (e.itemSid == katanaSid && e.slot == state::Inventory::kWeapon)
							found.insert(found.begin(), e.quality);
					}
					return found;
				};
				test::require(everywhere(d.inventoryOf(pid)) == want && instances(container(full).get<state::Inventory>()).empty(),
					"a transfer out of a container lost the Katana's quality");
			}
			// Loot (interact 5) takes all: walks there, event 13 done, container empty, theft when foreign.
			const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid = "someone-else";
			const std::size_t kinds = container(full).get<state::Inventory>()->items.size();
			std::uint32_t total = 0;
			for (const state::Inventory::Entry& e : container(full).get<state::Inventory>()->items)
				total += e.count;
			std::uint32_t carriedBefore = 0;
			for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
				carriedBefore += e.slot == state::Inventory::kCarried ? e.count : 0;
			stand(full, 40.0); // out of reach: the loot walks
			const std::size_t from = d.done.size();
			d.expectApplied(d.interact(pid, full, 5));
			int ticks = 0;
			while (!d.doneFor(full, from) && ticks++ < 800)
				d.tick();
			const auto looted = d.doneFor(full, from);
			test::require(looted && looted->action == 5 && looted->result == 0, "the loot order on a container did not finish");
			test::require(container(full).get<state::Inventory>()->items.empty(), "loot left items in the container");
			std::uint32_t carriedAfter = 0;
			for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
				carriedAfter += e.slot == state::Inventory::kCarried ? e.count : 0;
			test::require(carriedAfter == carriedBefore + total, "loot did not move every unit");
			test::require(d.thefts.size() == 2, "looting a foreign container sent no theft event");
			// A pack with room for one Katana: looting two takes one and reports it (event 13 result 6, partial), the other stays in the container;
			// then nothing fits (result 1); a ground Katana is not picked up either (result 1) and stays lying there.
			{
				const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid = mine; // not theft
				const std::string katanaSid = [&]
				{
					for (const kdata::Record& r : gd->records())
					{
						if (r.type == kworld::itemTypeByName("WEAPON") && r.name == "Katana")
							return r.stringID;
					}
					throw std::runtime_error("no Katana");
				}();
				const state::ItemQuality q1 = data->rollQuality(*gd, *gd->find(katanaSid), 0.75, 0.0);
				const state::ItemQuality q2 = data->rollQuality(*gd, *gd->find(katanaSid), 0.05, 0.9);
				flecs::entity player;
				d.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == pid) player = e; });
				state::Inventory tight = *d.inventoryOf(pid);
				tight.items.erase(std::remove_if(tight.items.begin(), tight.items.end(), [](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kCarried; }), tight.items.end());
				tight.gridW = 7;
				tight.gridH = 1;
				++tight.version;
				player.set<state::Inventory>(std::move(tight));
				state::Inventory crate = *container(full).get<state::Inventory>();
				crate.add(katanaSid, 1, state::Inventory::kCarried, q1);
				crate.add(katanaSid, 1, state::Inventory::kCarried, q2);
				++crate.version;
				container(full).set<state::Inventory>(std::move(crate));
				stand(full, 2.0);
				std::size_t from3 = d.done.size();
				d.expectApplied(d.interact(pid, full, 5));
				for (int waited = 0; !d.doneFor(full, from3) && waited < 400; ++waited)
					d.tick();
				const auto partial = d.doneFor(full, from3);
				test::require(partial && partial->result == InteractEvent::kPartial, "a loot that only partly fits did not end with result 6");
				test::require(d.inventoryOf(pid)->carried(katanaSid) == 1 && container(full).get<state::Inventory>()->carried(katanaSid) == 1,
					"the partial loot did not split the Katana between the pack and the container");
				from3 = d.done.size();
				d.expectApplied(d.interact(pid, full, 5));
				for (int waited = 0; !d.doneFor(full, from3) && waited < 400; ++waited)
					d.tick();
				const auto none = d.doneFor(full, from3);
				test::require(none && none->result == InteractEvent::kRefused && container(full).get<state::Inventory>()->carried(katanaSid) == 1, "a loot with no room did not end with result 1");
				state::GroundItem lying;
				lying.itemSid = katanaSid;
				lying.count = 1;
				lying.quality = q1;
				const state::Fighter& at = d.mutableFighter(pid);
				lying.x = lying.useX = at.x;
				lying.y = lying.useY = at.y;
				lying.z = lying.useZ = at.z;
				const std::uint64_t groundId = d.sim->spawnGroundItem(std::move(lying)).get<state::Id>()->value;
				d.run(2);
				from3 = d.done.size();
				d.expectApplied(d.interact(pid, groundId, 7));
				for (int waited = 0; !d.doneFor(groundId, from3) && waited < 400; ++waited)
					d.tick();
				const auto refused = d.doneFor(groundId, from3);
				test::require(refused && refused->result == InteractEvent::kRefused && d.exists(groundId) && d.inventoryOf(pid)->carried(katanaSid) == 1,
					"a pick-up with no room did not leave the item on the ground (result 1)");
				// Empty it again for the checks below.
				state::Inventory emptied = *container(full).get<state::Inventory>();
				emptied.items.clear();
				++emptied.version;
				container(full).set<state::Inventory>(std::move(emptied));
				const_cast<state::Container*>(container(full).get<state::Container>())->ownerSid = "someone-else";
			}
			// An empty container: loot is allowed and takes nothing (and is no theft).
			d.expectApplied(d.interact(pid, full, 5));
			d.run(40);
			test::require(d.thefts.size() == 2, "looting an empty container is no theft");
			std::cout << "containers: " << boxes.size() << " in view, transfer, loot of " << kinds << " stacks (" << total << " units), theft events " << d.thefts.size() << "\n";
			// Save and load: containers and their contents come back.
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_interact_containers.save";
			d.sim->saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			std::size_t count = 0;
			loaded.world().ecs().each([&](const state::Id&, const state::Container&) { ++count; });
			std::size_t before2 = 0;
			d.sim->world().ecs().each([&](const state::Id&, const state::Container&) { ++before2; });
			test::require(count == before2 && count >= boxes.size() && loaded.stateHash() == d.sim->stateHash(), "containers lost in the save");
			std::remove(path.c_str());
		}

		// ---- loot open (order 60 flags bit 0), and bodies as containers for orders 63 / 64 / 65 / 68 ----
		{
			Rig d(gd, data, nav, order, true);
			const std::uint64_t pid = d.duel.player;
			const std::uint64_t foe = d.duel.enemy;
			const auto entityOf = [&](std::uint64_t id)
			{
				flecs::entity found;
				d.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == id) found = e; });
				return found;
			};
			// The body belongs to another faction; it carries four Bread.
			flecs::entity foeEntity = entityOf(foe);
			state::Inventory foeStuff = *foeEntity.get<state::Inventory>();
			foeStuff.add(breadRecord, 4);
			const bool armed = std::any_of(foeStuff.items.begin(), foeStuff.items.end(), [](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kWeapon; });
			if (!armed)
				foeStuff.add(katanaRecord, 1, state::Inventory::kWeapon, katanaQuality);
			++foeStuff.version;
			foeEntity.set<state::Inventory>(foeStuff);
			foeEntity.set<state::Allegiance>(state::Allegiance{"bandit-test"});
			std::string mine;
			d.sim->world().ecs().each([&](const state::Id& i, const state::Allegiance& al) { if (i.value == pid) mine = al.factionSid; });
			test::require(mine != "bandit-test", "the test faction is the player's");
			const auto stand = [&](double dx)
			{
				state::Fighter& f = d.mutableFighter(pid);
				const state::Fighter& body = d.enemy();
				f.x = body.x + dx;
				f.y = body.y;
				f.z = body.z;
				f.clearRoute();
				d.run(2);
			};
			stand(3.0);
			// A bad flag bit, a flag on another action: bad_value. A conscious target: bad_target (take-all keeps not_allowed).
			d.expectReject(d.interactFlags(pid, foe, 5, 2), "bad_value");
			d.expectReject(d.interactFlags(pid, foe, 5, 3), "bad_value");
			d.expectReject(d.interactFlags(pid, foe, 5, 0x80), "bad_value");
			d.expectReject(d.interactFlags(pid, katana, 7, 1), "bad_value");
			d.expectReject(d.interactFlags(pid, foe, 5, 1), "bad_target");
			d.expectReject(d.interactFlags(pid, foe, 5, 0), "not_allowed");
			d.expectReject(d.interact(pid, foe, 5), "not_allowed");
			d.expectReject(d.transfer(pid, foe, breadRecord, 1, true), "bad_target"); // a standing character is no container
			// Knock the body out. Nothing was sent for it yet.
			state::Fighter& body = d.mutableFighter(foe);
			body.unconscious = true;
			body.koTimer = 1e6;
			body.hold = true;
			body.parts.front().health = body.parts.front().maxHealth - 10.0;
			d.run(2);
			test::require(d.inventories.count(foe) == 0, "the body's INVENTORY went out before any open");
			// Open from afar: the looter walks, nothing is taken, event 13 result 0 marks the arrival, then INVENTORY + ITEM_GRID.
			stand(60.0);
			const std::size_t from = d.done.size();
			d.expectApplied(d.interactFlags(pid, foe, 5, 1));
			int ticks = 0;
			while (!d.doneFor(foe, from) && ticks++ < 2000)
				d.tick();
			const auto arrived = d.doneFor(foe, from);
			test::require(arrived && arrived->action == 5 && arrived->result == 0, "the open order did not arrive");
			d.run(2);
			test::require(entityOf(foe).get<state::Inventory>()->carried(breadRecord) == 4, "an open took something");
			test::require(d.inventories.count(foe) == 1 && d.inventories[foe].records == 1 && d.gridRecords[foe] == 1
				&& d.inventories[foe].version == entityOf(foe).get<state::Inventory>()->version, "the opened body's INVENTORY + ITEM_GRID did not arrive");
			// A change while in range is sent; a take from the other faction's body is theft (event 14).
			const std::size_t thefts0 = d.thefts.size();
			const std::uint32_t mineBread = d.inventoryOf(pid)->carried(breadRecord);
			d.expectApplied(d.transfer(pid, foe, breadRecord, 1, true));
			d.run(2);
			test::require(d.inventoryOf(pid)->carried(breadRecord) == mineBread + 1 && entityOf(foe).get<state::Inventory>()->carried(breadRecord) == 3,
				"a transfer from a body did not move the item");
			test::require(d.inventories[foe].records == 2 && d.gridRecords[foe] == 2 && d.inventories[foe].version == entityOf(foe).get<state::Inventory>()->version,
				"a change to the opened body was not sent");
			test::require(d.thefts.size() == thefts0 + 1 && d.thefts.back().who == pid && d.thefts.back().container == foe && d.thefts.back().owner == "bandit-test",
				"taking from another faction's body sent no theft event");
			{
				// Looting (140712490 L93): the victim is KO'd, so the take from the body is crime 10, not theft 3.
				const state::CrimeRecord* looter = entityOf(pid).get<state::CrimeRecord>();
				test::require(looter != nullptr && looter->crime == crime::kLooting && looter->victimFaction == "bandit-test", "taking from a KO'd body is not looting");
			}
			{
				// Provenance (save v89): the theft marked the taken bread with the victim faction; its own stack, not merged with the clean bread.
				const state::Inventory* mine = d.inventoryOf(pid);
				std::uint32_t marked = 0;
				for (const state::Inventory::Entry& entry : mine->items)
					marked += entry.stolen == "bandit-test" ? entry.count : 0;
				test::require(marked == 1, "the bread taken from a foreign body is not marked stolen");
			}
			// A put into the body is no theft; the entry orders work on it as on a container.
			d.expectApplied(d.transfer(pid, foe, breadRecord, 1, false));
			d.run(2);
			test::require(d.thefts.size() == thefts0 + 1 && d.inventories[foe].records == 3, "putting into a body is no theft and is sent");
			{
				// A put into another character's pack is a planting: the holder's stack lists the planter (silent: it merged with the bread there).
				std::uint32_t planted = 0;
				for (const state::Inventory::Entry& entry : entityOf(foe).get<state::Inventory>()->items)
				{
					for (const state::Planting& p : entry.planted)
						planted += p.planter == pid ? p.units : 0;
				}
				test::require(planted == 1, "putting into a body recorded no planter");
			}
			{
				const std::uint32_t version = d.inventories[foe].version;
				std::uint16_t index = 0;
				for (std::size_t i = 0; i < d.inventories[foe].items.size(); ++i)
				{
					if (d.inventories[foe].items[i].item == breadRecord)
						index = static_cast<std::uint16_t>(i);
				}
				const std::uint32_t taker = d.order(68, [&](ks::BinaryWriter& w)
				{
					w.u64("id", pid);
					w.u64("container", foe);
					w.u32("version", version);
					w.u16("index", index);
					w.u16("count", 1);
					w.u8("take", 1);
					w.u8("section", 255);
					w.u8("x", 0);
					w.u8("y", 0);
				});
				d.expectApplied(taker);
				d.run(2);
				test::require(d.thefts.size() == thefts0 + 2, "an entry take from a body is theft");
				d.expectApplied(d.order(65, [&](ks::BinaryWriter& w) { w.u64("id", foe); w.u8("section", 0); }));
			}
			// Stripping: 68 takes an equipped entry of the body (section 255 in its INVENTORY), 63 falls back to the equipped ones.
			{
				flecs::entity me = entityOf(pid);
				state::Inventory roomy = *me.get<state::Inventory>();
				roomy.gridH = 40;
				++roomy.version;
				me.set<state::Inventory>(roomy);
				d.run(2);
				const auto equippedIndex = [&](bool weapon) -> int
				{
					const Rig::Inv& wire = d.inventories[foe];
					for (std::size_t i = 0; i < wire.items.size(); ++i)
					{
						const std::uint8_t slot = wire.items[i].slot;
						if (weapon ? slot == state::Inventory::kWeapon : (slot >= state::Inventory::kArmourBase && slot != state::Inventory::kBackpackSlot))
							return static_cast<int>(i);
					}
					return -1;
				};
				const auto strip = [&](int index, std::uint8_t section, std::uint8_t x, std::uint8_t y)
				{
					const std::uint32_t version = d.inventories[foe].version;
					return d.order(68, [&](ks::BinaryWriter& w)
					{
						w.u64("id", pid);
						w.u64("container", foe);
						w.u32("version", version);
						w.u16("index", static_cast<std::uint16_t>(index));
						w.u16("count", 1);
						w.u8("take", 1);
						w.u8("section", section);
						w.u8("x", x);
						w.u8("y", y);
					});
				};
				const int armour = equippedIndex(false);
				test::require(armour >= 0 && equippedIndex(true) >= 0, "the body has no equipped armour or weapon to strip");
				const std::string armourSid = d.inventories[foe].items[static_cast<std::size_t>(armour)].item;
				const std::size_t worn = d.enemy().armour.size();
				const std::uint32_t bodyVersion = entityOf(foe).get<state::Inventory>()->version;
				const std::size_t thefts1 = d.thefts.size();
				// An occupied rectangle: no_room, and the body is left as it was.
				d.expectReject(strip(armour, 0, 0, 0), "no_room");
				test::require(entityOf(foe).get<state::Inventory>()->version == bodyVersion && d.enemy().armour.size() == worn && d.thefts.size() == thefts1,
					"a stripping that found no room changed the body");
				// Into a free rectangle.
				const std::uint32_t armourBefore = d.inventoryOf(pid)->carried(armourSid);
				d.expectApplied(strip(armour, 0, 0, 30));
				d.run(2);
				test::require(d.inventoryOf(pid)->carried(armourSid) == armourBefore + 1 && d.enemy().armour.size() == worn - 1,
					"stripping armour did not move it to the looter and off the body");
				test::require(d.thefts.size() == thefts1 + 1 && d.thefts.back().container == foe, "stripping another faction's body is theft");
				test::require(equippedIndex(false) < 0 || d.inventories[foe].items[static_cast<std::size_t>(equippedIndex(false))].item != armourSid, "the stripped armour is still worn in the body's INVENTORY");
				// 63: the body carries no Katana (carried first), so the equipped one goes.
				const std::string weaponSid = d.inventories[foe].items[static_cast<std::size_t>(equippedIndex(true))].item;
				test::require(entityOf(foe).get<state::Inventory>()->carried(weaponSid) == 0, "the body carries the weapon too");
				// The take is the quick move: the weapon may be wielded at once (a free weapon slot) or carried; either way the looter holds one more.
				const auto holds = [&](const char* sid)
				{
					std::uint32_t n = d.inventoryOf(pid)->carried(sid);
					for (const state::Inventory::Entry& e : d.inventoryOf(pid)->items)
						n += e.slot == state::Inventory::kWeapon && e.itemSid == sid ? e.count : 0;
					return n;
				};
				const std::uint32_t weaponBefore = holds(weaponSid.c_str());
				d.expectApplied(d.transfer(pid, foe, weaponSid, 1, true));
				d.run(2);
				test::require(holds(weaponSid.c_str()) == weaponBefore + 1 && equippedIndex(true) < 0 && d.enemy().weapon.weaponType == 5,
					"63 did not fall back to the equipped weapon");
				test::require(d.thefts.size() == thefts1 + 2, "63 stripping another faction's body is theft");
				// Giving never equips onto a body: the armour goes in carried.
				d.expectApplied(d.transfer(pid, foe, armourSid, 1, false));
				d.run(2);
				test::require(d.enemy().armour.size() == worn - 1 && entityOf(foe).get<state::Inventory>()->carried(armourSid) == 1, "giving equipped onto a body");
			}
			// Leaving the range stops the updates.
			const std::size_t records = d.inventories[foe].records;
			stand(60.0);
			state::Inventory changed = *entityOf(foe).get<state::Inventory>();
			changed.add(breadRecord, 1);
			++changed.version;
			entityOf(foe).set<state::Inventory>(changed);
			d.run(6);
			test::require(d.inventories[foe].records == records && d.inventories[foe].version != changed.version, "the opened body kept sending out of range");
			d.expectReject(d.transfer(pid, foe, breadRecord, 1, true), "out_of_reach");
			d.expectReject(d.order(65, [&](ks::BinaryWriter& w) { w.u64("id", foe); w.u8("section", 0); }), "out_of_reach");
			// Flags 0 = take-all as before: the body is emptied.
			stand(3.0);
			const std::size_t from2 = d.done.size();
			d.expectApplied(d.interactFlags(pid, foe, 5, 0));
			for (ticks = 0; !d.doneFor(foe, from2) && ticks < 2000; ++ticks)
				d.tick();
			const auto taken = d.doneFor(foe, from2);
			test::require(taken && taken->result == 0 && entityOf(foe).get<state::Inventory>()->items.empty(), "flags 0 did not take everything");
			std::cout << "loot open: arrival, frames while in range, theft on take, none out of range, flags 0 takes all\n";
		}

		// ---- determinism, and save + load mid-walk ----
		const auto script = [](Rig& r, int run)
		{
			std::uint64_t pick = 0;
			r.sim->world().ecs().each([&](const state::Id& i, const state::GroundItem&) { pick = pick == 0 ? i.value : pick; });
			std::uint64_t doorId = 0;
			r.sim->world().ecs().each([&](const state::Id& i, const state::Door& d) { if (doorId == 0 && d.state == 1) doorId = i.value; });
			(void) doorId;
			if (r.sim->clockTick() == 0)
				r.interact(r.duel.player, pick, 7);
			r.run(run);
		};
		Rig a(gd, data, nav, order);
		a.farItems(50.0, false);
		script(a, 120);
		Rig b(gd, data, nav, order);
		b.farItems(50.0, false);
		script(b, 120);
		test::require(a.sim->stateHash() == b.sim->stateHash(), "two identical interact runs differ");

		Rig split(gd, data, nav, order);
		split.farItems(50.0, false);
		script(split, 3); // mid-walk to the item: the interact task is saved
		test::require(split.player().walking(), "the save point is not mid-walk");
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_interact.save";
		split.sim->saveGame(path);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
		CombatSettings settings;
		settings.substeps = 2;
		loaded.enableCombat(data, settings);
		loaded.enableNavigation(nav);
		loaded.run(117);
		test::require(loaded.stateHash() == a.sim->stateHash(), "save mid-walk + load + run differs from the uninterrupted run");
		std::remove(path.c_str());
		{
			// The saved doors and items came back with their state.
			std::size_t doorCount = 0;
			std::size_t itemCount = 0;
			loaded.world().ecs().each([&](const state::Id&, const state::Door&) { ++doorCount; });
			loaded.world().ecs().each([&](const state::Id&, const state::GroundItem&) { ++itemCount; });
			std::size_t doorsBefore = 0;
			a.sim->world().ecs().each([&](const state::Id&, const state::Door&) { ++doorsBefore; });
			test::require(doorCount == doorsBefore && doorCount >= doors.size(), "doors lost in the save");
			test::require(itemCount == 1, "the ground items after the pick-up and a load: " + std::to_string(itemCount));
		}

		// ---- lift and put down through the session (carry.hpp): carriedBy and the carrying flag on the wire, event 13 ----
		{
			Rig lift(gd, data, nav, order);
			const std::uint64_t carrier = lift.duel.player;
			const std::uint64_t body = lift.duel.enemy;
			lift.mutableFighter(body).unconscious = true;
			lift.mutableFighter(body).koTimer = 600.0;
			lift.expectApplied(lift.interact(carrier, body, 8));
			lift.run(120);
			test::require(lift.doneFor(body).has_value() && lift.doneFor(body)->action == 8 && lift.doneFor(body)->result == 0, "no event 13 for the lift");
			test::require(lift.carriedBy[body] == carrier && lift.carriedBy[carrier] == 0, "CHAR_DYNAMIC carriedBy after the lift");
			test::require((lift.flags[carrier] & ks::kFlagCarryL) != 0 && (lift.flags[body] & ks::kFlagCarryL) == 0, "CHAR_DYNAMIC carrying flag after the lift");
			lift.expectApplied(lift.interact(carrier, body, 13));
			lift.run(40);
			test::require(lift.carriedBy[body] == 0 && (lift.flags[carrier] & ks::kFlagCarryL) == 0, "CHAR_DYNAMIC after the put-down");
			std::size_t putDowns = 0;
			for (const Rig::Done& d : lift.done)
				putDowns += d.action == 13 && d.result == 0;
			test::require(putDowns == 1, "event 13 for the put-down");
		}

		// ---- tiles that leave the detail bounds are sent with size 0 ----
		{
			Rig far(gd, data, nav, order);
			far.run(2);
			const std::size_t had = far.navTiles.size();
			test::require(had > 0, "no tiles for the far rig");
			far.mutableFighter(far.duel.player).x += 20000.0;
			far.mutableFighter(far.duel.player).z += 20000.0;
			far.run(3);
			test::require(far.navRemoved.size() == had && far.navTiles.empty(), "tiles that left the bounds were not removed");
		}

		// ---- the gait ramp is knav's: a client predicting a stop-start with knav::Walker::drive matches the server ----
		{
			Rig gait(gd, data, nav, order);
			gait.mutableFighter(gait.duel.enemy).dead = true; // nobody to fight: the only motion is the drive's
			gait.run(2);
			const state::Fighter& p0 = gait.player();
			// A heading with room to run: the longest free run of 3 s at 40 u/s from the start.
			double heading = 0.0;
			double best = -1.0;
			for (int k = 0; k < 16; ++k)
			{
				const double h = k * 0.39269908;
				knav::Position q{p0.x, p0.y, p0.z};
				double travelled = 0.0;
				for (int i = 0; i < 60; ++i)
				{
					const knav::StepResult r = nav->walker().drive(q, knav::Drive::Forward, h, 40.0, 0.05);
					travelled += r.moved;
					q = r.at;
				}
				if (travelled > best)
				{
					best = travelled;
					heading = h;
				}
			}
			test::require(best > 100.0, "no free run for the gait test");
			const auto drive = [&](std::uint8_t mode)
			{
				gait.order(20, [&](ks::BinaryWriter& w) { w.u64("id", gait.duel.player); w.u8("drive", mode); w.f32("heading", static_cast<float>(heading)); });
			};
			// Hold: no chasing or footwork, so the only motion is the drive's (a stop stands still).
			gait.order(4, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", gait.duel.player); w.u8("hold", 1); });
			gait.run(6);
			test::require(gait.player().forwardSpeed == 0.0 && gait.player().hold, "the gait test's character does not stand still: speed "
				+ std::to_string(gait.player().forwardSpeed) + " hold " + std::to_string(gait.player().hold) + " state " + std::to_string((int) gait.player().state));
			const RaceCombat* race = data->race(p0.raceSid);
			knav::Gait g;
			g.speedMinSkill = race->speedMinSkill;
			g.speedMaxSkill = race->speedMaxSkill;
			g.walkSpeed = race->walkSpeed;
			g.athletics = p0.skills.athletics * CombatRules::effectiveStat(p0, CombatRules::kStatAthletics);
			g.run = p0.moveMode != 0;
			constexpr int kRigSubsteps = 2; // the Rig's CombatSettings::substeps
			knav::Position client{p0.x, p0.y, p0.z};
			double speed = p0.forwardSpeed;
			double worst = 0.0;
			double topSpeed = 0.0;
			// Forward 2 s, stop 0.3 s, forward again 2 s, back 0.5 s, forward 0.4 s.
			const struct { std::uint8_t mode; int ticks; } plan[] = {{1, 40}, {0, 6}, {1, 40}, {2, 10}, {1, 8}};
			for (const auto& leg : plan)
			{
				drive(leg.mode);
				for (int i = 0; i < leg.ticks; ++i)
				{
					gait.tick();
					const state::Fighter& server = gait.player();
					const knav::Drive mode = static_cast<knav::Drive>(server.drive);
					// The client steps like the server: the server's combat runs `substeps` steps per tick (the client must know it).
					for (int sub = 0; sub < kRigSubsteps; ++sub)
						client = nav->walker().drive(client, mode, server.driveHeading, knav::gaitSpeed(g), speed, 0.05 / kRigSubsteps).at;
					if (std::getenv("KGAIT_TRACE") != nullptr)
						std::fprintf(stderr, "mode %d server speed %.3f client %.3f  pos d %.3f drive %d heading %.3f/%.3f\n", (int) leg.mode, server.forwardSpeed, speed, std::hypot(client.x - server.x, client.z - server.z), (int) server.drive, server.driveHeading, heading);
					worst = std::max(worst, std::hypot(client.x - server.x, client.z - server.z));
					topSpeed = std::max(topSpeed, std::fabs(speed));
				}
			}
			std::cout << "gait ramp: client prediction within " << worst << " units of the server over a stop-start (top speed " << topSpeed
				<< ", gait " << knav::gaitSpeed(g) << ")\n";
			test::require(topSpeed > 20.0, "the gait test never got up to speed");
			test::require(worst <= 0.05, "the client's prediction with the shared gait ramp drifted from the server");
		}

		std::cout << "ksim_interact ok, hash " << std::hex << a.sim->stateHash() << std::dec << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
