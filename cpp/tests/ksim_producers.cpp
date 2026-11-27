// Producers (docs/reference/MINE_SELL_SLICE.md M1-M3; SIMLINK INTERACTABLE kind 4, orders 60 use / 63 / 68, TASK_PROGRESS kind 9, event 13
// result 7, event 14, PLAYER_SQUADS money, save v49). A wild iron and a wild copper deposit (foliage meshes with a `building type`, on the
// Hub navmesh) are worked through the very same function; nothing here names an ore except to look one up in the data.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

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
	Buf& str(const std::string& s)
	{
		put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
		bytes.insert(bytes.end(), s.begin(), s.end());
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
	bool done() const { return at == p.size(); }
};

// ---- what the client holds ----
struct Inter
{
	std::uint8_t kind = 0;
	std::string record;
	std::uint8_t state = 0;
	float health = 0, maxHealth = 0;
	std::uint16_t count = 0;
	std::uint32_t version = 0;
	std::string output; // the kind 4 tail
	std::uint16_t cap = 0;
	std::uint8_t operators = 0, maxOperators = 0;
	float progress = 0, rate = 0; // the 2026-11-02 tail
	std::uint64_t progressTick = 0;
};
struct Bld // a BUILDING (47) row as the focus panel reads it
{
	std::uint64_t id = 0, owner = 0;
	std::string record;
	std::uint8_t status = 0, workers = 0, maxWorkers = 0;
	std::uint32_t flags = 0, version = 0;
	float progress = 0, rate = 0, hp = 0, maxHp = 0, efficiency = 0, maxEfficiency = 0, quality = 0;
};
struct Dyn
{
	std::string action;
	std::uint64_t actionStart = 0;
	std::uint8_t actionFlags = 0;
	std::uint64_t tick = 0;
};
struct Inv
{
	std::uint32_t version = 0;
	std::vector<std::pair<std::string, std::uint32_t>> items;
	std::uint32_t gridVersion = 0;
	std::vector<std::array<int, 3>> grids; // section, w, h
	std::vector<std::array<int, 4>> cells; // index, section, x, y
};
struct Task
{
	std::uint64_t id = 0;
	std::uint8_t kind = 0;
	float progress = 0, rate = 0, value = 0;
};
struct Ev
{
	std::uint16_t kind = 0;
	std::uint64_t a = 0, b = 0;
	std::uint8_t action = 0, result = 0;
	std::string text;
};
struct Squads
{
	std::int64_t money = 0;
};

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
	std::uint32_t seq = 100;
	std::map<std::uint64_t, Inter> inter;
	std::map<std::uint64_t, Inv> inv;
	std::map<std::uint64_t, Dyn> dyn;
	std::vector<Bld> buildings; // every BUILDING row heard, in order
	std::vector<Task> tasks; // the last frame 39
	std::vector<Ev> events;
	std::vector<Squads> squads;

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
		connect();
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

	std::string text(std::uint32_t id) { return id == 0 ? std::string() : session->strings().text(id); }

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			Cursor c{frame.payload};
			switch (frame.type)
			{
			case ks::kInteractable:
			{
				c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				Inter r;
				r.kind = c.get<std::uint8_t>();
				r.record = text(c.get<std::uint32_t>());
				for (int i = 0; i < 6; ++i)
					c.get<float>();
				r.state = c.get<std::uint8_t>();
				r.health = c.get<float>();
				r.maxHealth = c.get<float>();
				r.count = c.get<std::uint16_t>();
				r.version = c.get<std::uint32_t>();
				if (r.kind == ks::kInteractableProducer)
				{
					r.output = text(c.get<std::uint32_t>());
					r.cap = c.get<std::uint16_t>();
					r.operators = c.get<std::uint8_t>();
					r.maxOperators = c.get<std::uint8_t>();
					r.progress = c.get<float>();
					r.rate = c.get<float>();
					r.progressTick = c.get<std::uint64_t>();
				}
				// the ownership tail (every kind): strid owner, u16 lockLevel, strid model, f32 yaw
				c.get<std::uint32_t>();
				c.get<std::uint16_t>();
				c.get<std::uint32_t>();
				c.get<float>();
				if (r.kind == ks::kInteractableBed)
				{
					// the bed tail (kind 5, ksim_player_sleep's record): u64 building, strid furniture, u64 occupant
					c.get<std::uint64_t>();
					c.get<std::uint32_t>();
					c.get<std::uint64_t>();
				}
				if (r.kind == ks::kInteractableContainer)
				{
					// the container tail (kind 3): u64 building, u16 piece
					c.get<std::uint64_t>();
					c.get<std::uint16_t>();
				}
				if (r.kind == ks::kInteractableMachine)
				{
					// the machine tail (kind 12): u8 machineKind, u8 state, strid input, strid output, a turret's u64 key
					const std::uint8_t machineKind = c.get<std::uint8_t>();
					c.get<std::uint8_t>();
					c.get<std::uint32_t>();
					c.get<std::uint32_t>();
					if (machineKind == 3)
						c.get<std::uint64_t>();
				}
				test::require(c.done(), "INTERACTABLE has trailing bytes (kind " + std::to_string(r.kind) + ")");
				inter[id] = r;
				break;
			}
			case ks::kBuilding:
			{
				c.get<std::uint64_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					Bld b;
					b.id = c.get<std::uint64_t>();
					b.record = text(c.get<std::uint32_t>());
					b.owner = c.get<std::uint64_t>();
					for (int k = 0; k < 7; ++k)
						c.get<float>(); // x y z, qw qx qy qz
					c.get<std::uint8_t>(); // floor
					b.status = c.get<std::uint8_t>();
					b.flags = c.get<std::uint32_t>();
					b.progress = c.get<float>();
					b.rate = c.get<float>();
					b.hp = c.get<float>();
					b.maxHp = c.get<float>();
					const std::uint8_t mats = c.get<std::uint8_t>();
					for (std::uint8_t k = 0; k < mats; ++k)
					{
						c.get<std::uint32_t>();
						c.get<std::uint16_t>();
						c.get<std::uint16_t>();
					}
					for (int k = 0; k < 5; ++k)
						c.get<float>(); // power value, supplied, demand, stored, capacity
					b.workers = c.get<std::uint8_t>();
					b.maxWorkers = c.get<std::uint8_t>();
					b.efficiency = c.get<float>();
					b.maxEfficiency = c.get<float>();
					b.quality = c.get<float>();
					c.get<std::uint64_t>(); // town
					b.version = c.get<std::uint32_t>();
					// Machines and containers of any owner are sent too since k16.5.26 (ksim_machine_wire); this test follows the wild deposits.
					if (b.id >= producers::kIdBase + producers::kFoliageIdBase && b.id < machines::kMachineIdBase)
						buildings.push_back(b);
				}
				test::require(c.done(), "BUILDING has trailing bytes after the rows");
				break;
			}
			case ks::kDynamic:
			{
				Dyn d;
				d.tick = c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				for (int i = 0; i < 3; ++i)
					c.get<float>();
				for (int i = 0; i < 3; ++i)
					c.get<float>(); // yaw, fwdSpeed, latSpeed
				const std::uint32_t flags = c.get<std::uint32_t>();
				c.get<std::uint64_t>(); // carriedBy
				c.get<float>();
				c.get<float>();
				d.action = text(c.get<std::uint32_t>());
				d.actionStart = c.get<std::uint64_t>();
				c.get<float>(); // actionSpeed
				c.get<std::uint8_t>(); // drive
				c.get<float>();
				c.get<std::uint32_t>();
				c.get<std::uint64_t>(); // building
				c.get<std::int16_t>();
				c.get<std::uint32_t>(); // appearanceVersion
				d.actionFlags = c.get<std::uint8_t>();
				if ((flags & ((1u << 15) | (1u << 16))) != 0) // the pose tail of a seated / asleep character
				{
					for (int i = 0; i < 4; ++i)
						c.get<float>();
					c.get<std::uint32_t>();
				}
				test::require(c.done(), "CHAR_DYNAMIC has trailing bytes after actionFlags (and the pose tail)");
				dyn[id] = d;
				break;
			}
			case ks::kInventory:
			{
				c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				Inv& v = inv[id];
				v.version = c.get<std::uint32_t>();
				v.items.clear();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					const std::string item = text(c.get<std::uint32_t>());
					const std::uint32_t count = c.get<std::uint32_t>();
					c.get<std::uint8_t>();
					v.items.emplace_back(item, count);
				}
				break;
			}
			case ks::kItemGrid:
			{
				c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				Inv& v = inv[id];
				v.gridVersion = c.get<std::uint32_t>();
				v.grids.clear();
				v.cells.clear();
				const std::uint8_t ns = c.get<std::uint8_t>();
				for (std::uint8_t i = 0; i < ns; ++i)
				{
					const int section = c.get<std::uint8_t>();
					const int w = c.get<std::uint8_t>();
					const int h = c.get<std::uint8_t>();
					v.grids.push_back({section, w, h});
				}
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					const int index = c.get<std::uint16_t>();
					const int section = c.get<std::uint8_t>();
					const int x = c.get<std::uint8_t>();
					const int y = c.get<std::uint8_t>();
					c.get<std::uint32_t>();
					c.get<std::uint32_t>();
					v.cells.push_back({index, section, x, y});
				}
				break;
			}
			case ks::kTaskProgress:
			{
				c.get<std::uint64_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				tasks.clear();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					Task t;
					t.id = c.get<std::uint64_t>();
					t.kind = c.get<std::uint8_t>();
					t.progress = c.get<float>();
					t.rate = c.get<float>();
					t.value = c.get<float>();
					tasks.push_back(t);
				}
				break;
			}
			case ks::kPlayerSquads:
			{
				Squads s;
				c.get<std::uint64_t>();
				c.get<std::uint64_t>();
				c.get<std::uint32_t>();
				for (int i = 0; i < 4; ++i)
					c.get<std::uint16_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					c.get<std::uint64_t>();
					const std::uint16_t len = c.get<std::uint16_t>();
					for (std::uint16_t k = 0; k < len; ++k)
						c.get<std::uint8_t>();
					const std::uint16_t m = c.get<std::uint16_t>();
					for (std::uint16_t k = 0; k < m; ++k)
						c.get<std::uint64_t>();
				}
				s.money = c.get<std::int64_t>();
				test::require(c.done(), "PLAYER_SQUADS has trailing bytes after money");
				squads.push_back(s);
				break;
			}
			case ks::kEvent:
			{
				c.get<std::uint64_t>();
				Ev e;
				e.kind = c.get<std::uint16_t>();
				if (e.kind == 13)
				{
					e.a = c.get<std::uint64_t>();
					e.b = c.get<std::uint64_t>();
					e.action = c.get<std::uint8_t>();
					e.result = c.get<std::uint8_t>();
				} else if (e.kind == 14) {
					e.a = c.get<std::uint64_t>();
					e.b = c.get<std::uint64_t>();
					e.text = text(c.get<std::uint32_t>());
				} else {
					break;
				}
				events.push_back(e);
				break;
			}
			default:
				break;
			}
		}
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

	// Sends an order; "" when applied (event 9), else the refusal reason of event 10.
	std::string send(const Buf& body, std::uint16_t kind)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
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
				return kindOf == 10 ? text(c.get<std::uint32_t>()) : std::string();
			}
		}
		throw test::Failure("an order got no answer");
	}
	std::string use(std::uint64_t who, std::uint64_t target)
	{
		Buf b;
		b.put<std::uint16_t>(1).put<std::uint64_t>(who).put<std::uint64_t>(target).put<std::uint8_t>(4).put<std::uint8_t>(0);
		return send(b, 60);
	}
	std::string transfer63(std::uint64_t who, std::uint64_t box, const std::string& item, std::uint16_t count, bool take)
	{
		Buf b;
		b.put<std::uint64_t>(who).put<std::uint64_t>(box).str(item).put<std::uint16_t>(count).put<std::uint8_t>(take ? 1 : 0);
		return send(b, 63);
	}
	std::string transfer68(std::uint64_t who, std::uint64_t box, std::uint32_t version, std::uint16_t index, std::uint16_t count, bool take)
	{
		Buf b;
		b.put<std::uint64_t>(who).put<std::uint64_t>(box).put<std::uint32_t>(version).put<std::uint16_t>(index).put<std::uint16_t>(count)
			.put<std::uint8_t>(take ? 1 : 0).put<std::uint8_t>(255).put<std::uint8_t>(0).put<std::uint8_t>(0);
		return send(b, 68);
	}

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }

	flecs::entity producerEntity(std::uint64_t id)
	{
		flecs::entity found;
		sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Producer&) { if (i.value == id) found = e; });
		return found;
	}
	const state::Producer* producer(std::uint64_t id)
	{
		const flecs::entity e = producerEntity(id);
		return e.is_valid() ? e.get<state::Producer>() : nullptr;
	}
	state::Producer* producerMut(std::uint64_t id) { return const_cast<state::Producer*>(producer(id)); }
	std::uint32_t stock(std::uint64_t id, const std::string& item)
	{
		const flecs::entity e = producerEntity(id);
		return e.is_valid() ? e.get<state::Inventory>()->carried(item) : 0;
	}
	std::uint32_t carried(std::uint64_t who, const std::string& item)
	{
		std::uint32_t n = 0;
		sim.world().ecs().each([&](const state::Id& i, const state::Inventory& v) { if (i.value == who) n = v.carried(item); });
		return n;
	}
	bool hasUseTask(std::uint64_t who)
	{
		bool found = false;
		sim.world().ecs().each([&](const state::Id& i, const state::InteractTask&) { if (i.value == who) found = true; });
		return found;
	}
	void teleport(std::uint64_t who, double x, double y, double z)
	{
		state::Fighter& f = fighter(who);
		f.x = x;
		f.y = y;
		f.z = z;
		f.clearRoute();
	}
	std::size_t events13(std::uint64_t target, std::uint8_t result) const
	{
		return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
			[&](const Ev& e) { return e.kind == 13 && e.b == target && e.action == 4 && e.result == result; }));
	}
};

