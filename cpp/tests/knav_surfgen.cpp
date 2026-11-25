// The geometric navmesh generator (knav/surfgen.hpp, docs/formats/NAVMESH_GENERATOR.md stage 1, kenshi-k16.7.131) on two buildings of the game data:
// the Hub bar (Storm House 3384, zone 20,32) and the Desert Paradise Longhouse (record 4028, placed at the original's pose), each baked in its own
// frame (Input::pose). Checked against our invariants and our own bakes, never against the original's mesh (user 2026-11-24: "surfgen_grade should
// be considered a dangerous target. The point of switching to our own navmesh everywhere is to unify things.").

#include "kdata/gamedata.hpp"
#include "knav/area.hpp"
#include "knav/surfgen.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using P3 = std::array<double, 3>;

double area3d(const knav::native::Mesh& mesh, const knav::native::Face& face)
{
	double sx = 0, sy = 0, sz = 0;
	for (std::uint32_t k = 0; k < face.numEdges; ++k)
	{
		const P3 &a = mesh.vertices[mesh.edges[face.startEdge + k].a], &b = mesh.vertices[mesh.edges[face.startEdge + (k + 1) % face.numEdges].a];
		sx += a[1] * b[2] - a[2] * b[1];
		sy += a[2] * b[0] - a[0] * b[2];
		sz += a[0] * b[1] - a[1] * b[0];
	}
	return 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
}

// Faces reachable from face 0 over matched edges.
std::size_t reachable(const knav::native::Mesh& mesh)
{
	if (mesh.faces.empty())
		return 0;
	std::vector<char> seen(mesh.faces.size(), 0);
	std::vector<std::size_t> stack{ 0 };
	seen[0] = 1;
	std::size_t count = 1;
	while (!stack.empty())
	{
		const std::size_t f = stack.back();
		stack.pop_back();
		for (std::uint32_t k = 0; k < mesh.faces[f].numEdges; ++k)
		{
			const std::int32_t other = mesh.edges[mesh.faces[f].startEdge + k].oppositeFace;
			if (other >= 0 && !seen[static_cast<std::size_t>(other)])
			{
				seen[static_cast<std::size_t>(other)] = 1;
				++count;
				stack.push_back(static_cast<std::size_t>(other));
			}
		}
	}
	return count;
}

struct Result
{
	knav::native::Mesh mesh;
	knav::surfgen::Report report;
	double area = 0;
};

Result run(const std::vector<kworld::WorldShape>& shapes, const std::vector<kworld::WorldShape>& mask, const kworld::PlacedInstance& placed)
{
	knav::surfgen::Input input;
	input.pose = knav::surfgen::Input::poseOf(placed);
	for (const kworld::WorldShape& s : shapes)
		input.shapes.push_back(&s);
	for (const kworld::WorldShape& m : mask)
		input.mask.push_back(&m);
	Result r;
	r.mesh = knav::surfgen::generate(input, {}, &r.report);
	for (const knav::native::Face& f : r.mesh.faces)
		r.area += area3d(r.mesh, f);
	const Result again = [&]
	{
		Result second;
		second.mesh = knav::surfgen::generate(input, {});
		return second;
	}();
	test::require(again.mesh.vertices == r.mesh.vertices && again.mesh.faces.size() == r.mesh.faces.size() && again.mesh.edges.size() == r.mesh.edges.size(),
		"the generator is deterministic");
	return r;
}

