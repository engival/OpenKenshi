// kenshi-k16.6.82 (journey_reload note 4): a character that stands at the save loads standing, not down. A new game (the Wanderer start) with
// combat and the squad AI on runs a few seconds, is saved and loaded in place (Simulation::reload, the server's load), and the player's
// fighter is checked at the load and a few ticks later: not down, not KO'd, no prone flag on the wire, the same ground height. Needs the
// world navmesh bake (argv[1], exit 77 without).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "nav_source.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace ks = ksim::simlink;

namespace {

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

// One admin session as a client: sends orders, keeps what the player's CHAR_DYNAMIC flags and the ragdoll events (48) said since the last reset.
struct Wire
{
	Simulation& sim;
	ks::ServerContext context; // the Session holds a reference to it: it must outlive the session
	std::unique_ptr<ks::Session> session;
	std::vector<std::uint8_t> bytes;
	std::uint32_t seq = 0;
	std::map<std::uint64_t, std::uint32_t> flags; // last CHAR_DYNAMIC flags per character
	std::vector<std::uint64_t> ragdolls; // ids of event 48 since the last clear
	std::string lastReason;
	std::map<std::uint64_t, std::vector<std::uint8_t>> frames; // last CHAR_DYNAMIC payload per character

	Wire(Simulation& s, const std::shared_ptr<const kdata::GameData>& gd, std::uint64_t dataHash, const std::string& saveDir)
		: sim(s)
	{
		context.gameData = gd;
		context.dataHash = dataHash;
		context.saveDir = saveDir;
		session = std::make_unique<ks::Session>(sim, context);
		session->setAdmin(true);
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
			p += 4 + len;
			if (type == ks::kDynamic)
			{
				flags[at<std::uint64_t>(bytes, b + 8)] = at<std::uint32_t>(bytes, b + 40);
				frames[at<std::uint64_t>(bytes, b + 8)].assign(bytes.begin() + b, bytes.begin() + p);
			}
			else if (type == ks::kEvent)
			{
				const std::uint16_t kind = at<std::uint16_t>(bytes, b + 8);
				if (kind == 48)
					ragdolls.push_back(at<std::uint64_t>(bytes, b + 10));
				else if (kind == 10)
					lastReason = "rejected";
			}
		}
		bytes.clear();
	}

	// The record's fields a client animates from, one line.
	std::string describe(std::uint64_t id)
	{
		const std::vector<std::uint8_t>& f = frames[id];
		std::ostringstream out;
		out << "len " << f.size() << " xyz " << at<float>(f, 16) << " " << at<float>(f, 20) << " " << at<float>(f, 24) << " yaw " << at<float>(f, 28)
			<< " speed " << at<float>(f, 32) << " " << at<float>(f, 36) << " flags 0x" << std::hex << at<std::uint32_t>(f, 40) << std::dec << " legs "
			<< at<float>(f, 52) << " " << at<float>(f, 56) << " action " << at<std::uint32_t>(f, 60) << " ("
			<< (at<std::uint32_t>(f, 60) != 0 ? session->strings().text(at<std::uint32_t>(f, 60)) : std::string("none")) << ") start "
			<< at<std::uint64_t>(f, 64) << " speed " << at<float>(f, 72);
		return out.str();
	}

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

	void order(std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
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
	}

	// A save-panel order (80 save_game, 81 load_game) by name; the session answers at the next tick.
	void named(std::uint16_t kind, const std::string& name)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", kind);
		w.str("name", name);
		if (kind == 81)
			w.u8("resetPositions", 0);
		w.end();
		std::vector<std::uint8_t> reply;
		ks::BinaryWriter out(reply);
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, out), "an order kicked the client");
		bytes.assign(reply.begin(), reply.end());
		parse();
	}
};

const state::Fighter* playerOf(Simulation& sim)
{
	for (const CombatArena::Member& member : sim.combat()->members())
		if (member.fighter.playerControlled)
			return &member.fighter;
	return nullptr;
}

// What a client would read of the character on the wire: CHAR_DYNAMIC flags bits 3 / 4 / 5 (KO, dead, prone).
std::uint32_t proneBits(const state::Fighter& f)
{
	return (f.unconscious ? simlink::kFlagKO : 0u) | (f.dead ? simlink::kFlagDead : 0u) | ((f.down || f.unconscious || f.dead) ? simlink::kFlagProne : 0u);
}

