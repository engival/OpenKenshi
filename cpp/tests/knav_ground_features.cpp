// Map features in the ground generator (kenshi-k16.7.194, 197): the Metal Arch Bridge (BRIDGE_FEATURE.md) gets its collision placed (Walkable,
// unfilled, pose and scale), reaches knav::ground as cutters, and the canyon floor under its arch stays ground. Its deck (the deck rule, NAVMESH_GENERATOR.md
// section 12) is walkable from rim to rim: one component of top faces spanning the bridge, linked to the ground faces at both ends. Three placements:
// 53, -32184 (BRIDGE_FEATURE.md) and the census sites -54433, -95121 and 12399, -29427 (the 345 u deck the shipped mesh misses).

#include "knav/ground.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>

namespace
{

namespace ground = knav::ground;

bool faceCovers(const knav::native::Mesh& m, const knav::native::Face& f, double x, double z)
{
	bool pos = false, neg = false;
	for (std::uint32_t k = 0; k < f.numEdges; ++k)
	{
		const auto& a = m.vertices[m.edges[f.startEdge + k].a];
		const auto& b = m.vertices[m.edges[f.startEdge + (k + 1) % f.numEdges].a];
		const double cross = (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]);
		(cross > 0 ? pos : neg) = true;
	}
	return !(pos && neg);
}

struct Deck
{
	std::size_t faces = 0, toGroundNear[2] = { 0, 0 };
	double min[2] = { 1e30, 1e30 }, max[2] = { -1e30, -1e30 };
};

// The longest component of top faces (joined over links among tops), its xz bound, and the links to ground faces within `end` u of each end of
// its longer axis.
Deck largestDeck(const knav::native::Mesh& mesh, const ground::Stats& stats, double end)
{
	std::set<std::uint32_t> tops(stats.topFaceIndex.begin(), stats.topFaceIndex.end());
	std::vector<int> comp(mesh.faces.size(), -1);
	std::vector<std::vector<std::uint32_t>> comps;
	for (const std::uint32_t f0 : stats.topFaceIndex)
	{
		if (comp[f0] >= 0)
			continue;
		comp[f0] = (int)comps.size();
		comps.push_back({});
		std::vector<std::uint32_t> stack = { f0 };
		while (!stack.empty())
		{
			const std::uint32_t f = stack.back();
			stack.pop_back();
			comps.back().push_back(f);
			for (std::uint32_t k = 0; k < mesh.faces[f].numEdges; ++k)
			{
				const auto& e = mesh.edges[mesh.faces[f].startEdge + k];
				if (e.oppositeFace >= 0 && tops.count(e.oppositeFace) && comp[e.oppositeFace] < 0)
				{
					comp[e.oppositeFace] = comp[f0];
					stack.push_back(e.oppositeFace);
				}
			}
		}
	}
	Deck best;
	for (const auto& faces : comps)
	{
		Deck d;
		d.faces = faces.size();
		for (const std::uint32_t f : faces)
			for (std::uint32_t k = 0; k < mesh.faces[f].numEdges; ++k)
			{
				const auto& v = mesh.vertices[mesh.edges[mesh.faces[f].startEdge + k].a];
				for (int a = 0; a < 2; ++a)
				{
					d.min[a] = std::min(d.min[a], v[a * 2]);
					d.max[a] = std::max(d.max[a], v[a * 2]);
				}
			}
		const int axis = d.max[0] - d.min[0] >= d.max[1] - d.min[1] ? 0 : 1;
		for (const std::uint32_t f : faces)
			for (std::uint32_t k = 0; k < mesh.faces[f].numEdges; ++k)
			{
				const auto& e = mesh.edges[mesh.faces[f].startEdge + k];
				if (e.oppositeFace < 0 || tops.count(e.oppositeFace))
					continue;
				const double along = mesh.vertices[e.a][axis * 2];
				if (along < d.min[axis] + end)
					++d.toGroundNear[0];
				if (along > d.max[axis] - end)
					++d.toGroundNear[1];
			}
		if (std::getenv("DEBUG_COMPS") && d.faces > 5)
			std::cout << "    comp " << d.faces << " faces, bound " << d.min[0] << ".." << d.max[0] << " x " << d.min[1] << ".." << d.max[1] << ", ground links near ends " << d.toGroundNear[0] << " / " << d.toGroundNear[1] << "\n";
		const auto span = [](const Deck& x) { return std::max(x.max[0] - x.min[0], x.max[1] - x.min[1]); };
		if (d.faces > 5 && (best.faces == 0 || span(d) > span(best)))
			best = d;
	}
	return best;
}

