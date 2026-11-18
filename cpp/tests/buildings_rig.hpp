#pragma once

// The rig of ksim_buildings_*: a scenario simulation with a player faction, one in-process Session whose output goes through the client's
// own decoder (client/src/net/simlink.cpp, compiled in unchanged), and helpers to send orders 90 / 91 / 94 as frames.

#include "net/simlink.hpp" // the client's decoder (client/src), compiled in unchanged

#include "ksim/buildings.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace bt {

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

// An order body for 90 / 91.
inline Buf placeBody(const BuildOrder& o)
{
	Buf b;
	b.str(o.record).put<float>(static_cast<float>(o.x)).put<float>(static_cast<float>(o.y)).put<float>(static_cast<float>(o.z));
	b.put<float>(static_cast<float>(o.qw)).put<float>(static_cast<float>(o.qx)).put<float>(static_cast<float>(o.qy)).put<float>(static_cast<float>(o.qz));
	b.put<std::uint8_t>(o.floor);
	return b;
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
	::simlink::SimLinkDecoder dec; // the client's decoder
	std::uint32_t seq = 100;
	std::vector<std::vector<std::uint8_t>> wire; // every write, frame by frame (decoded too)
	std::size_t buildingFrames = 0; // BUILDING frames seen since the last clearFrames

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

	void feed(const std::vector<std::uint8_t>& bytes)
	{
		for (std::size_t at = 0; at < bytes.size();)
		{
			std::uint32_t len = 0;
			std::memcpy(&len, &bytes[at], 4);
			std::uint16_t type = 0;
			std::memcpy(&type, &bytes[at + 4], 2);
			if (type == ks::kBuilding)
				++buildingFrames;
			const std::vector<std::uint8_t> view = test::clientFrame(&bytes[at], 4u + len);
			test::require(dec.feed(view.data(), view.size()), "the client decoder: " + dec.error());
			at += 4 + len;
		}
	}

	void connect()
	{
		session = std::make_unique<ks::Session>(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		feed(bytes);
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
		{
			sim.run(1);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			feed(bytes);
		}
	}

	// Sends order `kind` with `body`; returns "" when it was applied (event 9), else the reject reason (event 10).
	std::string send(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order " + std::to_string(kind) + " closed the session");
		dec.events.clear();
		feed(bytes);
		for (const auto& e : dec.events)
		{
			if ((e.kind == 9 || e.kind == 10) && e.a == seq)
				return e.kind == 9 ? std::string() : dec.str(e.str);
		}
		throw test::Failure("order " + std::to_string(kind) + " got no answer");
	}

	std::string check(const BuildOrder& o) { return send(90, placeBody(o)); }
	std::string place(const BuildOrder& o) { return send(91, placeBody(o)); }

	std::set<std::uint64_t> buildingIds() const
	{
		std::set<std::uint64_t> ids;
		for (const auto& [id, b] : dec.buildings)
			ids.insert(id);
		return ids;
	}

	// Places a building (order 91), runs two ticks and returns its Id (the one new BUILDING record); fails when the order is refused.
	std::uint64_t placeOk(const BuildOrder& o)
	{
		const std::set<std::uint64_t> before = buildingIds();
		const std::string reason = place(o);
		test::require(reason.empty(), "placement of " + o.record + " refused: " + reason);
		tick(2);
		for (const std::uint64_t id : buildingIds())
		{
			if (before.count(id) == 0)
				return id;
		}
		throw test::Failure("no BUILDING record arrived for " + o.record);
	}

	std::vector<std::string> unlocked() const { return sim.unlockedBuildings(sim.completedResearch()); }

	const kdata::Record* byName(const std::string& name) const
	{
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("BUILDING")))
		{
			if (r->name == name)
				return r;
		}
		return nullptr;
	}

	// Gives the player faction every RESEARCH record (so any BUILDING that some research enables is unlocked).
	void unlockAll()
	{
		std::vector<std::string> all;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("RESEARCH")))
			all.push_back(r->stringID);
		sim.world().ecs().each([&](state::PlayerFaction& f) { f.research = all; });
	}

	// A spot the sim accepts `record` at: scans a coarse grid of the world with the dry-run check (flat, dry, far from every town).
	// `wanted` picks which reason to find instead ("" = a valid site).
	std::optional<BuildOrder> findSite(const std::string& record, const std::string& wanted = "")
	{
		BuildOrder o;
		o.record = record;
		for (double z = -60000.0; z < 60000.0; z += 750.0)
		{
			for (double x = -60000.0; x < 60000.0; x += 750.0)
			{
				const std::optional<double> y = sim.groundHeight(x, z);
				if (!y)
					continue;
				o.x = x;
				o.z = z;
				o.y = *y;
				if (sim.checkBuildPlace(o) == wanted)
					return o;
			}
		}
		return std::nullopt;
	}

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }

	const state::PlayerBuilding* building(std::uint64_t id) const
	{
		const state::PlayerBuilding* found = nullptr;
		sim.world().ecs().each([&](const state::Id& i, const state::PlayerBuilding& b) { if (i.value == id) found = &b; });
		return found;
	}
};

} // namespace bt
