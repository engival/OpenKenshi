// Player sleep (task Y): the bed rates, the wake rule and the order on the wire (order 60 action 9, SIMLINK INTERACTABLE kind 5).
//  - FEATURES 160 (community numbers, UNSURE): while `Fighter::asleep` in a bed, healing and stun recovery run 8x, hunger use
//    0.33x, blood recovery 2x, and untreated damage does not degenerate (one named table, `kBedRates` in combat_rules.cpp);
//  - a sleeper wakes on any replacing player order, a move order included ("never gets up" cannot happen); a queued order
//    replaces nothing and does not wake it;
//  - the wire: the bed's kind 5 record carries an id built from the layout site (never a runtime index), the order makes the
//    player asleep (flag 16) with the pose tail on its bed, a second character ordered into the same bed is event 10
//    "occupied", and after a save + load the same id is on the wire with the sleeper's pose intact.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		if (r->name == name)
			return r;
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

void near(double got, double want, double tol, const char* what)
{
	if (std::fabs(got - want) > tol)
	{
		char b[200];
		std::snprintf(b, sizeof b, "%s: %.8g, wanted %.8g +- %g", what, got, want, tol);
		throw test::Failure(b);
	}
	std::printf("ok  %-56s %.8g\n", what, got);
}

// ---- the wire: order 60 action 9 on a real bed ----

namespace ks = ksim::simlink;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

// A client of one session: builds the frames the test sends and keeps the records it reads (CHAR_DYNAMIC, INTERACTABLE kind 5, events).
struct Wire
{
	Simulation& sim;
	ks::ServerContext context; // the Session holds a reference to it: it must outlive the session
	std::unique_ptr<ks::Session> session;
	std::vector<std::uint8_t> bytes;
	std::uint32_t seq = 0;
	struct Pose
	{
		float x = 0, y = 0, z = 0, yaw = 0;
		std::string clip;
	};
	struct Dyn
	{
		std::uint32_t flags = 0;
		std::optional<Pose> pose;
	};
	std::map<std::uint64_t, Dyn> dyn;
	struct BedRecord
	{
		std::uint32_t version = 0;
		std::uint64_t building = 0;
		std::uint64_t occupant = 0;
		std::string record;
		std::string furniture;
		float x = 0, y = 0, z = 0, yaw = 0;
		float useY = 0;
	};
	std::map<std::uint64_t, BedRecord> beds;
	struct Reply
	{
		bool applied = false;
		bool rejected = false;
		std::string reason;
	};
	std::map<std::uint32_t, Reply> replies;

	Wire(Simulation& s, const std::shared_ptr<const kdata::GameData>& gd, std::uint64_t dataHash)
		: sim(s)
	{
		context.gameData = gd;
		context.dataHash = dataHash;
		session = std::make_unique<ks::Session>(sim, context);
		ks::BinaryWriter w(bytes);
		session->begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", dataHash);
		h.str("name", "test");
		h.end();
		test::require(session->onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session->sendTick(w, true);
		parse();
	}

	void parse()
	{
		for (std::size_t p = 0; p < bytes.size();)
		{
			const std::uint32_t len = at<std::uint32_t>(bytes, p);
			const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
			test::require(len >= 2 && p + 4 + len <= bytes.size(), "a frame runs past the bytes");
			const std::size_t b = p + 6;
			const std::size_t size = p + 4 + len - b;
			p += 4 + len;
			if (type == ks::kDynamic)
			{
				const std::uint64_t id = at<std::uint64_t>(bytes, b + 8);
				Dyn& d = dyn[id];
				d.flags = at<std::uint32_t>(bytes, b + 40);
				if (size == 120)
					d.pose = Pose{at<float>(bytes, b + 100), at<float>(bytes, b + 104), at<float>(bytes, b + 108), at<float>(bytes, b + 112),
						text(at<std::uint32_t>(bytes, b + 116))};
				else
				{
					test::require(size == 100, "CHAR_DYNAMIC length " + std::to_string(size));
					d.pose.reset(); // no flag 15/16: no pose tail
				}
			} else if (type == ks::kInteractable) {
				if (bytes[b + 16] != ks::kInteractableBed)
					continue;
				test::require(size == 94, "INTERACTABLE kind 5 length " + std::to_string(size));
				BedRecord& r = beds[at<std::uint64_t>(bytes, b + 8)];
				r.record = text(at<std::uint32_t>(bytes, b + 17));
				r.x = at<float>(bytes, b + 21);
				r.y = at<float>(bytes, b + 25);
				r.z = at<float>(bytes, b + 29);
				r.useY = at<float>(bytes, b + 37);
				r.version = at<std::uint32_t>(bytes, b + 56);
				r.yaw = at<float>(bytes, b + 70);
				r.building = at<std::uint64_t>(bytes, b + 74);
				r.furniture = text(at<std::uint32_t>(bytes, b + 82));
				r.occupant = at<std::uint64_t>(bytes, b + 86);
			} else if (type == ks::kEvent) {
				const std::uint16_t kind = at<std::uint16_t>(bytes, b + 8);
				if (kind == 9)
					replies[at<std::uint32_t>(bytes, b + 10)].applied = true;
				else if (kind == 10)
				{
					Reply& r = replies[at<std::uint32_t>(bytes, b + 10)];
					r.rejected = true;
					r.reason = text(at<std::uint32_t>(bytes, b + 14));
				}
			}
		}
		bytes.clear();
	}

	std::string text(std::uint32_t id) const { return id == 0 ? std::string() : session->strings().text(id); }

	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
		{
			sim.tick();
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			parse();
		}
	}

