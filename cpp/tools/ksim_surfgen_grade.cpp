// ksim_surfgen_grade: runs the geometric navmesh generator (knav/surfgen.hpp, kenshi-k16.7.131) on one building and grades the result
// (dev only, KSIM_DEV). Cases are those of ksim_navcoll_export:
//
//   ksim_surfgen_grade hubbar|longhouse [--side interior|zone] [--islands seam] [--capture DIR --instance N] [--out DIR] [--cell SIZE]
//
// --side zone: grades the building's zone-side faces (stage 2, kenshi-k16.7.143) instead of the interior; against a capture only the faces of face_data 1 / 4.
//
// --capture DIR: the original's live export (nav_faces.tsv of kenshi-rwh.265 / rwh.270), --instance N: the runtime id of the building's own
// interior mesh (face_id >> 22); the generated mesh is compared with it by xz area (symmetric difference on a grid of --cell, default 0.5 u).
// --out DIR: writes nav_faces.tsv (the capture's format, face ids 0..n-1) for tools/navcoll_render.py.
// Game-derived geometry: never commit the output.

#include "kdata/gamedata.hpp"
#include "knav/area.hpp"
#include "knav/surfgen.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using P3 = std::array<double, 3>;

namespace
{

struct Case
{
	const char* name;
	double x, z;
	const char* record;
	const char* instance;
	bool place;
	double pos[3];
	double quat[4];
};

const Case kCases[] = {
	{ "hubbar", -51181.133, 2638.970, "", "4933-Newwworld", false, {}, {} },
	{ "longhouse", 29990.818, -83488.695, "4028-TwoStorey.mod", "", true, { 29990.818, 1016.181, -83488.695 }, { 0.5578, -0.0169, 0.8295, -0.0214 } },
};

// The capture's faces of one instance; walkableOnly: only face_data 1 / 4 (the building's material and the door's: not the terrain's -1).
std::vector<std::vector<P3>> loadFaces(const std::string& path, long instance, bool walkableOnly = false)
{
	std::set<long> keep;
	if (walkableOnly)
	{
		std::ifstream data(std::filesystem::path(path).replace_filename("nav_face_data.tsv"));
		std::string row;
		while (std::getline(data, row))
		{
			std::istringstream cells(row);
			std::string id, key, value;
			std::getline(cells, id, '\t');
			std::getline(cells, key, '\t');
			std::getline(cells, value, '\t');
			if (key == "face_data" && (value == "1" || value == "4"))
				keep.insert(std::stol(id));
		}
	}
	std::vector<std::vector<P3>> faces;
	std::ifstream in(path);
	std::string line;
	std::getline(in, line);
	while (std::getline(in, line))
	{
		std::istringstream row(line);
		std::string cell;
		std::getline(row, cell, '\t');
		const long id = std::stol(cell);
		if ((instance >= 0 && (id >> 22) != instance) || (walkableOnly && keep.count(id) == 0))
			continue;
		std::getline(row, cell, '\t');
		const int count = std::stoi(cell);
		std::vector<P3> ring;
		for (int i = 0; i < count && std::getline(row, cell, '\t'); ++i)
		{
			std::istringstream xyz(cell);
			P3 p{};
			xyz >> p[0] >> p[1] >> p[2];
			ring.push_back(p);
		}
		faces.push_back(std::move(ring));
	}
	return faces;
}

double area3(const std::vector<P3>& ring)
{
	double sx = 0, sy = 0, sz = 0;
	for (std::size_t i = 0; i < ring.size(); ++i)
	{
		const P3 &a = ring[i], &b = ring[(i + 1) % ring.size()];
		sx += a[1] * b[2] - a[2] * b[1];
		sy += a[2] * b[0] - a[0] * b[2];
		sz += a[0] * b[1] - a[1] * b[0];
	}
	return 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
}

bool inside(const std::vector<P3>& ring, double x, double z)
{
	bool in = false;
	for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
		if ((ring[i][2] > z) != (ring[j][2] > z) && x < (ring[j][0] - ring[i][0]) * (z - ring[i][2]) / (ring[j][2] - ring[i][2]) + ring[i][0])
			in = !in;
	return in;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2)
		{
			std::cerr << "usage: ksim_surfgen_grade hubbar|longhouse [--side interior|zone] [--islands seam] [--capture DIR --instance N] [--out DIR] [--cell SIZE]\n";
			return 2;
		}
		const Case* spec = nullptr;
		for (const Case& c : kCases)
			if (std::string(argv[1]) == c.name)
				spec = &c;
		if (spec == nullptr)
		{
			std::cerr << "unknown case " << argv[1] << "\n";
			return 2;
		}
		std::string capture, out;
		bool zoneSide = false;
		knav::surfgen::Settings settings;
		long instance = -1;
		double cell = 0.5;
		for (int i = 2; i + 1 < argc; i += 2)
		{
			const std::string a = argv[i];
			if (a == "--capture") capture = argv[i + 1];
			else if (a == "--instance") instance = std::atol(argv[i + 1]);
			else if (a == "--out") out = argv[i + 1];
			else if (a == "--cell") cell = std::atof(argv[i + 1]);
			else if (a == "--islands") settings.pruneZoneIslands = std::string(argv[i + 1]) == "seam";
			else if (a == "--side") zoneSide = std::string(argv[i + 1]) == "zone";
		}
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library{ dataDir, gd };
		const kworld::ZoneCell zone = kworld::zoneCellOf(spec->x, spec->z);
		kworld::AreaGeometry area = knav::gatherArea(dataDir, terrain, zone, zone);
		kworld::PlacedInstance placed;
		std::vector<kworld::WorldShape> shapes;
		if (spec->place)
		{
			placed.instanceID = std::string("placed-") + spec->name;
			placed.buildingID = spec->record;
			placed.x = spec->pos[0]; placed.y = spec->pos[1]; placed.z = spec->pos[2];
			placed.qw = spec->quat[0]; placed.qx = spec->quat[1]; placed.qy = spec->quat[2]; placed.qz = spec->quat[3];
			placed.scale = 1.0f;
			placed.hasScale = gd.find(spec->record) != nullptr;
			placed.cell = placed.positionCell = zone;
			library.addBuilding(placed, shapes, area.coverage);
		} else {
			const kworld::AreaBuilding* best = nullptr;
			for (const kworld::AreaBuilding& b : area.buildings)
				if (!b.shapes->empty() && b.placed.instanceID.find(spec->instance) != std::string::npos)
					best = &b;
			if (best == nullptr)
			{
				std::cerr << "no building\n";
				return 1;
			}
			placed = best->placed;
			shapes = *best->shapes;
		}
		std::vector<kworld::WorldShape> mask;
		library.addInteriorMask(placed, mask);