void bridge(const std::string& dataDir, const kworld::Heightmap& terrain, double cx, double cz)
{
	const double r = 1200;
	std::cout << "Metal Arch Bridge at " << cx << ", " << cz << "\n";
	const kworld::AreaGeometry area = kworld::gatherArea(dataDir, terrain, kworld::zoneCellOf(cx - r, cz - r), kworld::zoneCellOf(cx + r, cz + r));
	test::require(area.features && !area.features->empty(), "the area has feature collision");
	const kworld::WorldShape* deck = nullptr;
	for (const kworld::WorldShape& s : *area.features)
		if (s.min[0] < cx && s.max[0] > cx && s.min[2] < cz && s.max[2] > cz && s.max[0] - s.min[0] > 600 && (!deck || s.vertices.size() > deck->vertices.size()))
			deck = &s;
	test::require(deck != nullptr, "the Metal Arch Bridge shape lies over the sample site");
	test::require(deck->use == kworld::NavUse::Walkable && !deck->closed, "a feature is Walkable and unfilled");

	ground::Params params;
	params.keepLooseTops = std::getenv("KEEP_LOOSE") != nullptr;
	ground::CutterCounts counts;
	ground::Stats stats;
	const knav::native::Mesh mesh = ground::generateBox(dataDir, terrain, cx - r, cz - r, cx + r, cz + r, params, {}, true, &stats, &counts);
	std::cout << "  cutters: " << counts.buildingShapes << " building, " << counts.foliage << " foliage, " << counts.features << " feature; tops: " << stats.topTris << " tris ("
		<< stats.deckTris << " deck, " << stats.deckRejected << " rejected), " << stats.topFaces << " faces, " << stats.topLinks << " links, " << stats.topDroppedFaces << " dropped ("
		<< stats.topDroppedArea << " u2); ms: tops " << stats.msTops << " + link " << stats.msLink << "\n";
	test::require(counts.features > 0, "map feature shapes reach the ground as cutters");

	const Deck d = largestDeck(mesh, stats, 150);
	const int axis = d.max[0] - d.min[0] >= d.max[1] - d.min[1] ? 0 : 1;
	const double span = d.max[axis] - d.min[axis];
	std::cout << "  the deck: " << d.faces << " faces over " << span << " u, links to the ground within 150 u of its ends: " << d.toGroundNear[0] << " / " << d.toGroundNear[1] << "\n";
	test::require(span > 900, "the deck is one component from rim to rim");
	test::require(d.toGroundNear[0] > 0 && d.toGroundNear[1] > 0, "the deck is joined to the ground at both rims");

	// Under the arch: a deck vertex well over the ground whose xz the ground mesh still covers.
	std::set<std::uint32_t> tops(stats.topFaceIndex.begin(), stats.topFaceIndex.end());
	std::size_t under = 0, covered = 0;
	for (std::size_t i = 0; i + 2 < deck->vertices.size(); i += 3)
	{
		const double x = deck->vertices[i], y = deck->vertices[i + 1], z = deck->vertices[i + 2];
		if (x < cx - r + 40 || x > cx + r - 40 || z < cz - r + 40 || z > cz + r - 40 || y - terrain.height(x, z) < 60)
			continue;
		++under;
		for (std::uint32_t f = 0; f < mesh.faces.size(); ++f)
			if (!tops.count(f) && faceCovers(mesh, mesh.faces[f], x, z))
			{
				++covered;
				break;
			}
	}
	std::cout << "  deck vertices 60+ u over the ground: " << under << ", ground covers under " << covered << "\n";
	test::require(under > 0 && covered * 2 >= under, "the ground under the arch stays walkable");
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		bridge(dataDir, terrain, 53.17, -32184.3);
		bridge(dataDir, terrain, -54433, -95121);
		bridge(dataDir, terrain, 12399, -29427);
		std::cout << "knav_ground_features ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