	std::uint32_t order(std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", kind);
		body(w);
		w.end();
		std::vector<std::uint8_t> reply;
		ks::BinaryWriter out(reply);
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, out), "an order kicked the client");
		bytes.assign(reply.begin(), reply.end());
		parse();
		return seq;
	}

	std::uint32_t interact(std::uint64_t actor, std::uint64_t target, std::uint8_t action)
	{
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", actor);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", 0);
		});
	}

	std::uint32_t move(std::uint64_t actor, double x, double y, double z)
	{
		return order(1, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", actor);
			w.f32("x", static_cast<float>(x));
			w.f32("z", static_cast<float>(z));
			w.f32("y", static_cast<float>(y));
			w.u8("flags", 2); // yValid
		});
	}
};

std::uint64_t spawnPlayer(Simulation& sim, const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data,
	const std::string& recordSid, std::uint64_t& rng, double x, double y, double z)
{
	state::Fighter f = data->makeFighter(gd, recordSid, "", rng);
	f.playerControlled = true;
	f.team = 0;
	f.x = x;
	f.y = y;
	f.z = z;
	return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
}

void wireSleep(const kworld::LoadOrder& order, const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data,
	const std::shared_ptr<MeshNavigator>& nav, const std::string& servantSid, std::uint64_t dataHash)
{
	// A bare world (the Hub's buildings) with combat and the Hub mesh: every placed building with a layout indexes its beds.
	Simulation sim = Simulation::load(KENSHI_DATA, 91);
	sim.setGameData(std::make_shared<const kdata::GameData>(gd));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(data, settings);
	sim.enableNavigation(nav);

	const std::vector<Simulation::BedView> beds = sim.bedViews();
	test::require(!beds.empty(), "no real bed in the world");
	// The client's live bed (the Hub's Storm House, floor 1, the one their check named): the same layout, the same use node.
	const Simulation::BedView* bed = &beds.front();
	for (const Simulation::BedView& b : beds)
		if (std::hypot(b.x + 51190.0, b.z - 2597.0) < std::hypot(bed->x + 51190.0, bed->z - 2597.0))
			bed = &b;
	test::require((bed->id & routines::kSiteIdFlag) != 0, "the bed id is not a layout site id");
	{
		// The id is routines::siteId of the layout placement's site (2^62 + FNV(building, slot)), not a runtime index; the furniture
		// BUILDING record and the use node's own height travel with it (the pose tail and the kind 5 `use` y read them).
		const routines::SiteEntry* entry = sim.bedOf(bed->id);
		test::require(entry != nullptr && routines::siteId(entry->site) == bed->id, "the bed id is not its layout site's id");
		test::require(!entry->record.empty() && entry->record == bed->record, "the bed's site carries no furniture BUILDING record");
		test::require(entry->hasNodeY && entry->nodeY == bed->useY, "the bed's site does not carry the use node's height");
		// The node's rotated y is carried, not the piece's floor: some bed in the world has its node above (or below) the floor.
		bool offset = false;
		for (const Simulation::BedView& b : beds)
			offset = offset || std::fabs(b.useY - b.y) > 1.0e-3;
		test::require(offset, "every bed's use node sits exactly on its floor: the node height is not carried");
		// The client's bed (the Hub bar's floor-1 room, the one their check and the exe oracle named): the exe's use node there is
		// 1647.42249 (kinspect pu7). The placement is tilted ~2.2 degrees, so the layout has to be rotated in full: a yaw-only
		// placement put this node at 1646.429, a whole unit low (task Y3).
		near(bed->useY, 1647.42249, 0.1, "the client's bed's use node is the exe's floor-1 height");
		// The same tilt spreads the bar's three upstairs beds over more than a unit of floor: no single placement height can
		// place them all (1646.67 / 1647.34 / 1648.01).
		double low = 1.0e30;
		double high = -1.0e30;
		for (const Simulation::BedView& b : beds)
			if (std::hypot(b.x + 51190.0, b.z - 2597.0) < 300.0)
			{
				low = std::min(low, b.useY);
				high = std::max(high, b.useY);
			}
		test::require(high - low > 1.0, "the Hub bar's beds all stand at one height: the placement's tilt is not applied");
	}
	// The bed's use point is on the Hub mesh, and it routes away: the wake order needs a real walk.
	const auto stand = nav->walker().snap({bed->x, bed->y, bed->z}, 4.0);
	test::require(stand && std::hypot(stand->x - bed->x, stand->z - bed->z) <= 4.0, "the nearest bed's use point is not walkable");

	std::uint64_t rng = 7;
	const std::uint64_t sleeper = spawnPlayer(sim, gd, data, servantSid, rng, bed->x, bed->y, bed->z);
	// A point near the bed that routes with the sleeper's own clearance: the wake order is a real move.
	std::optional<knav::Position> away;
	for (int k = 0; k < 16 && !away; ++k)
	{
		const double angle = 0.7853981633974483 * k;
		for (const double distance : {6.0, 12.0, 20.0, 30.0})
		{
			const auto goal = nav->walker().snap({bed->x + distance * std::cos(angle), bed->y, bed->z + distance * std::sin(angle)}, 8.0);
			if (goal && nav->routeFrom({bed->x, bed->y, bed->z}, goal->x, goal->z, goal->y, false, sim.pathRadius(*sim.combat()->find(sleeper))).found)
			{
				away = goal;
				break;
			}
		}
	}
	test::require(away.has_value(), "no route away from the bed (the wake order)");
	// The wake order goes to the spot the client's live check measured: inside the room on floor 1, about 10 u from the bed.
	const auto clientSpot = nav->walker().snap({-51181.0, bed->y, 2600.0}, 8.0);
	if (clientSpot)
		away = clientSpot;
	Wire w(sim, std::make_shared<const kdata::GameData>(gd), dataHash);
	test::require(w.beds.count(bed->id) == 1, "the bed's kind 5 record did not reach the client");
	test::require(w.beds[bed->id].version == 1 && w.beds[bed->id].occupant == 0, "the free bed's record is wrong");
	test::require(!w.beds[bed->id].record.empty() && w.beds[bed->id].record == bed->record, "the bed record's `record` strid is not the furniture BUILDING record");
	test::require(!w.beds[bed->id].furniture.empty() && w.beds[bed->id].furniture == bed->record,
		"the bed tail's `furniture` strid is not the furniture BUILDING record\"" + w.beds[bed->id].furniture + "\"");
	test::require(w.beds[bed->id].building == bed->building, "the bed tail's building");
	near(w.beds[bed->id].y, bed->y, 0.01, "the kind 5 pos y is the floor point (the walk's end)");
	near(w.beds[bed->id].useY, bed->useY, 0.01, "the kind 5 use y is the use node's height");

	// The order: the player lies down in the bed it names.
	const std::uint32_t sleepSeq = w.interact(sleeper, bed->id, 9);
	w.run(2);
	test::require(w.replies[sleepSeq].applied && !w.replies[sleepSeq].rejected, "the sleep order was not applied");
	const Wire::Dyn sleeperDyn = w.dyn[sleeper];
	test::require((sleeperDyn.flags & ks::kFlagAsleep) != 0, "CHAR_DYNAMIC does not carry flag 16 for the ordered sleeper");
	test::require(sleeperDyn.pose.has_value() && sleeperDyn.pose->clip == bed->clip, "no pose tail, or not the bed's clip");
	near(sleeperDyn.pose->x, bed->x, 0.01, "poseX is the bed's use node");
	near(sleeperDyn.pose->y, bed->useY, 0.01, "poseY is the use node's height");
	near(sleeperDyn.pose->z, bed->z, 0.01, "poseZ is the bed's use node");
	near(sleeperDyn.pose->yaw, bed->yaw, 1.0e-4, "poseYaw is the bed's lie-down facing");
	test::require(w.beds[bed->id].version == 2 && w.beds[bed->id].occupant == sleeper, "the occupied bed's record (version 2, occupant)");

	// A second character ordered into the same bed: event 10 "occupied".
	std::uint64_t rng2 = 11;
	const std::uint64_t second = spawnPlayer(sim, gd, data, servantSid, rng2, bed->x + 1.0, bed->y, bed->z);
	w.run(1);
	const std::uint32_t refusedSeq = w.interact(second, bed->id, 9);
	w.run(1);
	test::require(w.replies[refusedSeq].rejected && w.replies[refusedSeq].reason == "occupied",
		"the second character into the same bed: " + w.replies[refusedSeq].reason);

	// A carrier (k16.6.217): order 60 action 9 from an actor carrying a KO'd squadmate puts the carried one in a free bed; an occupied
	// bed refuses, and carrying a corpse is not_allowed.
	{
		const Wire::Dyn before = w.dyn[sleeper];
		(void)before;
		const Simulation::BedView* free = nullptr;
		for (const Simulation::BedView& b : beds)
			if (b.id != bed->id && std::hypot(b.x - bed->x, b.z - bed->z) < 400.0 && w.beds.count(b.id) == 1 && w.beds[b.id].occupant == 0)
				if (free == nullptr || std::hypot(b.x - bed->x, b.z - bed->z) < std::hypot(free->x - bed->x, free->z - bed->z))
					free = &b;
		test::require(free != nullptr, "no second bed near the first");
		std::uint64_t rngC = 31;
		const std::uint64_t carrier = spawnPlayer(sim, gd, data, servantSid, rngC, free->x + 1.5, free->y, free->z);
		const std::uint64_t victim = spawnPlayer(sim, gd, data, servantSid, rngC, free->x + 2.5, free->y, free->z);
		const auto lift = [&](std::uint64_t id)
		{
			state::Fighter* v = const_cast<state::Fighter*>(sim.combat()->find(victim));
			v->unconscious = true;
			v->carriedBy = id;
			const_cast<state::Fighter*>(sim.combat()->find(id))->carrying = victim;
		};
		lift(carrier);
		test::require(sim.sleepRefusal(carrier, bed->id) == "occupied", "a carrier into an occupied bed is not 'occupied'");
		test::require(sim.sleepRefusal(carrier, free->id).empty(), "a carrier into a free bed is refused: " + sim.sleepRefusal(carrier, free->id));
		const std::uint32_t putSeq = w.interact(carrier, free->id, 9);
		w.run(2);
		test::require(w.replies[putSeq].applied && !w.replies[putSeq].rejected, "the carrier's bed order was refused: " + w.replies[putSeq].reason);
		for (int i = 0; i < 600 && sim.combat()->find(carrier)->carrying != 0; ++i)
			w.run(1);
		const state::Fighter* put = sim.combat()->find(victim);
		test::require(sim.combat()->find(carrier)->carrying == 0 && !sim.combat()->find(carrier)->asleep, "the carrier still carries, or lay down");
		test::require(put->asleep && put->carriedBy == 0, "the carried one is not asleep in the bed");
		test::require(w.beds[free->id].occupant == victim, "the bed does not report the carried one");
		test::require(sim.lyingInBed(victim, *put), "the carried one is not lyingInBed (the CHAR_MEDICAL putIn bit 8 the client reads)");
		const std::uint64_t other = spawnPlayer(sim, gd, data, servantSid, rngC, free->x + 4.0, free->y, free->z);
		test::require(sim.sleepRefusal(other, free->id) == "occupied", "a second order on the filled bed is not 'occupied': " + sim.sleepRefusal(other, free->id));
		const std::uint32_t secondSeq = w.interact(other, free->id, 9);
		w.run(2);
		test::require(w.replies[secondSeq].rejected && w.replies[secondSeq].reason == "occupied", "the second order 60 action 9 was not refused occupied");
		// A corpse in the arms: not_allowed.
		std::uint64_t corpseCarrier = spawnPlayer(sim, gd, data, servantSid, rngC, free->x + 5.0, free->y, free->z);
		const std::uint64_t corpse = spawnPlayer(sim, gd, data, servantSid, rngC, free->x + 6.0, free->y, free->z);
		state::Fighter* dead = const_cast<state::Fighter*>(sim.combat()->find(corpse));
		dead->dead = true;
		dead->carriedBy = corpseCarrier;
		const_cast<state::Fighter*>(sim.combat()->find(corpseCarrier))->carrying = corpse;
		test::require(sim.sleepRefusal(corpseCarrier, free->id) == "not_allowed", "carrying a corpse into a bed is not 'not_allowed'");
		std::printf("carrier: put-in, occupied, corpse refusal\n");
	}

	// The save: after a load the same bed id is on the wire (2^62 + FNV(building, slot), not a runtime index), the bed reports
	// the sleeper, and its pose tail is still its bed (the site index a load rebuilds lists it).
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_player_sleep.ksav";
	sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
	loaded.setGameData(std::make_shared<const kdata::GameData>(gd));
	loaded.enableCombat(data, settings);
	loaded.enableNavigation(nav);
	Wire lw(loaded, std::make_shared<const kdata::GameData>(gd), dataHash);
	test::require(lw.beds.count(bed->id) == 1,
		"the bed's id is not on the wire after save + load: it must be 2^62 + FNV(building, slot), not a runtime index");
	test::require(lw.beds[bed->id].version == 2 && lw.beds[bed->id].occupant == sleeper, "the loaded bed record is not the sleeper's");
	test::require((lw.dyn[sleeper].flags & ks::kFlagAsleep) != 0, "the sleeper is not asleep after the load");
	test::require(lw.dyn[sleeper].pose.has_value() && lw.dyn[sleeper].pose->clip == bed->clip, "the loaded sleeper has no pose tail");
	near(lw.dyn[sleeper].pose->x, bed->x, 0.01, "a loaded sleeper's poseX is its bed");
	near(lw.dyn[sleeper].pose->z, bed->z, 0.01, "a loaded sleeper's poseZ is its bed");
	std::remove(path.c_str());

	// A replacing move order wakes it (the loaded game, the same rule).
	const std::uint32_t moveSeq = lw.move(sleeper, away->x, away->y, away->z);
	lw.run(2);
	test::require(lw.replies[moveSeq].applied, "the replacing move order was refused: " + lw.replies[moveSeq].reason);
	test::require((lw.dyn[sleeper].flags & ks::kFlagAsleep) == 0 && !lw.dyn[sleeper].pose.has_value(), "the replacing order did not wake the sleeper");
	test::require(lw.beds[bed->id].version == 1 && lw.beds[bed->id].occupant == 0, "the bed stays occupied (version 2) after the wake: the client cannot order it again");
	// A second sleep order on the same bed, after the wake, from where the player stands (inside the room, the client's live check):
	// the route to the bed must be found again (the wake leaves no state that blocks it).
	for (int i = 0; i < 60; ++i)
		lw.run(1);
	const std::uint32_t againSeq = lw.interact(sleeper, bed->id, 9);
	lw.run(2);
	test::require(lw.replies[againSeq].applied, "the second sleep order after a wake was refused: " + lw.replies[againSeq].reason);
	for (int i = 0; i < 600 && !loaded.combat()->find(sleeper)->asleep; ++i)
		lw.run(1);
	test::require(loaded.combat()->find(sleeper)->asleep, "the second sleep order after a wake never put the player to sleep");
	test::require(lw.beds[bed->id].version == 2 && lw.beds[bed->id].occupant == sleeper, "the bed does not report the second sleep (version 2, occupant)");

	// The client's live sequence: the first order from the street outside (the walk in through the door), the wake, then a second
	// order from inside the room about 10 u from the bed (floor 1). Both routes must hold.
	{
		Simulation street = Simulation::load(KENSHI_DATA, 91);
		street.setGameData(std::make_shared<const kdata::GameData>(gd));
		street.enableCombat(data, settings);
		street.enableNavigation(nav);
		std::uint64_t rng3 = 23;
		std::uint64_t walker = 0;
		std::optional<knav::Position> start;
		for (int k = 0; k < 32 && !start; ++k)
		{
			const double angle = 0.19634954084936207 * k;
			for (const double distance : {20.0, 30.0, 40.0, 60.0, 80.0})
			{
				const double x = bed->x + distance * std::cos(angle);
				const double z = bed->z + distance * std::sin(angle);
				const auto spot = nav->walker().snap({x, bed->y, z}, 8.0);
				if (!spot)
					continue;
				walker = spawnPlayer(street, gd, data, servantSid, rng3, spot->x, spot->y, spot->z);
				const Route route = nav->routeFrom({spot->x, spot->y, spot->z}, bed->x, bed->z, bed->y, false,
					street.pathRadius(*street.combat()->find(walker)));
				if (route.found)
				{
					start = spot;
					break;
				}
				rng3 = 23;
			}
		}
		test::require(start.has_value() && walker != 0, "no street order point routes to the bed");
		Wire sw(street, std::make_shared<const kdata::GameData>(gd), dataHash);
		const std::uint32_t streetSeq = sw.interact(walker, bed->id, 9);
		sw.run(1);
		test::require(sw.replies[streetSeq].applied, "the street sleep order was refused: " + sw.replies[streetSeq].reason);
		for (int i = 0; i < 600 && !street.combat()->find(walker)->asleep; ++i)
			sw.run(1);
		test::require(street.combat()->find(walker)->asleep, "the walk from the street did not end asleep");
		const auto inside = nav->walker().snap({-51181.0, bed->y, 2600.0}, 8.0);
		test::require(inside.has_value(), "the client's inside spot is not on the mesh");
		const std::uint32_t wakeSeq = sw.move(walker, inside->x, inside->y, inside->z);
		sw.run(1);
		test::require(sw.replies[wakeSeq].applied && !street.combat()->find(walker)->asleep, "the inside move did not wake the street sleeper");
		for (int i = 0; i < 600 && street.combat()->find(walker)->walking(); ++i)
			sw.run(1);
		const std::uint32_t backSeq = sw.interact(walker, bed->id, 9);
		sw.run(2);
		test::require(sw.replies[backSeq].applied, "the second sleep order from inside the room was refused: " + sw.replies[backSeq].reason);
		for (int i = 0; i < 600 && !street.combat()->find(walker)->asleep; ++i)
			sw.run(1);
		test::require(street.combat()->find(walker)->asleep, "the second sleep order from inside the room never put the player to sleep");
	}
	std::printf("wire: bed %llu, flag 16 + pose tail, occupied refusal, the same id after save + load, a second sleep after a wake\n",
		static_cast<unsigned long long>(bed->id));
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_player_sleep HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* servant = recordNamed(gd, "CHARACTER", "Escaped Servant");
		CombatSettings settings;
		settings.medicalClock = 1.0;
		CombatRules rules(data, settings);
		std::uint64_t rng = 41;
		const double kDt = 0.05;

		// A fresh fighter at full blood: one part damaged, its bandage pool full (so the pool never limits the rate).
		const auto make = [&](bool asleep)
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.playerControlled = true;
			f.skills.toughness = 50.0;
			f.hunger = 1.0;
			f.asleep = asleep;
			f.wounds.clear();
			f.blood = 1.0e9; // max, for the healing rows; the blood row sets its own level
			for (state::FighterPart& p : f.parts)
			{
				p.health = p.maxHealth - p.wear;
				p.pool = 0.0;
				p.stun = 0.0;
				p.heat = 1.0;
			}
			f.blood = f.maxBloodRace;
			return f;
		};
		const auto healGain = [&](bool asleep)
		{
			state::Fighter f = make(asleep);
			state::FighterPart& p = f.parts[1];
			p.health = p.maxHealth - p.wear - 50.0;
			p.pool = 1.0e6; // never the limiting term
			const double before = p.health;
			rules.updateMedical(f, kDt);
			return f.parts[1].health - before;
		};

		// ---- FEATURES 160: healing 8x ----
		const double awakeGain = healGain(false);
		const double sleepGain = healGain(true);
		test::require(awakeGain > 0.0, "the awake part does not heal at all");
		near(sleepGain / awakeGain, 8.0, 1.0e-9, "healing while asleep, x the awake rate");

		// ---- hunger 0.33x ----
		{
			state::Fighter awake = make(false);
			state::Fighter sleeping = make(true);
			near(rules.hungerUse(awake), awake.hungerRate, 1.0e-12, "hunger use awake (standing, F 1)");
			near(rules.hungerUse(sleeping) / rules.hungerUse(awake), 0.33, 1.0e-12, "hunger use asleep, x the awake rate");
		}

		// ---- blood recovery 2x ----
		{
			state::Fighter awake = make(false);
			state::Fighter sleeping = make(true);
			awake.blood = sleeping.blood = 0.5 * awake.maxBloodRace;
			const double before = awake.blood;
			rules.updateMedical(awake, kDt);
			rules.updateMedical(sleeping, kDt);
			test::require(awake.blood > before, "blood does not recover on the ground");
			near((sleeping.blood - before) / (awake.blood - before), 2.0, 1.0e-9, "blood recovery asleep, x the awake rate");
		}

		// ---- no degeneration while asleep ----
		{
			state::Fighter awake = make(false);
			state::Fighter sleeping = make(true);
			for (state::FighterPart& p : awake.parts)
			{
				p.health = p.maxHealth - p.wear - 0.5 * (p.maxHealth - p.wear); // 50 %: past the 20 % degeneration floor
				p.pool = 0.0;
			}
			sleeping.parts = awake.parts;
			const double before = awake.parts[1].health;
			rules.updateMedical(awake, kDt);
			rules.updateMedical(sleeping, kDt);
			test::require(awake.parts[1].health < before, "an untreated part does not degenerate on the ground");
			near(sleeping.parts[1].health, before, 1.0e-12, "an untreated part while asleep");
		}

		// ---- a replacing move order wakes the sleeper ----
		{
			Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
			sim.enableCombat(data, settings);
			state::Fighter f = make(false);
			const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			auto& arenaFighter = const_cast<state::Fighter&>(*sim.combat()->find(id));
			const double x = arenaFighter.x;
			const double z = arenaFighter.z;
			const double y = arenaFighter.y;
			arenaFighter.asleep = true;
			sim.enqueue(Command::fighterMoveTo(state::Id{ id }, { { x + 10.0, z } }, { y }, 1, false, false));
			sim.run(1);
			test::require(!sim.combat()->find(id)->asleep, "a move order did not wake the sleeper");

			// A queued order replaces nothing: the sleeper stays in bed.
			const_cast<state::Fighter&>(*sim.combat()->find(id)).asleep = true;
			sim.enqueue(Command::fighterMoveTo(state::Id{ id }, { { x + 20.0, z } }, { y }, 2, true, false));
			sim.run(1);
			test::require(sim.combat()->find(id)->asleep, "a queued order woke the sleeper");
		}

		// ---- the wire: order 60 action 9, the kind 5 record, occupancy and a save + load (task Y) ----
		{
			const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
			const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
			wireSleep(order, gd, data, nav, servant->stringID, ksim::simlink::dataHash(order));
		}

		std::cout << "ksim_player_sleep: ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_player_sleep: " << e.what() << "\n";
		return 1;
	}
}