		knav::surfgen::Input input;
		std::size_t furniture = 0;
		for (const kworld::WorldShape& s : shapes)
		{
			input.shapes.push_back(&s);
			furniture += s.furniture ? 1 : 0;
		}
		for (const kworld::WorldShape& m : mask)
			input.mask.push_back(&m);
		knav::surfgen::Report report, otherReport;
		const auto t0 = std::chrono::steady_clock::now();
		input.side = zoneSide ? knav::surfgen::Side::Zone : knav::surfgen::Side::Interior;
		const knav::native::Mesh mesh = knav::surfgen::generate(input, settings, &report);
		input.side = zoneSide ? knav::surfgen::Side::Interior : knav::surfgen::Side::Zone;
		const knav::native::Mesh other = knav::surfgen::generate(input, settings, &otherReport);
		input.side = zoneSide ? knav::surfgen::Side::Zone : knav::surfgen::Side::Interior;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		std::vector<std::vector<P3>> ours;
		for (const knav::native::Face& f : mesh.faces)
		{
			std::vector<P3> ring;
			for (std::uint32_t k = 0; k < f.numEdges; ++k)
				ring.push_back(mesh.vertices[mesh.edges[f.startEdge + k].a]);
			ours.push_back(std::move(ring));
		}
		double area3d = 0;
		for (const auto& r : ours)
			area3d += area3(r);
		std::printf("%s: %zu shapes (%zu furniture, %zu mask), %.0f ms\n", spec->name, shapes.size(), furniture, mask.size(), ms);
		std::printf("  faces %zu (pieces before merge %zu), xz area %.0f, 3D area %.0f, matched edge pairs %zu, boundary edges %zu\n", report.faces, report.pieces,
			report.area, area3d, report.matchedEdges, report.boundaryEdges);
		std::printf("  %s side; seam %zu edges / %.1f u (other side %zu / %.1f u), seams differ by at most %.4f u in the plan, %.4f u in height; door faces %zu (%.1f u2), other side %zu (%.1f u2)\n",
			zoneSide ? "zone" : "interior", report.seamEdges, report.seamLength, otherReport.seamEdges, otherReport.seamLength, knav::surfgen::seamMismatch(mesh, other).xz, knav::surfgen::seamMismatch(mesh, other).y,
			report.doorFaces, report.doorArea, otherReport.doorFaces, otherReport.doorArea);
		if (std::getenv("SEAM_DUMP"))
			for (const auto* m : { &mesh, &other })
			{
				std::printf("  seam of %s:\n", m == &mesh ? "this side" : "other side");
				for (const knav::native::Edge& e : m->edges)
					if (e.linked)
						std::printf("    (%.3f %.3f %.3f) (%.3f %.3f %.3f)\n", m->vertices[e.a][0], m->vertices[e.a][1], m->vertices[e.a][2], m->vertices[e.b][0], m->vertices[e.b][1], m->vertices[e.b][2]);
			}
		std::printf("  components before pruning %zu, dropped %zu faces / %.0f u2, steep (>48 deg) %.0f, overlap area %.3f\n", report.components, report.droppedFaces,
			report.droppedArea, report.steepFaces, report.overlapArea);
		// boundary (wall) edges: distance to the furniture footprints and the share parallel to the building's axes
		{
			using P2 = std::array<double, 2>;
			std::vector<std::vector<P2>> footprints;
			for (const kworld::WorldShape& sh : shapes)
			{
				if (!sh.furniture)
					continue;
				std::vector<P2> pts;
				for (std::size_t i = 0; i + 2 < sh.vertices.size(); i += 3)
					pts.push_back({ sh.vertices[i], sh.vertices[i + 2] });
				std::sort(pts.begin(), pts.end());
				std::vector<P2> h(2 * pts.size());
				std::size_t k = 0;
				const auto cr = [](const P2& a, const P2& b, const P2& c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); };
				for (std::size_t i = 0; i < pts.size(); ++i)
				{
					while (k >= 2 && cr(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
					h[k++] = pts[i];
				}
				for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
				{
					while (k >= t && cr(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
					h[k++] = pts[i - 1];
				}
				if (k > 1) { h.resize(k - 1); footprints.push_back(std::move(h)); }
			}
			// the building's axes: the 1-degree bin (mod 90) holding most boundary length
			std::array<double, 90> bins{};
			for (std::size_t e = 0; e < mesh.edges.size(); ++e)
				if (mesh.wall(e))
				{
					const auto& a = mesh.vertices[mesh.edges[e].a];
					const auto& b = mesh.vertices[mesh.edges[e].b];
					double deg = std::fmod(std::atan2(b[2] - a[2], b[0] - a[0]) * 180.0 / 3.14159265358979 + 900.0, 90.0);
					bins[static_cast<std::size_t>(deg) % 90] += std::hypot(b[0] - a[0], b[2] - a[2]);
				}
			const double yaw = (static_cast<double>(std::max_element(bins.begin(), bins.end()) - bins.begin()) + 0.5) * 3.14159265358979 / 180.0;
			std::vector<double> near;
			double axisLength = 0, wallLength = 0;
			for (std::size_t e = 0; e < mesh.edges.size(); ++e)
			{
				if (!mesh.wall(e))
					continue;
				const auto& a = mesh.vertices[mesh.edges[e].a];
				const auto& b = mesh.vertices[mesh.edges[e].b];
				const double length = std::hypot(b[0] - a[0], b[2] - a[2]);
				if (length < 1e-6)
					continue;
				wallLength += length;
				double angle = std::fmod(std::atan2(b[2] - a[2], b[0] - a[0]) - yaw + 40.0 * 3.14159265358979, 3.14159265358979 / 2.0);
				if (angle < 0.052 || angle > 3.14159265358979 / 2.0 - 0.052)
					axisLength += length;
				for (double t = 0.25 * std::min(length, 0.5); t < length; t += 0.5)
				{
					const double x = a[0] + (b[0] - a[0]) * t / length, z = a[2] + (b[2] - a[2]) * t / length;
					double best = 1e30;
					for (const auto& fp : footprints)
						for (std::size_t i = 0; i < fp.size(); ++i)
						{
							const P2 &p0 = fp[i], &p1 = fp[(i + 1) % fp.size()];
							const double l2 = (p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1]);
							const double u = l2 > 0 ? std::clamp(((x - p0[0]) * (p1[0] - p0[0]) + (z - p0[1]) * (p1[1] - p0[1])) / l2, 0.0, 1.0) : 0.0;
							best = std::min(best, std::hypot(x - p0[0] - u * (p1[0] - p0[0]), z - p0[1] - u * (p1[1] - p0[1])));
						}
					if (best < 1.0)
						near.push_back(best);
				}
			}
			std::sort(near.begin(), near.end());
			std::printf("  boundary: %.0f u, axis-parallel %.0f%%, %zu samples within 1 u of a furniture footprint, median distance %.3f u\n", wallLength,
				wallLength > 0 ? 100.0 * axisLength / wallLength : 0.0, near.size(), near.empty() ? -1.0 : near[near.size() / 2]);
		}
		{
			// the ground floor: the faces of the shape that owns the largest face, against that face's plane (Newell normal)
			std::size_t big = 0;
			for (std::size_t i = 0; i < ours.size(); ++i)
				big = area3(ours[i]) > area3(ours[big]) ? i : big;
			double nx = 0, ny = 0, nz = 0, cx = 0, cy = 0, cz = 0;
			const auto& ring = ours[big];
			for (std::size_t i = 0; i < ring.size(); ++i)
			{
				const P3 &a = ring[i], &b = ring[(i + 1) % ring.size()];
				nx += (a[1] - b[1]) * (a[2] + b[2]);
				ny += (a[2] - b[2]) * (a[0] + b[0]);
				nz += (a[0] - b[0]) * (a[1] + b[1]);
				cx += a[0] / ring.size(); cy += a[1] / ring.size(); cz += a[2] / ring.size();
			}
			double worst = 0;
			std::size_t count = 0;
			for (std::size_t i = 0; i < ours.size(); ++i)
				if (report.faceShape[i] == report.faceShape[big])
				{
					++count;
					for (const P3& p : ours[i])
						worst = std::max(worst, std::fabs(((p[0] - cx) * nx + (p[1] - cy) * ny + (p[2] - cz) * nz) / std::sqrt(nx * nx + ny * ny + nz * nz)));
				}
			std::printf("  floor of the largest face's shape: %zu faces, worst vertex distance from the largest face's plane %.3f u\n", count, worst);
		}
		std::printf("  component areas (before pruning):");
		for (const double a : report.componentAreas)
			std::printf(" %.0f", a);
		std::printf("\n");
		if (!out.empty())
		{
			std::filesystem::create_directories(out);
			std::ofstream tsv(std::filesystem::path(out) / "nav_faces.tsv");
			tsv << "face_id\tvertex_count\txyz...\n";
			for (std::size_t i = 0; i < ours.size(); ++i)
			{
				tsv << i << "\t" << ours[i].size();
				for (const P3& p : ours[i])
					tsv << "\t" << p[0] << " " << p[1] << " " << p[2];
				tsv << "\n";
			}
			std::ofstream owners(std::filesystem::path(out) / "face_shapes.tsv");
			for (std::size_t i = 0; i < report.faceShape.size(); ++i)
				owners << i << "\t" << report.faceShape[i] << "\n";
		}
		if (!capture.empty())
		{
			const auto theirs = loadFaces(capture + "/nav_faces.tsv", instance, zoneSide);
			double theirArea = 0, theirArea3 = 0;
			double minx = 1e30, maxx = -1e30, minz = 1e30, maxz = -1e30;
			for (const auto& r : theirs)
			{
				theirArea3 += area3(r);
				for (const P3& p : r)
				{
					minx = std::min(minx, p[0]); maxx = std::max(maxx, p[0]);
					minz = std::min(minz, p[2]); maxz = std::max(maxz, p[2]);
				}
			}
			for (const auto& r : ours)
				for (const P3& p : r)
				{
					minx = std::min(minx, p[0]); maxx = std::max(maxx, p[0]);
					minz = std::min(minz, p[2]); maxz = std::max(maxz, p[2]);
				}
			double both = 0, onlyOurs = 0, onlyTheirs = 0;
			for (double x = minx; x < maxx; x += cell)
				for (double z = minz; z < maxz; z += cell)
				{
					bool a = false, b = false;
					for (const auto& r : ours) a = a || inside(r, x + cell / 2, z + cell / 2);
					for (const auto& r : theirs) b = b || inside(r, x + cell / 2, z + cell / 2);
					(a && b ? both : a ? onlyOurs : onlyTheirs) += b || a ? cell * cell : 0.0;
				}
			(void)theirArea;
			std::printf("  capture: %zu faces, 3D area %.0f; xz coverage both %.0f, only ours %.0f, only capture %.0f u2\n", theirs.size(), theirArea3, both, onlyOurs,
				onlyTheirs);
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_surfgen_grade: " << e.what() << "\n";
		return 1;
	}
}
