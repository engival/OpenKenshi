// The original's stale-tile check, wired into the runtime native loader (MeshNavigator::openNative; docs/formats/
// NAVMESH_CACHE.md "The stale-tile check in the loader", kworld/navhash_source.hpp). A shipped tile is a cache: the loader recomputes each
// zone's and interior's hash from our world data and compares it with the hash stored beside the shipped mesh.
//  - zone 10,10: the forest word alone reproduces the stored hash -> the shipped tile is used as is;
//  - the Hub (20,32): the stored hash is stale (kinspect: c9ebf91d vs the live d4dc7de5) -> our bake, zone and all its
//    interiors (no interior is decided on its own);
//  - zone 23,33: fresh, its two bare interiors match -> shipped;
//  - zone 24,18: fresh; its three bare interiors ship, the one with children is not computable (our child gather order is
//    a guess) -> our bake;
//  - zone 38,22: fresh; its bare interior's stored hash differs -> our bake (a stale interior in a fresh zone).
// The log receives one line per converted region, with the shipped/rebaked/unverifiable counts. Runs against the install
// (KENSHI_DATA); exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using ksim::MeshNavigator;

const MeshNavigator::NativeTileReport& reportOf(const MeshNavigator& nav, kworld::ZoneCell zone)
{
	const MeshNavigator::NativeTileReport* report = nav.nativeReport(zone);
	test::require(report != nullptr, "no stale-check report for zone " + std::to_string(zone.x) + "," + std::to_string(zone.z));
	return *report;
}