struct Deposit
{
	ProducerSite site;
	const producers::Spec* spec = nullptr;
	double useX = 0, useY = 0, useZ = 0;
	std::string name;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_producers HUB.knav");
		test::require(kSaveVersion == 136, "the save version is 136 (v136 task ds/k16.6.457: CrimeRecord::announced, the EVENT 51 sent bit; v135 task ds/k16.6.730: Fighter::machineUse; v134 task ds/k16.7.156: PlayerBuilding::placement; v133 task ds/rmw6: Fighter::offPath; v132 task ds/k16.17.13.14: Fighter::age, a squad animal's rolled age; v131 task ds/k16.6.514: WorldOptions::feedAnimals and shareFood; v130 task ds/k16.6.637: Fighter due-stamps; v129 task ds/e9s9.10: RunStates::FarOrder cur / onMarker and FarMarker width; v128 task ds/e9s9.4: Fighter::portals / portalNext / vx / vz, the route corridor; v127 reserved for k16.17.13.14; v126 task ds/k16.6.614: RunStates promotion work in flight; v118 task ds/k16.6-fought-saved: Fighter::fought, the has-fought mark; v117 task ds/slice-load-parity: BoardStates occupancy map and occupyTown; v116 task ds/k16.6.483: RunStates::farOrders, the leg-by-leg far move orders; v115 reserved for k16.6.12.10; v114 task ds/k16.8.50: Fighter::pathSamples, the pair-distance cache; v113 task ds/perception-aim-xp: FighterSkills::perception; v112 task ds/passive-hold: Fighter::passive and Fighter::holdPosition; v111 task ds/drive-strafe: Fighter::driveStrafe; v110 task ds/unsaved-audit: state::RunStates, the limb play, alarm board and alarm wait; v109 task ds/save-walk-arrest: Fighter::walkHeading / walkFacing and ArrestStates; v108 task ds/container-piece: Container building / piece; v107 task ds/slave-timers: Fighter::slaveStamp; v106 task ds/spectate-stagger: Inventory slot kSheathed, the weapon on the back; v105 task ds/save-import3b: TownState::info, ExtraTown, FactionState rank / trust, WorldEventState used; v104 task ds/limb-tint: GroundItem tint, a severed limb's skin, blood and scale; v103 task ds/save-import2: MemberDelta look block, the name and sex of an imported member; v102 task ds/stragglers: state::StragglerOf, the link of a member split off its squad at demotion; v101 task ds/member-slot: state::MemberSlot, the build slot fixed at spawn; v100 task ds/corpses: CorpseDecay deathTick / cause and MemberDelta kit, the off-screen death and the stored medic's kit; v99 task ds/delta-store: state::MemberDelta, the per-member delta kept across demotion; v98 task ds/stealth-ko: Fighter::stealthKo and FighterSkills::thievery / assassination, the stealth KO under way; v97 task ds/turret: Fighter::mount and FighterSkills::turrets, the turret a gunner mans; v96 task ds/ranged: Fighter::ranged and FighterSkills::crossbows, the carried crossbow; v95 task ds/slaver-deliver: BoardStates owned / depot, the ownership set and the slaver depot; v94 task ds/board-pick: BoardStates startedAt / traderState / occupyHeld; v93 task ds/town-port: state::TalkHold, the character the player talked to stands still; v92 the Board save; v91 task ds/kidnap: Fighter::captive; v90 task ds/arrest-2: state::CropPlot, the farm crop growth and ripe food; v89 task ds/provenance: InventoryEntry planted / stolen, GroundItem::stolen; v88 task ds/construction: PlayerBuilding ruin / dismantled / furniture / mounted, FighterSkills::engineer; v87 task locks-save: state::LockStates, the picked / cut cage and shackle locks; v86 task CH group 3: Routine::shopping / shoppingDay / shoppingTimer / shoppingLatch / shoppingShop / shoppingX-Z, the shopping list and the 119 timer; v85 task indoors: Fighter::indoors, the interior-and-ceiling state; v84 task CH: state::TakeOut plan / step / liftOrdered / dropSet, the intruder eviction's G4 chain; v83 task patrol: the 216 town search; v82 task CB: Routine::plan / step, the plan the slaver chain runs; v81 task CC: Fighter::diceSeed, the stable combat dice identity; v80 unused); v79 Fighter::attackType, the animal technique family (task BQ); v78 task BZ: Fighter::aggressorSquads, the squad map's AGGRESSOR bit that goal 34 reads; v77 state::WorldOptions, the new-game cod / gdm / ht (task BT); v76 Fighter::restYaw / restRolls / restYawValid, the ragdoll rest pose (task BG); v75 Fighter::met, the dialogue memory (task BB); v74 Fighter::combatGoal (task AT, the goal that started a fight); v73 CaravanOrder::postedAt (T4d, the delivery order's age); v72 state::WorldEventState dead + playerTouched (task AN); v71 state::Container key + lock (task AL); v70 FactionState::cash (the dialogue economy's per-faction money); v69 the real-economy overlay: EconomySettings, TownStock, EconomyProducer, CaravanOrder, SupplyBoard; v68 the slave trade: the captivity fields and the chain; v67 Fighter::aimYaw; v66 Fighter::hitList / holdSteer; v65 Fighter::moveMode / speedGroup; v64 Conversation::replies / menuTick; v63 state::ItemNode; v62 GroundItem::ownerSid; v61 Fighter::goal; v60 state::BuildingLooks; v59 SquadInstance::homeBuilding; v58 state::DialogueMemory, Conversation::follower; v57 state::Conversation / DialogueShown; v56 ItemQuality::uniform; v55 producer collision bounds");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const producers::Book book(*gd);
		const grid::Rules rules(*gd);