void expectStanding(Simulation& sim, const std::string& when)
{
	const state::Fighter* f = playerOf(sim);
	test::require(f != nullptr, when + ": no player character");
	test::require(!f->down && !f->unconscious && !f->dead, when + ": the player is down / KO'd / dead (down " + std::to_string(f->down)
		+ ", ko " + std::to_string(f->unconscious) + ")");
	test::require(proneBits(*f) == 0, when + ": the wire flags say prone");
	test::require(!f->restYawValid, when + ": a lying yaw is held for a standing body");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_load_pose: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		auto gd = std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA)));
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA));
		sim.markPristine();
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		const auto nav = test::openWorldNav(world, zones);
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		sim.enableAi(std::move(ai));
		const StartCatalog catalog = StartCatalog::build(*gd, KENSHI_DATA);
		std::string start;
		for (const StartOff& off : catalog.all())
			if (start.empty() && off.name.find("Wanderer") != std::string::npos)
				start = off.sid;
		test::require(!start.empty(), "no Wanderer start");
		sim.enqueue(Command::newGame(start, "Tester", 1));
		sim.run(20);
		sim.run(200); // ten seconds standing at the start
		expectStanding(sim, "before the save");
		const state::Fighter before = *playerOf(sim);

		const std::string saveDir = std::string(KSIM_BINARY_DIR) + "/ksim_load_pose_saves";
		std::filesystem::create_directories(saveDir);
		std::uint64_t playerId = 0;
		for (const CombatArena::Member& member : sim.combat()->members())
			if (member.fighter.playerControlled)
				playerId = member.id;
		Wire wire(sim, gd, 0, saveDir);
		wire.run(5);

		// The player in each state a save can catch it in: saved over the save panel (order 80), the world runs on, loaded (order 81). The
		// first frame after the load and the ones after it must say what the last frame before the save said, and never prone / KO.
		const auto roundTrip = [&](const std::string& what)
		{
			const std::uint32_t was = wire.flags[playerId];
			const std::string line = wire.describe(playerId);
			wire.named(80, "pose");
			wire.run(2);
			wire.run(100);
			wire.ragdolls.clear();
			const std::uint64_t beforeLoad = sim.clockTick();
			wire.flags.clear();
			wire.named(81, "pose");
			wire.run(1);
			test::require(sim.clockTick() < beforeLoad, what + ": load_game did not load (the clock did not go back)");
			test::require(wire.flags.count(playerId) == 1, what + ": no CHAR_DYNAMIC for the player after load_game");
			std::cout << "  " << what << ": before " << line << "\n  " << what << ": after  " << wire.describe(playerId) << "\n";
			constexpr std::uint32_t kDown = ks::kFlagProne | ks::kFlagKO | ks::kFlagDead;
			test::require((wire.flags[playerId] & kDown) == 0, what + ": the first frame after the load says prone / KO");
			test::require(wire.flags[playerId] == was, what + ": the first frame's flags differ from the last before the save");
			wire.run(60);
			test::require((wire.flags[playerId] & kDown) == 0, what + ": 60 ticks after the load the wire says prone / KO");
			test::require(std::find(wire.ragdolls.begin(), wire.ragdolls.end(), playerId) == wire.ragdolls.end(), what + ": a ragdoll event (48) for the player after the load");
			expectStanding(sim, what);
		};
		roundTrip("standing");
		const state::Fighter& at = *playerOf(sim);
		wire.order(1, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", playerId);
			w.f32("x", static_cast<float>(at.x + 60.0));
			w.f32("z", static_cast<float>(at.z));
			w.f32("y", static_cast<float>(at.y));
			w.u8("flags", 2); // yValid
		});
		wire.run(30);
		roundTrip("walking");
		wire.order(10, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", playerId); w.u8("which", 1); w.u8("value", 1); });
		wire.run(40);
		roundTrip("sneaking");
		wire.order(10, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", playerId); w.u8("which", 1); w.u8("value", 0); });
		wire.order(10, [&](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", playerId); w.u8("which", 0); w.u8("value", 1); });
		wire.run(40);
		roundTrip("blocking");
		// The playtest save (kenshi-k16.6.82 rework): argv[2] is a save the user's server wrote; load it through order 81 and trace the player's frames.
		if (argc > 2 && std::filesystem::exists(argv[2]))
		{
			std::filesystem::copy_file(argv[2], saveDir + "/user", std::filesystem::copy_options::overwrite_existing);
			wire.ragdolls.clear();
			wire.flags.clear();
			wire.frames.clear();
			wire.named(81, "user");
			wire.run(1);
			const state::Fighter* loaded = playerOf(sim);
			test::require(loaded != nullptr, "the playtest save has no player character");
			std::uint64_t userPlayer = 0;
			for (const CombatArena::Member& member : sim.combat()->members())
				if (member.fighter.playerControlled)
					userPlayer = member.id;
			std::cout << "  user save: player " << userPlayer << " at " << loaded->x << " " << loaded->y << " " << loaded->z << " down " << loaded->down << " ko "
				<< loaded->unconscious << " dead " << loaded->dead << " restYawValid " << loaded->restYawValid << " building " << loaded->building << "\n";
			test::require(wire.flags.count(userPlayer) == 1, "user save: no CHAR_DYNAMIC for the player after load_game");
			std::string last;
			for (int tick = 0; tick < 100; ++tick)
			{
				const std::string line = wire.describe(userPlayer);
				if (line != last)
					std::cout << "  user save: tick " << tick << " " << line << "\n";
				last = line;
				constexpr std::uint32_t kDown = ks::kFlagProne | ks::kFlagKO | ks::kFlagDead;
				test::require((wire.flags[userPlayer] & kDown) == 0, "user save: tick " + std::to_string(tick) + " after the load says prone / KO / dead");
				wire.run(1);
			}
			test::require(std::find(wire.ragdolls.begin(), wire.ragdolls.end(), userPlayer) == wire.ragdolls.end(), "user save: a ragdoll event (48) for the player");
			expectStanding(sim, "user save");
		}
		std::filesystem::remove_all(saveDir);
		const state::Fighter beforeY = *playerOf(sim);
		std::stringstream saved;
		sim.save(saved);
		const std::string savedBytes = saved.str();
		sim.run(100); // the world moves on; the load puts it back
		saved.seekg(0);
		sim.reload(saved, false);
		{
			std::stringstream again;
			sim.save(again);
			test::require(again.str() == savedBytes, "the loaded world does not re-save to the same bytes");
		}
		expectStanding(sim, "at the load");
		for (const int ticks : {1, 5, 40})
		{
			sim.run(ticks);
			expectStanding(sim, "load + " + std::to_string(ticks) + " ticks");
		}
		const state::Fighter& after = *playerOf(sim);
		test::require(std::abs(after.y - beforeY.y) < 5.0, "the player's height jumped across the load: " + std::to_string(beforeY.y) + " -> " + std::to_string(after.y));
		std::cout << "ksim_load_pose ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_load_pose: FAILED: " << e.what() << "\n";
		return 1;
	}
}