void common(const char* name, const Result& r)
{
	std::cout << name << ": " << r.mesh.faces.size() << " faces, 3D area " << r.area << ", matched " << r.report.matchedEdges << ", dropped "
		<< r.report.droppedFaces << " faces / " << r.report.droppedArea << " u2, overlap " << r.report.overlapArea << ", components " << r.report.components << ", reached "
		<< r.report.reachedShare << (r.report.seedSuspect ? " (seed suspect)" : "") << std::endl;
	test::require(reachable(r.mesh) == r.mesh.faces.size(), "one component: every face reaches every other over matched edges");
	test::require(r.report.overlapArea < 0.05, "no two faces overlap in xz at the same height (u2)");
	test::require(r.report.steepFaces == 0, "no face steeper than 48 degrees");
	for (const knav::native::Face& f : r.mesh.faces)
		test::require(f.numEdges >= 3, "faces have three corners or more");
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library{ dataDir, gd };

		// the Hub bar: zone 20,32, instance 4933-Newwworld
		{
			const kworld::ZoneCell zone = kworld::zoneCellOf(-51181.133, 2638.970);
			const kworld::AreaGeometry area = knav::gatherArea(dataDir, terrain, zone, zone);
			const kworld::AreaBuilding* bar = nullptr;
			for (const kworld::AreaBuilding& b : area.buildings)
				if (!b.shapes->empty() && b.placed.instanceID.find("4933-Newwworld") != std::string::npos)
					bar = &b;
			test::require(bar != nullptr, "the Hub bar is placed in zone 20,32");
			test::require(bar->localShapes && bar->localMask && !bar->localMask->empty(), "the bar has an interior mask (and its building-frame copies)");
			const std::vector<kworld::WorldShape>& mask = *bar->localMask;
			const Result r = run(*bar->localShapes, mask, bar->placed);
			common("hub bar", r);
			// our own bake (the lab's ./run.sh bar: 24,067 u2 in xz, both floors), not the original's mesh (user 2026-11-24: grading against the
			// original is "a dangerous target")
			test::require(r.report.area > 20000.0, "the bar's two floors are there (our bake: 24,067 u2 in xz)");
			test::require(r.report.reachedShare > 0.9 && !r.report.seedSuspect, "the entry reaches the building (our bake: 98 %)");
			test::require(r.report.droppedArea < 100.0, "pruning removes only specks");

			// stage 2 (kenshi-k16.7.143): the building's zone-side faces, the seam on the mask planes, the door painter on both meshes
			knav::surfgen::Input zoneInput;
			zoneInput.pose = knav::surfgen::Input::poseOf(bar->placed);
			for (const kworld::WorldShape& s : *bar->localShapes)
				zoneInput.shapes.push_back(&s);
			for (const kworld::WorldShape& m : mask)
				zoneInput.mask.push_back(&m);
			zoneInput.side = knav::surfgen::Side::Zone;
			Result z;
			z.mesh = knav::surfgen::generate(zoneInput, {}, &z.report);
			for (const knav::native::Face& f : z.mesh.faces)
				z.area += area3d(z.mesh, f);
			std::cout << "hub bar zone side: " << z.mesh.faces.size() << " faces, 3D area " << z.area << ", seam " << z.report.seamEdges << " edges / " << z.report.seamLength
				<< " u (interior " << r.report.seamEdges << " / " << r.report.seamLength << "), door faces " << z.report.doorFaces << " / " << r.report.doorFaces
				<< ", door area zone " << z.report.doorArea << " interior " << r.report.doorArea << "\n";
			test::require(z.mesh.faces.size() >= 5, "the bar has zone-side faces (the entrance steps, the landing)");
			test::require(z.report.overlapArea < 0.05, "no two zone-side faces overlap");
			// the seam: both sides carry it on the mask planes, equal in the plan (0.1 u: one sliver of half a unit ends 0.07 u apart) and in length
			test::require(z.report.seamEdges > 0 && r.report.seamEdges > 0, "both meshes have seam edges");
			const knav::surfgen::SeamGap gap = knav::surfgen::seamMismatch(r.mesh, z.mesh);
			std::cout << "seam gap: plan " << gap.xz << " u, height " << gap.y << " u\n";
			test::require(gap.xz < 0.1, "the seam is the same line on both sides (plan, 0.1 u)");
			test::require(gap.y < 0.1, "the two surfaces meet at the seam within 0.1 u in height");
			test::require(std::fabs(z.report.seamLength - r.report.seamLength) < 0.5, "the seam has the same length on both sides");
			// the door painter: the door's key on both meshes
			const kworld::WorldShape* door = nullptr;
			for (const kworld::WorldShape& s : *bar->shapes)
				if (s.use == kworld::NavUse::Door)
					door = &s;
			test::require(door != nullptr && door->door != 0, "the bar has a keyed door shape");
			for (const Result* side : std::array<const Result*, 2>{ &r, &z })
			{
				std::size_t keyed = 0;
				for (const knav::native::Face& f : side->mesh.faces)
				{
					test::require((f.data == 4) == (f.door != 0), "a face carries the door's key exactly when it is a door face");
					keyed += f.door == door->door ? 1 : 0;
				}
				test::require(keyed >= 1, "the door's key is on the face");
			}
			test::require(r.report.doorArea > 10.0 && r.report.doorArea < 20.0, "the interior's door area: the doorway's width by its depth (our bake: 13.8 u2)");
			// the entrance steps: planar to 0.1 u; the top (42.3 deg) is a handful of faces of one plane, the biggest holding most of its area
			double stepsArea = 0, biggest = 0;
			std::size_t stepFaces = 0;
			for (const knav::native::Face& f : z.mesh.faces)
			{
				const P3 &a = z.mesh.vertices[z.mesh.edges[f.startEdge].a], &b = z.mesh.vertices[z.mesh.edges[f.startEdge + 1].a], &c = z.mesh.vertices[z.mesh.edges[f.startEdge + 2].a];
				const P3 u{ b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				const P3 n{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
				const double slope = std::acos(std::fabs(n[1]) / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])) * 180.0 / 3.14159265358979;
				if (std::fabs(slope - 42.3) > 1.0)
					continue;
				const double ar = area3d(z.mesh, f);
				stepsArea += ar;
				biggest = std::max(biggest, ar);
				++stepFaces;
				const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				for (std::uint32_t k = 3; k < f.numEdges; ++k)
				{
					const P3& q = z.mesh.vertices[z.mesh.edges[f.startEdge + k].a];
					test::require(std::fabs(((q[0] - a[0]) * n[0] + (q[1] - a[1]) * n[1] + (q[2] - a[2]) * n[2]) / len) < 0.1, "a steps face is planar within 0.1 u");
				}
			}
			std::cout << "entrance steps: " << stepFaces << " faces, " << stepsArea << " u2, the biggest " << biggest << "\n";
			test::require(stepFaces >= 1 && biggest > 0.7 * stepsArea, "the entrance steps are one strip with a few edge pieces: the biggest face holds >70% of the area");
		}
		// the Desert Paradise Longhouse: record 4028 placed at the original's pose
		{
			kworld::PlacedInstance placed;
			placed.instanceID = "placed-longhouse";
			placed.buildingID = "4028-TwoStorey.mod";
			placed.x = 29990.818; placed.y = 1016.181; placed.z = -83488.695;
			placed.qw = 0.5578; placed.qx = -0.0169; placed.qy = 0.8295; placed.qz = -0.0214;
			placed.scale = 1.0f;
			placed.hasScale = true;
			placed.cell = placed.positionCell = kworld::zoneCellOf(placed.x, placed.z);
			// placed in its own frame (kworld::AreaBuilding::localShapes), the pose carries the mesh into the world
			kworld::PlacedInstance local = placed;
			local.x = local.y = local.z = 0.0;
			local.qw = 1.0;
			local.qx = local.qy = local.qz = 0.0;
			std::vector<kworld::WorldShape> shapes, mask;
			kworld::CollisionCoverage coverage;
			test::require(library.addBuilding(local, shapes, coverage).has_value(), "the Longhouse record has collision");
			test::require(library.addInteriorMask(local, mask) && mask.size() == 3, "the Longhouse mask is three boxes");
			const Result r = run(shapes, mask, placed);
			common("longhouse", r);
			// our own bake: 62,286 u2 (3D), 76 % of the floor reached (the rest is roof and ledges the entry does not reach)
			test::require(r.area > 55000.0, "the Longhouse's floors are there (our bake: 62,286 u2)");
			test::require(r.report.reachedShare > 0.7 && !r.report.seedSuspect, "the entry reaches the building (our bake: 76 %)");
			// the upper floor (y 1080..1100 in the world) is connected to the ground floor by the stair
			double upper = 0;
			for (const knav::native::Face& f : r.mesh.faces)
				if (r.mesh.vertices[r.mesh.edges[f.startEdge].a][1] > 1075.0 && r.mesh.vertices[r.mesh.edges[f.startEdge].a][1] < 1100.0)
					upper += area3d(r.mesh, f);
			test::require(upper > 15000.0, "the Longhouse upper floor is in the one connected mesh");
		}
		std::cout << "knav_surfgen ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_surfgen: " << e.what() << "\n";
		return 1;
	}
}