		// ---- the rules, from the merged data ----
		{
			const kdata::Record* ironResource = nullptr;
			const kdata::Record* oreMine = nullptr;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("BUILDING")))
			{
				ironResource = record->name == "Iron Resource" ? record : ironResource;
				oreMine = record->name == "Ore Mine" ? record : oreMine;
			}
			test::require(ironResource != nullptr && oreMine != nullptr, "the data has Iron Resource and Ore Mine");
			const producers::Spec* a = book.find(ironResource->stringID);
			const producers::Spec* b = book.find(oreMine->stringID);
			test::require(a != nullptr && b != nullptr, "both are producers: wild deposit and built mine go through one path");
			test::require(a->output == b->output && a->cap == 5 && b->cap == 5, "the same `produces` and the cap 5 (v0)");
			test::require(a->maxOperators == 3 && b->maxOperators == 3, "max operators 3 from the BUILDING, not the functionality's 6");
			test::require(a->statUsed == 3 && !a->progressBar && !b->progressBar, "stat used 3 (Labouring); no progress bar (MQ9)");
			const double perDay = a->productionMult * a->outputRate / 3.0;
			test::require(std::fabs(producers::baseProgressPerTick(*a) * kTicksPerGameDay - perDay) < 1e-9, "items per game day = production mult x output rate / max operators");
			test::require(std::fabs(producers::skillMult(100.0) - 1.4) < 1e-12 && std::fabs(producers::skillMult(0.0) - 0.5) < 1e-12, "skill mult 0.5 .. 1.4");
			// Live check (kinspect item 59): copper, Labouring 3, richness 0.8, rate 36 a day -> 15.2 a day
			producers::Spec copperLike = *a;
			copperLike.productionMult = 72.0;
			copperLike.outputRate = 1.0;
			copperLike.maxOperators = 2;
			state::FighterSkills skills;
			skills.labouring = 3.0;
			test::require(std::fabs(producers::operatorProgress(copperLike, 0.8, skills) * kTicksPerGameDay - 15.2) < 0.05, "kinspect's live figure: 15.2 a day");
			std::size_t small = 0;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("BUILDING")))
				small += record->name.rfind("Small ", 0) == 0 && book.find(record->stringID) != nullptr ? 1 : 0;
			test::require(small >= 2, "the Small Copper / Iron Resource variants are producers through the data");
			std::cout << "spec ok\n";
		}

		Rig r(order, gd, data, nav);
		const std::uint64_t p1 = r.squad.players[0];
		const std::uint64_t p2 = r.squad.players[1];

		// ---- money on PLAYER_SQUADS ----
		{
			test::require(!r.squads.empty(), "PLAYER_SQUADS came on connect (with the trailing money)");
			const std::size_t before = r.squads.size();
			r.tick(5);
			test::require(r.squads.size() == before, "unchanged money sends nothing");
			flecs::entity faction;
			r.sim.world().ecs().each([&](flecs::entity e, const state::PlayerFaction&) { faction = e; });
			// a copy + set, as the sim writes it: an in-place write skips the OnSet that refreshes the squad cache (k16.6.279)
			const auto setMoney = [&](std::int64_t money)
			{
				state::PlayerFaction held = *faction.get<state::PlayerFaction>();
				held.money = money;
				faction.set<state::PlayerFaction>(std::move(held));
			};
			setMoney(4321);
			r.tick(2);
			test::require(r.squads.size() == before + 1 && r.squads.back().money == 4321, "a money change sends PLAYER_SQUADS with the new i64");
			setMoney(0);
			r.tick(2);
			test::require(r.squads.size() == before + 2 && r.squads.back().money == 0, "and back");
			std::cout << "money ok\n";
		}

		// ---- a wild iron and a wild copper deposit on the Hub navmesh ----
		const kworld::ZoneCell hub = kworld::zoneCellOf(ks::kHubX, ks::kHubZ);
		Deposit iron, copper;
		for (int ring = 0; ring <= 3 && (iron.spec == nullptr || copper.spec == nullptr); ++ring)
		{
			for (int cz = hub.z - ring; cz <= hub.z + ring; ++cz)
			{
				for (int cx = hub.x - ring; cx <= hub.x + ring; ++cx)
				{
					if (std::max(std::abs(cx - hub.x), std::abs(cz - hub.z)) != ring)
						continue;
					for (const ProducerSite& site : r.sim.wildProducerSites(cx, cz))
					{
						const producers::Spec* spec = book.find(site.recordSid);
						Deposit& slot = spec->resource == 1 ? iron : copper;
						if ((spec->resource != 1 && spec->resource != 3) || slot.spec != nullptr)
							continue;
						const auto spot = nav->walker().snap({site.x, site.y, site.z}, producers::kUseSnap);
						if (!spot)
							continue;
						slot = {site, spec, spot->x, spot->y, spot->z, gd->find(site.recordSid)->name};
					}
				}
			}
		}
		test::require(iron.spec != nullptr && copper.spec != nullptr, "a wild iron and a wild copper deposit on the Hub navmesh within 3 zone cells of the Hub");
		std::cout << "iron   " << iron.name << " #" << iron.site.id << " at " << iron.site.x << "," << iron.site.z << " ops " << iron.spec->maxOperators << "\n";
		std::cout << "copper " << copper.name << " #" << copper.site.id << " at " << copper.site.x << "," << copper.site.z << " ops " << copper.spec->maxOperators << "\n";

		// A third player character (a copy of the first), for the slot test.
		state::Fighter clone = r.fighter(p1);
		const std::uint64_t p3 = r.sim.spawnFighter(clone).get<state::Id>()->value;
		r.context.looks[p3] = r.context.looks[p1];
		r.tick(2);
		const std::array<std::uint64_t, 3> crew = {p1, p2, p3};

		// The same function for both deposits.
		const auto work = [&](const Deposit& d)
		{
			const std::uint64_t id = d.site.id;
			const std::string item = d.spec->output;
			const grid::Size slot = rules.footprint(item);
			bool anyNear = false;
			for (const std::uint64_t who : crew)
				anyNear = anyNear || std::hypot(r.fighter(who).x - d.site.x, r.fighter(who).z - d.site.z) <= producers::kMaterialiseRadius;
			if (!anyNear)
				test::require(r.producer(id) == nullptr, d.name + ": not materialised while nobody is near");
			for (std::size_t k = 0; k < crew.size(); ++k)
			{
				r.teleport(crew[k], d.useX + 1.0 + static_cast<double>(k), d.useY, d.useZ);
				r.fighter(crew[k]).skills.labouring = 50.0;
			}
			r.tick(45);
			const state::Producer* p = r.producer(id);
			test::require(p != nullptr, d.name + ": materialised near the players");
			test::require(p->prodState == 3 && p->operators.empty() && p->progress == 0.0 && p->output == item && p->cap == d.spec->cap
				&& p->maxOperators == d.spec->maxOperators, d.name + ": idle, empty, the data's output, cap and operators");
			test::require(p->richness >= 0.1, d.name + ": richness is the zone's, floor 0.1");

			// ---- M3 on the wire: kind 4 and its tail, the always-on contents ----
			const auto seen = r.inter.find(id);
			test::require(seen != r.inter.end(), d.name + ": INTERACTABLE sent");
			test::require(seen->second.kind == ks::kInteractableProducer && seen->second.record == d.site.recordSid && seen->second.state == 3
				&& seen->second.count == 0 && seen->second.health == 0 && seen->second.maxHealth == 0, d.name + ": kind 4 record");
			test::require(seen->second.output == item && seen->second.cap == d.spec->cap && seen->second.operators == 0 && seen->second.maxOperators == d.spec->maxOperators,
				d.name + ": the tail names the output item, the cap and the operators");
			const Inv& held = r.inv.at(id);
			test::require(held.items.empty() && held.grids.size() == 1 && held.grids[0] == std::array<int, 3>{0, slot.w, slot.h} && held.gridVersion == held.version,
				d.name + ": INVENTORY + ITEM_GRID: one section 0 of the item's footprint, empty");

			test::require(seen->second.rate == 0.0f && seen->second.progress == 0.0f && seen->second.progressTick > 0, d.name + ": idle: progress 0, rate 0, a progressTick");
			test::require(d.spec->animation == "stonemine", d.name + ": the operate clip is the data's (an empty ref falls back to stonemine)");
			test::require(r.dyn.at(crew[0]).action.empty() && r.dyn.at(crew[0]).actionFlags == 0, d.name + ": not operating: no action, flags 0");

			// ---- M2: operate ----
			const std::size_t crewSize = static_cast<std::size_t>(std::min(d.spec->maxOperators, 3));
			// MQ16 facing: the operator turns to the building origin, at no more than 20 rad/s a tick, and ends on it (not a snap).
			const state::Producer* origin = r.producer(id);
			r.teleport(crew[0], d.useX + 10.0, d.useY, d.useZ); // the use point is the origin: stand off it, inside the use range
			r.tick(1);
			const double bearing = std::atan2(origin->x - r.fighter(crew[0]).x, origin->z - r.fighter(crew[0]).z);
			test::require(std::hypot(origin->x - r.fighter(crew[0]).x, origin->z - r.fighter(crew[0]).z) > 1.0, d.name + ": the operator stands off the origin");
			r.fighter(crew[0]).yaw = std::remainder(bearing + 2.5, 2.0 * 3.14159265358979323846); // 2.5 rad off the origin
			test::require(r.use(crew[0], id).empty(), d.name + ": use accepted");
			double facingStep = 0.0;
			for (int t = 0; t < 6; ++t)
			{
				const double before = r.fighter(crew[0]).yaw;
				r.tick(1);
				facingStep = std::max(facingStep, std::fabs(std::remainder(r.fighter(crew[0]).yaw - before, 2.0 * 3.14159265358979323846)));
			}
			test::require(std::fabs(std::remainder(r.fighter(crew[0]).yaw - bearing, 2.0 * 3.14159265358979323846)) < 0.02,
				d.name + ": the operator faces the building origin after 6 ticks (off by " + std::to_string(std::remainder(r.fighter(crew[0]).yaw - bearing, 2.0 * 3.14159265358979323846)) + ")");
			test::require(facingStep <= 20.0 * kTickSeconds + 1e-9 && facingStep > 0.2, d.name + ": the facing turns at the normal rate, not a snap (largest tick " + std::to_string(facingStep) + ")");
			const Inter one = r.inter.at(id);
			test::require(one.rate > 0.0f && std::fabs(one.rate - r.sim.producerRate(*r.producer(id))) < 1e-4f * one.rate + 1e-7f, d.name + ": one operator: rate is the producer's");
			test::require(r.dyn.at(crew[0]).action == d.spec->animation && r.dyn.at(crew[0]).actionFlags == 1, d.name + ": the operator plays the mine's clip, loop bit set");
			const std::uint64_t operateStart = r.dyn.at(crew[0]).actionStart;
			test::require(operateStart > 0 && operateStart <= one.progressTick, d.name + ": actionStart is the tick the operate task began");
			test::require(r.dyn.at(crew[1]).action.empty() && r.dyn.at(crew[1]).actionFlags == 0, d.name + ": a bystander has no action");
			{
				// the client's extrapolation: progress + rate x (now - progressTick), now a few ticks later
				r.tick(10);
				const double predicted = one.progress + static_cast<double>(one.rate) * static_cast<double>(r.sim.clockTick() - one.progressTick) * kTickSeconds;
				test::require(r.inter.at(id).version == one.version && std::fabs(r.producer(id)->progress - predicted) < 0.02 * std::max(predicted, 0.05) + 1e-4,
					d.name + ": progress grows by about rate x dt between two dumps (" + std::to_string(r.producer(id)->progress) + " vs " + std::to_string(predicted) + ")");
				test::require(r.dyn.at(crew[0]).actionStart == operateStart, d.name + ": actionStart is stable while operating");
			}
			for (std::size_t k = 1; k < crewSize; ++k)
				test::require(r.use(crew[k], id).empty(), d.name + ": use accepted");
			r.tick(6);
			if (crewSize >= 2)
			{
				test::require(r.inter.at(id).version > one.version && r.inter.at(id).rate > one.rate * 1.5f, d.name + ": a second operator raises the rate and the record is sent again");
				test::require(r.dyn.at(crew[0]).actionStart == operateStart, d.name + ": and the first operator's actionStart does not move");
			}
			p = r.producer(id);
			test::require(p->operators.size() == crewSize && p->prodState == 2, d.name + ": the crew works it");
			test::require(r.inter.at(id).operators == crewSize && r.inter.at(id).state == 2, d.name + ": the wire follows the operator count and state");
			if (d.spec->maxOperators <= 2)
			{
				test::require(r.use(p3, id) == "no_slot", d.name + ": all slots taken is no_slot");
			}
			// TASK_PROGRESS: none for a producer whose functionality has no bar (MQ9); kind 9 when it has one
			test::require(std::none_of(r.tasks.begin(), r.tasks.end(), [](const Task& t) { return t.kind == 9; }), d.name + ": no kind 9 without the functionality flag");
			r.producerMut(id)->progressBar = true;
			r.tick(2);
			test::require(r.tasks.size() == crewSize && r.tasks[0].kind == 9, d.name + ": kind 9 rows with the flag set");
			{
				const state::Producer* now = r.producer(id);
				test::require(std::fabs(r.tasks[0].rate - r.sim.producerRate(*now)) < 1e-5 && r.tasks[0].rate > 0.0f && r.tasks[0].progress > 0.0f
					&& std::fabs(r.tasks[0].progress - now->progress) < 0.02, d.name + ": kind 9 carries the producer's progress and rate");
			}
			r.producerMut(id)->progressBar = false;
			r.tick(2);
			test::require(r.tasks.empty(), d.name + ": and the bar goes with the flag");

			// the first unit arrives when the shared progress reaches 0.999
			const double p0 = r.producer(id)->progress;
			double dp = 0.0;
			for (std::size_t k = 0; k < crewSize; ++k)
				dp += producers::operatorProgress(*d.spec, r.producer(id)->richness, r.fighter(crew[k]).skills);
			const double expectTicks = (0.999 - p0) / dp;
			const double skillBefore = r.fighter(p1).skills.labouring;
			int ticks = 0;
			while (r.stock(id, item) == 0 && ticks < 200000)
			{
				r.sim.run(1);
				++ticks;
			}
			r.tick(1);
			test::require(r.stock(id, item) == 1 && std::fabs(ticks - expectTicks) <= 2.0 + expectTicks * 0.01,
				d.name + ": first unit after " + std::to_string(ticks) + " ticks, expected " + std::to_string(expectTicks));
			test::require(r.fighter(p1).skills.labouring > skillBefore, d.name + ": Labouring trains");
			test::require(r.inter.at(id).count == 1 && r.inv.at(id).items.size() == 1 && r.inv.at(id).items[0].second == 1, d.name + ": the unit is in the output (wire)");

			// ---- the cap: result 7 for every operator, the stack stays one entry up to the cap ----
			int guard = 0;
			while (r.producer(id)->prodState != 1 && guard++ < 400000)
			{
				r.sim.run(20);
				if (r.stock(id, item) < d.spec->cap)
					test::require(r.producer(id)->operators.size() == crewSize, d.name + ": the crew keeps working below the cap");
			}
			r.tick(2);
			test::require(r.stock(id, item) == d.spec->cap && r.producer(id)->prodState == 1 && r.producer(id)->operators.empty() && r.producer(id)->progress == 0.0,
				d.name + ": full at the cap, nobody operating, progress 0");
			test::require(r.events13(id, 7) == crewSize, d.name + ": event 13 action 4 result 7 for each operator");
			for (std::size_t k = 0; k < crewSize; ++k)
				test::require(!r.hasUseTask(crew[k]), d.name + ": the order ended");
			test::require(r.inter.at(id).state == 1 && r.inter.at(id).count == d.spec->cap && r.inter.at(id).operators == 0, d.name + ": the wire says full");
			test::require(r.inter.at(id).rate == 0.0f && r.inter.at(id).progress == 0.0f, d.name + ": full: rate 0, progress 0");
			for (std::size_t k = 0; k < crewSize; ++k)
				test::require(r.dyn.at(crew[k]).action.empty() && r.dyn.at(crew[k]).actionFlags == 0, d.name + ": the operate action clears when the task ends");
			test::require(r.inv.at(id).items.size() == 1 && r.inv.at(id).items[0].second == d.spec->cap && r.inv.at(id).cells.size() == 1
				&& r.inv.at(id).cells[0] == std::array<int, 4>{0, 0, 0, 0}, d.name + ": one stack in the one slot");
			test::require(r.use(p1, id) == "full", d.name + ": a new use is refused `full`");
			r.tick(60);
			test::require(r.stock(id, item) == d.spec->cap, d.name + ": nothing overflows, nothing more is made");

			// ---- taking: 63 by record, theft only for an owned producer, 68 by entry; putting in and rearranging refused ----
			test::require(r.transfer63(p1, id, item, 2, true).empty(), d.name + ": order 63 take accepted");
			r.tick(3);
			test::require(r.stock(id, item) == d.spec->cap - 2 && r.carried(p1, item) >= 2, d.name + ": 63 moved two into the character");
			test::require(r.producer(id)->prodState == 3 && r.inter.at(id).state == 3, d.name + ": below the cap it is idle again");
			// ---- putting back (kenshi-k16.19.13): the produced item while it fits; a foreign item or one over the cap is refused ----
			{
				const std::string& foreign = (&d == &iron ? copper : iron).spec->output;
				const auto putBack68 = [&](std::uint32_t count)
				{
					const Inv& pack = r.inv.at(p1);
					for (std::size_t k = 0; k < pack.items.size(); ++k)
					{
						if (pack.items[k].first == item && pack.items[k].second >= count)
							return r.transfer68(p1, id, r.inv.at(p1).version, static_cast<std::uint16_t>(k), static_cast<std::uint16_t>(count), false);
					}
					return std::string("no such entry");
				};
				test::require(r.transfer63(p1, id, foreign, 1, false) == "not_allowed", d.name + ": a foreign item into a producer (63 take 0) is not_allowed");
				const Inv& pack = r.inv.at(p1);
				std::size_t foreignAt = pack.items.size();
				for (std::size_t k = 0; k < pack.items.size(); ++k)
					foreignAt = pack.items[k].first != item && foreignAt == pack.items.size() ? k : foreignAt;
				if (foreignAt < pack.items.size())
					test::require(r.transfer68(p1, id, r.inv.at(p1).version, static_cast<std::uint16_t>(foreignAt), 1, false) == "not_allowed", d.name + ": a foreign item (68 take 0) is not_allowed");
				{
					// the pack must have room: keep one stack of the other deposit's ore, drop the rest
					r.sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&)
					{
						if (i.value != p1)
							return;
						state::Inventory lean = *e.get<state::Inventory>();
						bool kept = false;
						std::erase_if(lean.items, [&](const state::Inventory::Entry& entry)
						{
							const bool drop = entry.itemSid == foreign && entry.slot == state::Inventory::kCarried && kept;
							kept = kept || entry.itemSid == foreign;
							return drop;
						});
						++lean.version;
						e.set<state::Inventory>(std::move(lean));
					});
					r.tick(2);
				}
				{
					// the pack must hold more than the room (a character that mined it all carries only what it took)
					const grid::Rules rules(*r.sim.gameData());
					r.sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&)
					{
						if (i.value != p1)
							return;
						state::Inventory more = *e.get<state::Inventory>();
						grid::autoplace(more, rules, state::Inventory::Entry{item, 3, 0, {}}); // what fits (the pack may be nearly full of the earlier deposit's ore)
						++more.version;
						e.set<state::Inventory>(std::move(more));
					});
					r.tick(2);
				}
				test::require(r.transfer63(p1, id, item, 1, true).empty(), d.name + ": take one more");
				r.tick(3);
				test::require(r.stock(id, item) == d.spec->cap - 3, d.name + ": cap - 3 left");
				test::require(r.transfer63(p1, id, item, 1, false).empty(), d.name + ": 63 put back one is accepted");
				r.tick(3);
				test::require(r.stock(id, item) == d.spec->cap - 2, d.name + ": the output count is back up by one");
				test::require(r.transfer63(p1, id, item, 3, false) == "full", d.name + ": more than the room is refused `full`");
				test::require(r.stock(id, item) == d.spec->cap - 2, d.name + ": a refused put changes nothing");
				test::require(r.transfer63(p1, id, item, 1, true).empty(), d.name + ": take one again");
				r.tick(3);
				test::require(putBack68(1).empty(), d.name + ": 68 put back one is accepted");
				r.tick(3);
				test::require(r.stock(id, item) == d.spec->cap - 2, d.name + ": 68 put back raises the same stack");
				test::require(r.transfer63(p1, id, item, 2, false).empty(), d.name + ": two more units fill it to the cap");
				r.tick(3);
				test::require(r.stock(id, item) == d.spec->cap, d.name + ": full again");
				const std::string over = putBack68(1);
				test::require(over == "no_room", d.name + ": 68 into a full output is refused `no_room`, got `" + over + "`");
				test::require(r.stock(id, item) == d.spec->cap, d.name + ": a refused 68 put changes nothing");
				test::require(r.transfer63(p1, id, item, 2, true).empty(), d.name + ": back to cap - 2 for the take-out below");
				r.tick(3);
			}
			{
				Buf b64;
				b64.put<std::uint64_t>(id).put<std::uint32_t>(r.inv.at(id).version).put<std::uint16_t>(0).put<std::uint8_t>(0).put<std::uint8_t>(0).put<std::uint8_t>(0);
				test::require(r.send(b64, 64) == "not_allowed", d.name + ": order 64 on a producer is not_allowed");
				Buf b65;
				b65.put<std::uint64_t>(id).put<std::uint8_t>(0);
				test::require(r.send(b65, 65) == "not_allowed", d.name + ": order 65 on a producer is not_allowed");
			}
			test::require(std::none_of(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 14 && e.b == id; }), d.name + ": no theft from a wild deposit");
			const std::uint32_t carriedBefore = r.carried(p1, item);
			r.fighter(p1).skills.thievery = 100.0; // a sure take from the owned store (steal chance 127%, never caught): ksim_steal_chance rolls it
			r.producerMut(id)->ownerSid = "1-owner.test";
			test::require(r.transfer68(p1, id, r.inv.at(id).version, 0, d.spec->cap - 2, true).empty(), d.name + ": order 68 take accepted");
			r.tick(3);
			test::require(r.stock(id, item) == 0 && r.carried(p1, item) == carriedBefore + d.spec->cap - 2, d.name + ": 68 moved the rest");
			test::require(std::count_if(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 14 && e.a == p1 && e.b == id && e.text == "1-owner.test"; }) == 1,
				d.name + ": theft (event 14) only for the owned producer");
			r.producerMut(id)->ownerSid.clear();
			test::require(r.inv.at(id).items.empty(), d.name + ": the slot is empty on the wire");
			test::require(!producerSaved(r.producerEntity(id)), d.name + ": an emptied producer is not saved");
			std::cout << d.name << " ok (" << crewSize << " operators)\n";
		};
		work(iron);
		work(copper);

		// ---- the focus panel's record (SIMLINK Order 87, a producer's id; kenshi-k16.19.14) ----
		{
			const std::uint64_t id = copper.site.id;
			const auto focus = [&](std::uint64_t target)
			{
				Buf b;
				b.put<std::uint64_t>(target);
				return r.send(b, 87);
			};
			const auto rowsOf = [&](std::uint64_t target)
			{
				return static_cast<std::size_t>(std::count_if(r.buildings.begin(), r.buildings.end(), [&](const Bld& b) { return b.id == target; }));
			};
			test::require(rowsOf(id) == 0, "no BUILDING row for the wild copper node before it is focused (machines and containers of any owner are sent since k16.5.26)");
			state::Producer* node = r.producerMut(id);
			test::require(node != nullptr, "the copper node is materialised");
			const double x = node->x;
			const double z = node->z;
			node->x = node->z = 1.0e7; // out of the detail bounds
			test::require(focus(id) == "bad_target", "a producer outside the detail bounds is bad_target");
			node->x = x;
			node->z = z;
			test::require(focus(~std::uint64_t(0) - 1) == "bad_target", "an id that is neither a character nor a producer is bad_target");
			r.tick(2);
			test::require(r.buildings.empty(), "a refused focus sends nothing");
			test::require(focus(id).empty(), "a producer id is accepted as the focus");
			r.tick(2);
			test::require(rowsOf(id) == 1, "focus: one BUILDING row for the node (" + std::to_string(rowsOf(id)) + ")");
			Bld row = r.buildings.back();
			const state::Producer* p = r.producer(id);
			test::require(row.record == copper.site.recordSid && row.owner == 0 && (row.flags & 1) != 0, "wild (owner 0), built, the node's BUILDING record");
			test::require(std::fabs(row.quality - static_cast<float>(p->richness)) < 1e-6f, "quality is the node's richness");
			test::require(row.hp > 0.0f && row.hp == row.maxHp && row.progress == 1.0f && row.rate == 0.0f, "Condition from the BUILDING record's HP; progress 1, rate 0");
			test::require(row.workers == p->operators.size() && row.maxWorkers == p->maxOperators, "workers are the operators, max the node's");
			const float most = static_cast<float>(p->richness * copper.spec->outputRate);
			test::require(std::fabs(row.maxEfficiency - most) < 1e-6f, "max efficiency = richness x the BUILDING's output rate x research 1.0");
			test::require(std::fabs(row.efficiency - most * std::min(1.0f, static_cast<float>(row.workers) / row.maxWorkers)) < 1e-6f, "efficiency scales with the worker fraction");
			test::require(row.status == (p->prodState == 1 ? 5 : p->prodState == 2 ? 0 : 6), "status follows the producer state");
			std::cout << "producer focus: " << copper.name << " quality " << row.quality << " workers " << int(row.workers) << "/" << int(row.maxWorkers) << " efficiency "
				<< row.efficiency << " max " << row.maxEfficiency << " hp " << row.hp << "/" << row.maxHp << " status " << int(row.status) << "\n";
			// an unchanged node resends nothing; a change of the crew sends one record
			const std::size_t heard = r.buildings.size();
			r.tick(5);
			test::require(r.buildings.size() == heard, "unchanged: nothing more");
			const std::size_t before = p->operators.size();
			p = nullptr;
			std::uint64_t mover = 0;
			for (const std::uint64_t who : crew)
				mover = mover == 0 && std::find(r.producer(id)->operators.begin(), r.producer(id)->operators.end(), who) != r.producer(id)->operators.end() ? who : mover;
			if (mover != 0) // an operator walks off
			{
				Buf b;
				b.put<std::uint16_t>(1).put<std::uint64_t>(mover).put<float>(static_cast<float>(x) + 300.0f).put<float>(static_cast<float>(z)).put<float>(0.0f).put<std::uint8_t>(0);
				test::require(r.send(b, 1).empty(), "move order accepted");
			} else {
				for (const std::uint64_t who : crew)
					if (mover == 0 && r.use(who, id).empty())
						mover = who;
			}
			r.tick(40);
			test::require(r.producer(id)->operators.size() != before, "the crew changed");
			test::require(r.buildings.size() > heard && r.buildings.back().id == id && r.buildings.back().workers == r.producer(id)->operators.size()
				&& r.buildings.back().version != row.version, "a crew change sends a changed record with the new worker count");
			// a character focus replaces it, and stops the node's records; 87 0 clears
			test::require(focus(p1).empty(), "a character focus still works");
			const std::size_t quiet = r.buildings.size();
			r.tick(40);
			test::require(r.buildings.size() == quiet, "the character focus replaced the node: no more BUILDING rows");
			test::require(focus(id).empty() && focus(0).empty(), "focus the node, then 87 0");
			const std::size_t cleared = r.buildings.size();
			r.tick(40);
			test::require(r.buildings.size() == cleared, "87 0: nothing more");
			std::cout << "producer focus ok\n";
		}

		// ---- save v49 mid-progress, load, continue ----
		{
			const std::uint64_t id = copper.site.id;
			const std::string item = copper.spec->output;
			r.tick(40);
			test::require(r.producer(id) != nullptr, "the copper producer is still materialised");
			const std::size_t crewSize = static_cast<std::size_t>(std::min(copper.spec->maxOperators, 3));
			for (std::size_t k = 0; k < crewSize; ++k)
				test::require(r.use(crew[k], id).empty(), "use accepted");
			int guard = 0;
			while ((r.stock(id, item) < 2 || r.producer(id)->progress < 0.3) && guard++ < 200000)
				r.sim.run(1);
			test::require(r.stock(id, item) == 2 && r.producer(id)->progress >= 0.3 && r.producer(id)->operators.size() == crewSize, "the save point is mid-progress with two units");
			test::require(producerSaved(r.producerEntity(id)), "a producer with stock and progress is saved");
			std::ostringstream saved;
			r.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, r.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == r.sim.stateHash(), "the hash changed across save and load");
			flecs::entity back;
			loaded.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Producer&) { if (i.value == id) back = e; });
			test::require(back.is_valid() && back.get<state::Producer>()->progress == r.producer(id)->progress
				&& back.get<state::Producer>()->operators == r.producer(id)->operators, "the producer's progress and operators are in the save");
			r.sim.run(400);
			loaded.run(400);
			test::require(loaded.stateHash() == r.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			const std::uint32_t units = r.stock(id, item);
			r.sim.run(2000);
			loaded.run(2000);
			test::require(loaded.stateHash() == r.sim.stateHash() && r.stock(id, item) >= units, "and still the same after the next units");
			std::cout << "save ok\n";
		}

		// ---- reach from a distance (user, 2026-11-03): from the Hub's front gate every deposit nearby routes and can be worked, the iron
		// deposit too, whose use range (50) to the origin lies inside its own rock: the approach point and the bounds x 1.2 (item59 MQ9) ----
		{
			const auto gate = nav->walker().snap({-51652.0, 1558.0, 2759.0}, 200.0);
			test::require(gate.has_value(), "the front gate pose is on the Hub navmesh");
			std::vector<std::uint64_t> near;
			for (const std::uint64_t who : crew)
				r.teleport(who, gate->x, gate->y, gate->z);
			r.tick(25);
			r.sim.world().ecs().each([&](const state::Id& id, const state::Producer& p)
			{
				if (std::hypot(p.x - gate->x, p.z - gate->z) > producers::kMaterialiseRadius)
					return;
				const InteractTarget target = findInteractTarget(r.sim.world().ecs(), r.sim.combat(), id.value);
				const Route route = routeToInteract(*nav, r.fighter(p1), target, static_cast<std::uint8_t>(InteractAction::Use), false,
					r.sim.pathRadius(r.fighter(p1)));
				// The clearance radius pushes a route's end off the walls (radius + 0.5), so a use point beside the deposit is reached
				// by an approach route that stops short of it: the arrival test is the end within the producer's reach.
				const bool inReach = route.found && !route.points.empty()
					&& target.nearEnough(route.points.back().x, route.heights.back(), route.points.back().z, target.reachStopped);
				std::cout << "from the gate: " << p.recordSid << " #" << id.value << " origin " << p.x << "," << p.z << " range " << p.useRange
					<< " box " << p.boxMaxX - p.boxMinX << " x " << p.boxMaxZ - p.boxMinZ << ": route " << (inReach ? (route.approach ? "in reach (approach)" : "found") : "NONE") << "\n";
				test::require(inReach, p.recordSid + ": routes from the front gate");
				test::require(p.boxMaxX > p.boxMinX && p.boxMaxZ > p.boxMinZ, p.recordSid + ": a foliage deposit has its rock's bounds");
				near.push_back(id.value);
			});
			test::require(!near.empty(), "deposits near the front gate");
			for (const std::uint64_t id : near)
			{
				for (const std::uint64_t who : crew)
					r.teleport(who, gate->x, gate->y, gate->z);
				r.tick(2);
				const state::Producer* p = r.producer(id);
				if (p == nullptr || p->prodState == 1)
					continue; // filled earlier in this test
				test::require(r.use(p1, id).empty(), p->recordSid + ": use from the gate accepted");
				bool working = false;
				for (int t = 0; t < 2400 && !working; ++t)
				{
					r.tick(1);
					const state::Producer* now = r.producer(id);
					working = now != nullptr && std::find(now->operators.begin(), now->operators.end(), p1) != now->operators.end();
				}
				const state::Fighter& f = r.fighter(p1);
				std::cout << "  walked to " << r.producer(id)->recordSid << ": " << (working ? "operating" : "NOT operating") << " at d "
					<< std::hypot(f.x - r.producer(id)->x, f.z - r.producer(id)->z) << "\n";
				test::require(working, r.producer(id)->recordSid + ": walked from the gate and operates");
			}
		}
		// ---- the operate range (kenshi-k16.19.5): no stopped hysteresis. Ordered from 150 u the miner mines only once inside the use range;
		// moved 120 u out (past 100, under the old 200) it leaves its slot and walks back ----
		{
			const std::uint64_t id = copper.site.id;
			const state::Producer* p = r.producer(id);
			test::require(p != nullptr, "the copper producer is materialised");
			const double ox = p->x;
			const double oz = p->z;
			const double range = p->useRange;
			const auto operating = [&] { const state::Producer* q = r.producer(id); return q != nullptr && std::find(q->operators.begin(), q->operators.end(), p1) != q->operators.end(); };
			const auto away = [&](double distance)
			{
				const auto spot = nav->nearestWalkable(ox, oz - distance, 30.0);
				test::require(spot.has_value(), "walkable ground " + std::to_string(distance) + " u from the copper origin");
				r.teleport(p1, spot->x, r.fighter(p1).y, spot->z);
			};
			for (const std::uint64_t who : crew)
				r.teleport(who, ox, p->useY, oz - 400.0);
			r.tick(25);
			test::require(r.producer(id)->operators.empty() && r.producer(id)->prodState != 1, "the copper producer is idle and not full");
			away(150.0);
			r.tick(1);
			test::require(r.use(p1, id).empty(), "use accepted from 150 u");
			double firstDistance = -1.0;
			for (int t = 0; t < 1200 && !operating(); ++t)
				r.tick(1);
			test::require(operating(), "ordered from 150 u, the miner reaches the copper and works it");
			firstDistance = std::hypot(r.fighter(p1).x - ox, r.fighter(p1).z - oz);
			std::cout << "range: mining began " << firstDistance << " u from the origin (use range " << range << ")\n";
			test::require(firstDistance < range + 1.0, "it mines only inside the use range, not from the old doubled reach");
			r.tick(60);
			test::require(operating(), "it keeps working where it stands");
			// moved out to 120 u: past the use range, under the old stopped reach 2 x range
			away(120.0);
			r.tick(2);
			test::require(!operating(), "teleported 120 u out, it has left its slot");
			for (int t = 0; t < 1200 && !operating(); ++t)
				r.tick(1);
			test::require(operating(), "and walks back and works again");
			test::require(std::hypot(r.fighter(p1).x - ox, r.fighter(p1).z - oz) < range + 1.0, "within the use range");
			// CHAR_STATS goal (k16.6.471): operating reads 87 OPERATE_MACHINERY / 'Operating machine' / source 1 / the producer, and 0xFFFF once the order ends
			const Simulation::CharStats working = r.sim.charStats(p1);
			test::require(working.goalEnum == 87 && working.goal == "Operating machine" && working.source == 1 && working.targetId == id,
				"operating: goal 87 Operating machine, source 1, target the producer");
			test::require(working.hasTargetPoint && std::hypot(working.targetX - ox, working.targetZ - oz) < 1.0, "operating: the target point is the producer's");
			const_cast<flecs::world&>(r.sim.world().ecs()).each([&](flecs::entity e, const state::Id& who) { if (who.value == p1) e.remove<state::InteractTask>(); });
			r.tick(2);
			const Simulation::CharStats idle = r.sim.charStats(p1);
			test::require(idle.goalEnum == 0xFFFF && idle.goal.empty() && idle.source == 0, "the order ended: no goal again");
		}
		// ---- k16.5.33: a Use order whose deposit has no route blocks the deposit for 20 s, re-armed while it stays unreachable ----
		{
			const std::uint64_t id = copper.site.id;
			state::Producer* p = r.producerMut(id);
			const double ox = p->x;
			const double oz = p->z;
			const auto spot = nav->nearestWalkable(ox, oz - 60.0, 30.0);
			test::require(spot.has_value(), "walkable ground 60 u from the copper origin");
			r.teleport(p1, spot->x, r.fighter(p1).y, spot->z);
			r.tick(25);
			test::require(r.use(p1, id).empty(), "use accepted while the deposit routes");
			p->x = ox + 30000.0; // the deposit moves off the mesh: the walk ends short of it
			const std::size_t before = r.events13(id, InteractEvent::kUnreachable);
			for (int t = 0; t < 600 && r.events13(id, InteractEvent::kUnreachable) == before; ++t)
				r.tick(1);
			test::require(r.events13(id, InteractEvent::kUnreachable) == before + 1, "the Use order ends unreachable");
			test::require(!r.hasUseTask(p1), "and the order is gone");
			const std::uint64_t until = r.sim.worldTaskBlockUntil(p1, id);
			const std::uint64_t now = r.sim.clockTick();
			test::require(until > now && until <= now + 400, "the {member, producer} block holds about 20 s");
			r.tick(500);
			test::require(r.sim.worldTaskBlockUntil(p1, id) > r.sim.clockTick(), "at expiry the unreachable deposit is re-armed");
			p->x = ox; // reachable again: the next expiry clears it
			r.tick(500);
			test::require(r.sim.worldTaskBlockUntil(p1, id) == 0, "a reachable deposit's block is cleared at expiry");
			std::cout << "producer no-route block ok\n";
		}
		std::cout << "ksim_producers ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_producers FAILED: " << e.what() << "\n";
		return 1;
	}
}
