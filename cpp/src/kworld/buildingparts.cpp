#include "kworld/buildingparts.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/navhash.hpp"

#include <algorithm>
#include <utility>

namespace kworld {

namespace {

struct Candidate
{
	const std::string* id;
	int weight;
};

// One record's `category` refs against the stream (FUN_140559b80). Group 0: chance 0 includes with no draw, any other
// chance draws trunc(rand / 32768 * 100) and includes when below it (100 still draws). Group N: one pick, weighted.
void select(const kdata::GameData& gameData, const kdata::Record& record, std::string_view category, MsvcRand& rng,
	std::vector<PickedPart>& out, int depth)
{
	const auto* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	std::vector<const std::string*> picked;
	// Several groups > 0 are drawn in the original's boost::unordered_map<int, ...> chain order: a new group is linked at the
	// head, so the LAST-first-appearing group draws first (rwh.246 section 3; inferred from the container's insertion rule,
	// not a live trace; the data uses only groups 1 and 2). Group-0 draws all come first, during the scan.
	std::vector<std::pair<int, std::vector<Candidate>>> groups;
	for (const auto& entry : refs->entries)
	{
		if (entry.v0 == kdata::kReferenceRemoved)
			continue;
		const int weight = entry.hasV1V2 ? entry.v1 : 100;
		if (entry.v0 == 0)
		{
			if (weight == 0 || static_cast<int>(rng.next() / 32768.0 * 100.0) < weight)
				picked.push_back(&entry.targetID);
			continue;
		}
		auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == entry.v0; });
		if (group == groups.end())
			group = groups.insert(groups.end(), { entry.v0, {} });
		if (weight > 0)
			group->second.push_back({ &entry.targetID, weight });
	}
	for (auto it = groups.rbegin(); it != groups.rend(); ++it)
	{
		const auto& members = it->second;
		if (members.empty())
			continue;
		if (members.size() == 1)
		{
			picked.push_back(members[0].id);
			continue;
		}
		long total = 0;
		for (const Candidate& member : members)
			total += member.weight;
		const double f = rng.next() / 32768.0 * static_cast<double>(total);
		long cumulative = 0;
		for (const Candidate& member : members)
		{
			cumulative += member.weight;
			if (static_cast<double>(cumulative) >= f)
			{
				picked.push_back(member.id);
				break;
			}
		}
	}
	for (const std::string* id : picked)
	{
		out.push_back({ *id, depth });
		if (depth < 8)
			if (const kdata::Record* part = gameData.find(*id))
				select(gameData, *part, "parts", rng, out, depth + 1);
	}
}

} // namespace

std::uint32_t partsSeed(float x, float z)
{
	return comb(F(x), F(z));
}

std::vector<PickedPart> selectBuildingPartTree(const kdata::GameData& gameData, const kdata::Record& building,
	float x, float z, PartStage stage)
{
	// The original srand()s the seed before the exterior stage and again before the interior stage; the mask and a door
	// start from the seed too. Layout furniture is its own Building and seeds from its own x/z; a door is created at the
	// parent's position, so the parent's x/z is its own (rwh.246 sections 1-2).
	MsvcRand rng(partsSeed(x, z));
	std::vector<PickedPart> out;
	const std::string_view category = stage == PartStage::Interior ? "interior"
		: stage == PartStage::InteriorMask ? "interior mask" : "parts";
	select(gameData, building, category, rng, out, 0);
	return out;
}

std::vector<std::string> selectBuildingParts(const kdata::GameData& gameData, const kdata::Record& building,
	float x, float z, PartStage stage)
{
	std::vector<std::string> ids;
	for (const PickedPart& pick : selectBuildingPartTree(gameData, building, x, z, stage))
		ids.push_back(pick.id);
	return ids;
}

} // namespace kworld
