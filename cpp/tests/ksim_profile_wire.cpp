// Profile query over SIMLINK (docs/formats/SIMLINK.md PROFILE, order 120, frame 70; docs/PROFILING.md): through in-process Sessions (no
// socket). Frame 70 is decoded by hand from the layout in SIMLINK.md. An admin session gets a PROFILE frame listing the sections (a
// KSIM_PROFILE build) or `unsupported` (an OFF build); a non-admin session gets `not_allowed` in both, checked first; a body is refused
// as malformed; the order log never sees 120. argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/navigator.hpp"
#include "ksim/profile.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cstring>
#include <iostream>
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
	std::string str()
	{
		const std::uint16_t n = get<std::uint16_t>();
		test::require(at + n <= p.size(), "frame overrun");
		std::string s(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + n));
		at += n;
		return s;
	}
};

struct Client
{
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;

	Client(Simulation& sim, ks::ServerContext& context, bool admin, std::uint64_t dataHash)
		: session(std::make_unique<ks::Session>(sim, context))
	{
		session->setAdmin(admin);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
	}

	// Sends order 120 with `body`; everything the server wrote. `closed` is set when the session refused the frame.
	std::vector<Frame> profileQuery(const Buf& body, std::string& reason, bool& answered, bool& closed)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(120);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		closed = !session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w);
		const std::vector<Frame> all = frames(bytes);
		reason.clear();
		answered = false;
		for (const Frame& frame : all)
		{
			if (frame.type != ks::kEvent)
				continue;
			Cursor c{frame.payload};
			c.get<std::uint64_t>();
			const std::uint16_t kind = c.get<std::uint16_t>();
			if ((kind == 9 || kind == 10) && c.get<std::uint32_t>() == seq)
			{
				answered = true;
				if (kind == 10)
					reason = session->strings().text(c.get<std::uint32_t>());
			}
		}
		return all;
	}
};

int countType(const std::vector<Frame>& all, std::uint16_t type)
{
	int n = 0;
	for (const Frame& f : all)
		n += f.type == type;
	return n;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_profile_wire HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		ks::ServerContext context;
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		ks::spawnSquad(sim, *gd, *data, context);
		sim.run(30);

		Client admin(sim, context, true, context.dataHash);
		Client other(sim, context, false, context.dataHash);
		std::string reason;
		bool answered = false;
		bool closed = false;

		// A non-admin session learns nothing, in either build.
		{
			const std::vector<Frame> all = other.profileQuery(Buf{}, reason, answered, closed);
			test::require(!closed && answered && reason == "not_allowed", "a non-admin profile_query answers not_allowed (got '" + reason + "')");
			test::require(countType(all, 70) == 0, "a non-admin session gets no PROFILE frame");
		}
		// A body is a malformed frame.
		{
			admin.profileQuery(Buf{}.put<std::uint8_t>(1), reason, answered, closed);
			test::require(closed, "profile_query with a body is refused as malformed");
		}
		Client admin2(sim, context, true, context.dataHash);
#if KSIM_PROFILE
		{
			const std::vector<Frame> all = admin2.profileQuery(Buf{}, reason, answered, closed);
			test::require(!closed && answered && reason.empty(), "an admin profile_query is applied (got '" + reason + "')");
			test::require(countType(all, 70) == 1 && ks::kProfile == 70, "an admin session gets exactly one PROFILE frame");
			const Frame* frame = nullptr;
			for (const Frame& f : all)
				frame = f.type == 70 ? &f : frame;
			Cursor c{frame->payload};
			test::require(c.get<std::uint64_t>() == sim.clockTick(), "PROFILE tick is the clock's");
			const std::uint32_t window = c.get<std::uint32_t>();
			test::require(window > 0 && window <= profile::Profiler::kWindow, "PROFILE window " + std::to_string(window));
			const float mean = c.get<float>();
			const float p50 = c.get<float>();
			const float p99 = c.get<float>();
			const float max = c.get<float>();
			test::require(mean > 0 && p50 > 0 && p50 <= p99 && p99 <= max, "PROFILE tick mean / p50 / p99 / max are ordered");
			test::require(c.get<float>() == 50000.0f, "PROFILE tickBudgetUs is the 0.05 s tick period");
			const float characters = c.get<float>();
			const float squads = c.get<float>();
			c.get<float>(); // pathQueries
			c.get<float>(); // pathUs
			test::require(characters > 0 && squads > 0, "PROFILE counts characters and squads");
			const std::uint16_t n = c.get<std::uint16_t>();
			test::require(n == profile::kSections, "PROFILE lists every section (" + std::to_string(n) + ")");
			std::uint64_t total = 0;
			for (std::uint16_t i = 0; i < n; ++i)
			{
				const std::string name = c.str();
				test::require(name == profile::kSectionNames[i], "PROFILE row " + std::to_string(i) + " is '" + name + "', wanted '" + profile::kSectionNames[i] + "'");
				const float meanUs = c.get<float>();
				const float maxUs = c.get<float>();
				test::require(meanUs >= 0 && maxUs >= meanUs, "PROFILE row " + name + " mean <= max");
				total += c.get<std::uint64_t>();
			}
			test::require(total > 0, "PROFILE rows carry the run totals");
			test::require(c.at == frame->payload.size(), "PROFILE has trailing bytes");
			// The rows keep their order for the whole run, and a later answer is newer.
			sim.run(5);
			const std::vector<Frame> again = admin2.profileQuery(Buf{}, reason, answered, closed);
			test::require(countType(again, 70) == 1, "a second query is answered too");
			std::uint64_t tick2 = 0;
			for (const Frame& f : again)
			{
				if (f.type == 70)
					std::memcpy(&tick2, f.payload.data(), 8);
			}
			test::require(tick2 == sim.clockTick(), "the second PROFILE carries the newer tick");
		}
		std::cout << "profile wire: admin gets PROFILE (" << profile::kSections << " rows), non-admin not_allowed, a body refused\n";
#else
		{
			const std::vector<Frame> all = admin2.profileQuery(Buf{}, reason, answered, closed);
			test::require(!closed && answered && reason == "unsupported", "an OFF build answers unsupported (got '" + reason + "')");
			test::require(countType(all, 70) == 0, "an OFF build sends no PROFILE frame");
		}
		std::cout << "profile wire (OFF build): admin unsupported, non-admin not_allowed, a body refused\n";
#endif
		for (const ks::OrderLogEntry& entry : context.orderLog)
			test::require(entry.kind != 120, "the order log holds a profile_query");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_profile_wire: FAILED: " << e.what() << "\n";
		return 1;
	}
}