const MeshNavigator::NativeInteriorReport& interiorOf(const MeshNavigator::NativeTileReport& report, std::uint64_t uid,
	const char* where)
{
	for (const MeshNavigator::NativeInteriorReport& interior : report.interiors)
		if (interior.uid == uid)
			return interior;
	throw test::Failure("interior " + std::to_string(uid) + " not reported in " + where);
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_native_stale: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::vector<std::string> log;
		const auto nav = MeshNavigator::openNative(KENSHI_DATA, { { 10, 10 } },
			[&log](const std::string& line) { log.push_back(line); });

		// A zone whose only hash term is the forest: 48 instances, no buildings, the shipped hash reproduced.
		const MeshNavigator::NativeTileReport& tenth = reportOf(*nav, { 10, 10 });
		test::require(tenth.decision.choice == MeshNavigator::NativeChoice::Shipped, "zone 10,10 is not shipped");
		test::require(tenth.decision.stored == 0xe93d103c && tenth.decision.ours == 0xe93d103c,
			"zone 10,10's stored and recomputed hashes are not both e93d103c");
		test::require(tenth.interiors.empty(), "zone 10,10 is reported with interiors");

		// A two-biome zone: its word comes out right only with the biome list in the live order (the wider biome first,
		// kinspect navhash section 14), so the shipped tile is used as is.
		nav->setCoverage({ { 10, 11 } });
		const MeshNavigator::NativeTileReport& coastal = reportOf(*nav, { 10, 11 });
		test::require(coastal.decision.choice == MeshNavigator::NativeChoice::Shipped, "zone 10,11 is not shipped");
		test::require(coastal.decision.stored == 0x7c47f9a3 && coastal.decision.ours == 0x7c47f9a3,
			"zone 10,11's stored and recomputed zone hashes are not both 7c47f9a3");

		// The Hub: the shipped zone hash is stale, so the whole tile (zone and interiors) comes from our bake.
		nav->setCoverage({ { 20, 32 } });
		const MeshNavigator::NativeTileReport& hub = reportOf(*nav, { 20, 32 });
		test::require(hub.decision.choice == MeshNavigator::NativeChoice::Rebaked, "the Hub zone is not rebaked");
		test::require(hub.decision.reason == "stale hash", "the Hub's rebake is not a stale hash: " + hub.decision.reason);
		test::require(hub.decision.stored == 0xc9ebf91d && hub.decision.ours == 0xf847e471,
			"the Hub's stored/recomputed zone hashes are not c9ebf91d/f847e471");
		test::require(hub.interiors.empty(), "the Hub's interiors are decided on their own beside a zone rebake");

		// A fresh zone in the same region: its two bare interiors match the stored hashes and stay shipped.
		nav->setCoverage({ { 23, 33 } });
		const MeshNavigator::NativeTileReport& stack = reportOf(*nav, { 23, 33 });
		test::require(stack.decision.choice == MeshNavigator::NativeChoice::Shipped, "zone 23,33 is not shipped");
		test::require(stack.interiors.size() == 2, "zone 23,33 does not report its two interiors");
		for (const MeshNavigator::NativeInteriorReport& interior : stack.interiors)
		{
			test::require(interior.decision.choice == MeshNavigator::NativeChoice::Shipped,
				"zone 23,33 interior " + std::to_string(interior.uid) + " is not shipped");
			test::require(interior.decision.stored == interior.decision.ours, "a shipped interior's hash does not match stored");
		}

		// A fresh zone with children: the bare interiors ship, the child-bearing one is not computable (our gather order).
		nav->setCoverage({ { 24, 18 } });
		const MeshNavigator::NativeTileReport& town = reportOf(*nav, { 24, 18 });
		test::require(town.decision.choice == MeshNavigator::NativeChoice::Shipped, "zone 24,18 is not shipped");
		for (const std::uint64_t uid : { 0x80499ull, 0xb0499ull, 0x50499ull })
			test::require(interiorOf(town, uid, "zone 24,18").decision.choice == MeshNavigator::NativeChoice::Shipped,
				"a bare interior of zone 24,18 is not shipped");
		const MeshNavigator::NativeInteriorReport& childOrder = interiorOf(town, 0x60499, "zone 24,18");
		test::require(childOrder.decision.choice == MeshNavigator::NativeChoice::NotComputable,
			"the child-bearing interior of zone 24,18 is not counted not computable");
		test::require(childOrder.decision.reason == "child gather order unknown",
			"the child-bearing interior's reason is not the gather order: " + childOrder.decision.reason);
		test::require(childOrder.decision.stored == 0x27658f9a && childOrder.decision.ours == 0x816a1c55,
			"the child-bearing interior's stored/recomputed hashes are not 27658f9a/816a1c55");

		// A fresh zone whose bare interior is stale: its zone mesh ships, the interior's tiles come from our bake.
		nav->setCoverage({ { 38, 22 } });
		const MeshNavigator::NativeTileReport& stale = reportOf(*nav, { 38, 22 });
		test::require(stale.decision.choice == MeshNavigator::NativeChoice::Shipped, "zone 38,22 is not shipped");
		const MeshNavigator::NativeInteriorReport& interior = interiorOf(stale, 0x305a7, "zone 38,22");
		test::require(interior.decision.choice == MeshNavigator::NativeChoice::Rebaked, "zone 38,22's interior is not rebaked");
		test::require(interior.decision.reason == "stale hash", "zone 38,22's interior rebake is not a stale hash");
		test::require(interior.decision.stored == 0xc2d8cc6a && interior.decision.ours == 0x92dd6d72,
			"zone 38,22's interior stored/recomputed hashes are not c2d8cc6a/92dd6d72");

		const MeshNavigator::NativeStaleStats& stats = nav->nativeStaleStats();
		test::require(stats.zonesScanned >= 5, "too few zones were decided"); // the check is lazy: only wanted zones
		test::require(stats.zonesShipped >= 3 && stats.zonesRebaked >= 1, "the zone counters do not add up");
		test::require(stats.interiorsShipped >= 5, "too few shipped interiors counted");
		test::require(stats.interiorsRebaked >= 1, "too few stale interiors counted");
		test::require(stats.interiorsNotComputable >= 1 && stats.interiorChildOrder >= 1,
			"the child-order fallback is not counted");
		test::require(stats.bakeMs > 0.0 && stats.checkMs >= 0.0, "the check/bake time is not recorded");
		// No silent fallback: every conversion leaves a line with the counts.
		test::require(log.size() >= 4, "the loader did not log its conversions");
		bool rebakeLogged = false;
		for (const std::string& line : log)
			rebakeLogged = rebakeLogged || line.find("rebaked (stale hash)") != std::string::npos;
		test::require(rebakeLogged, "no log line reports a stale-hash rebake");
		for (const std::string& line : log)
			std::cout << line << "\n";
		std::cout << "ksim_native_stale: zones " << stats.zonesShipped << " shipped, " << stats.zonesRebaked << " rebaked, "
			<< stats.zonesNotComputable << " unverifiable; interiors " << stats.interiorsShipped << " shipped, "
			<< stats.interiorsRebaked << " rebaked, " << stats.interiorsNotComputable << " unverifiable\n";

		// The loader can read the sim's own merged game data instead of loading a second copy (openNative's `gameData`;
		// ksim_server passes context.gameData): the same inputs, the same decision.
		const auto data = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const auto shared = MeshNavigator::openNative(KENSHI_DATA, { { 10, 10 } }, [](const std::string&) {}, data.get());
		const MeshNavigator::NativeTileReport& sharedTenth = reportOf(*shared, { 10, 10 });
		test::require(sharedTenth.decision.choice == MeshNavigator::NativeChoice::Shipped
			&& sharedTenth.decision.stored == 0xe93d103c && sharedTenth.decision.ours == 0xe93d103c,
			"a loader sharing the sim's game data does not decide zone 10,10 as e93d103c");
		std::cout << "ksim_native_stale: shared game data decides zone 10,10 as e93d103c too\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_native_stale: " << e.what() << "\n";
		return 1;
	}
}
