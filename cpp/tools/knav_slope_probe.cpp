// knav_slope_probe: how many walkable faces does the open-sheet slope rule (bake.cpp shapeSlope, k16.7.58) turn walkable, and does
// the original's shipped navtile walk them? World-wide over the placed shapes (the bake's own gather):
//   knav_slope_probe OUT.tsv [--regions RX0 RZ0 RX1 RZ1] [--lo DEG] [--hi DEG]
// A face counts when its shape is an open walkable sheet (use Walkable, not closed, not solid) and its upward normal is between
// --lo (default 40, the walk limit) and --hi (default 55, the stair limit). It is "shipped-walked" when a polygon of the shipped
// tile of its zone covers its centroid in xz (1.5 u slack) and lies within 8 u of it in y. One line per building (or foliage):
// id, zone of the first face, faces, horizontal area, walked faces and area; totals on stdout.

#include "knav/area.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kXzSlack = 1.5;
constexpr double kYSlack = 8.0;

using Tile = std::vector<knav::native::Mesh>;

// Does one polygon of a shipped mesh cover (x, z) within the slack, and how high is its plane there?
bool polygonAt(const knav::native::Mesh& m, const knav::native::Face& face, double x, double z, double y, double& dy)
{
	std::vector<std::array<double, 3>> p;
	for (std::uint32_t e = face.startEdge; e < face.startEdge + face.numEdges; ++e)
		p.push_back(m.vertices[m.edges[e].a]);
	if (p.size() < 3)
		return false;
	bool inside = false;
	double edgeDist = 1e30;
	for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
	{
		const double ax = p[j][0], az = p[j][2], bx = p[i][0], bz = p[i][2];
		if ((az > z) != (bz > z) && x < (bx - ax) * (z - az) / (bz - az) + ax)
			inside = !inside;
		const double dx = bx - ax, dz = bz - az;
		const double len2 = dx * dx + dz * dz;
		const double t = len2 > 0 ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0) : 0.0;
		edgeDist = std::min(edgeDist, std::hypot(x - (ax + t * dx), z - (az + t * dz)));
	}
	if (!inside && edgeDist > kXzSlack)
		return false;
	// Plane through the first non-degenerate triple (xz area > 0); y at (x, z), else the mean height.
	double mean = 0.0;
	for (const auto& v : p)
		mean += v[1] / static_cast<double>(p.size());
	double py = mean;
	for (std::size_t k = 2; k < p.size(); ++k)
	{
		const double ux = p[k - 1][0] - p[0][0], uz = p[k - 1][2] - p[0][2], vx = p[k][0] - p[0][0], vz = p[k][2] - p[0][2];
		const double det = ux * vz - uz * vx;
		if (std::fabs(det) < 1e-6)
			continue;
		const double s = ((x - p[0][0]) * vz - (z - p[0][2]) * vx) / det;
		const double t = ((z - p[0][2]) * ux - (x - p[0][0]) * uz) / det;
		py = p[0][1] + s * (p[k - 1][1] - p[0][1]) + t * (p[k][1] - p[0][1]);
		break;
	}
	dy = std::fabs(py - y);
	return true;
}

struct Totals
{
	std::size_t faces = 0, walked = 0;
	double area = 0.0, walkedArea = 0.0;
};


// One connected strip of band faces (shared vertices) of a shape: what it joins.
struct Strip
{
	std::vector<std::array<int, 2>> faces; // (index into the shape's tris, walked)
};

using Key = std::array<long, 3>;

Key keyOf(const float* v)
{
	return { std::lround(v[0] * 4.0), std::lround(v[1] * 4.0), std::lround(v[2] * 4.0) };
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: knav_slope_probe OUT.tsv [--regions RX0 RZ0 RX1 RZ1] [--lo DEG] [--hi DEG]\n";
		return 2;
	}
	int regions[4] = { 0, 0, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1 };
	double lo = 40.0, hi = 55.0;
	std::string stripsPath = "/dev/null";
	for (int i = 2; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--regions" && i + 4 < argc)
		{
			for (int& r : regions)
				r = std::atoi(argv[++i]);
		}
		else if (arg == "--strips" && i + 1 < argc)
			stripsPath = argv[++i];
		else if (arg == "--lo" && i + 1 < argc)
			lo = std::atof(argv[++i]);
		else if (arg == "--hi" && i + 1 < argc)
			hi = std::atof(argv[++i]);
	}
	const double upMax = std::cos(lo * kPi / 180.0), upMin = std::cos(hi * kPi / 180.0);
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		knav::AreaGatherer gatherer(dataDir, terrain);
		const std::string navtiles = knav::native::navtilesDir(dataDir);
		std::map<std::pair<int, int>, Tile> tiles;
		const auto tile = [&](int i, int j) -> const Tile&
		{
			const auto key = std::make_pair(i, j);
			auto found = tiles.find(key);
			if (found == tiles.end())
				found = tiles.emplace(key, knav::native::loadTile(navtiles, i, j)).first;
			return found->second;
		};
		std::ofstream out(argv[1]);
		std::ofstream strips(stripsPath);
		strips << "id\tzone_x\tzone_z\tfaces\tarea_u2\twalked_area_u2\tslope_mean\trise\tbottom_y\ttop_flat\tbottom_flat\tshape_tris\ttop_walked\tbottom_walked\ttop_ground\tbottom_ground\n";
		out << "id\tzone_x\tzone_z\ty\tfaces\tarea_u2\twalked_faces\twalked_area_u2\tno_tile_faces\n";
		Totals total;
		std::size_t noTile = 0, shapesHit = 0;
		for (int rz = regions[1]; rz <= regions[3]; ++rz)
		{
			for (int rx = regions[0]; rx <= regions[2]; ++rx)
			{
				const kworld::ZoneCell min{ rx * knav::kRegionZones, rz * knav::kRegionZones };
				const kworld::ZoneCell max{ min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1 };
				const knav::AreaGeometry area = gatherer.gather(min, max);
				const auto scan = [&](const std::string& id, const std::vector<kworld::WorldShape>& shapes)
				{
					Totals t;
					std::size_t missing = 0;
					int zx = 0, zz = 0;
					double yMean = 0.0;
					bool hit = false;
					for (const kworld::WorldShape& s : shapes)
					{
						if (s.use != kworld::NavUse::Walkable || s.closed || s.solid)
							continue;
						for (std::size_t k = 0; k + 2 < s.indices.size(); k += 3)
						{
							const float* a = &s.vertices[s.indices[k] * 3];
							const float* b = &s.vertices[s.indices[k + 1] * 3];
							const float* c = &s.vertices[s.indices[k + 2] * 3];
							const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
							const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
							const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
							if (len < 1e-6)
								continue;
							const double up = n[1] / len;
							if (!(up > upMin && up <= upMax))
								continue;
							const double cx = (a[0] + b[0] + c[0]) / 3.0, cy = (a[1] + b[1] + c[1]) / 3.0, cz = (a[2] + b[2] + c[2]) / 3.0;
							const int fx = static_cast<int>(std::floor((cx - kworld::kWorldMin) / kworld::kZoneSize));
							const int fz = static_cast<int>(std::floor((cz - kworld::kWorldMin) / kworld::kZoneSize));
							if (fx < min.x || fx > max.x || fz < min.z || fz > max.z)
								continue; // counted by the region that owns the zone
							const double hArea = std::fabs(e1[0] * e2[2] - e1[2] * e2[0]) / 2.0;
							++t.faces;
							t.area += hArea;
							if (!hit)
							{
								hit = true;
								zx = fx;
								zz = fz;
								yMean = cy;
							}
							const Tile& st = tile(fx, fz);
							if (st.empty())
							{
								++missing;
								continue;
							}
							bool walked = false;
							for (const knav::native::Mesh& m : st)
							{
								for (const knav::native::Face& f : m.faces)
								{
									double dy = 0.0;
									if (polygonAt(m, f, cx, cz, cy, dy) && dy <= kYSlack)
									{
										walked = true;
										break;
									}
								}
								if (walked)
									break;
							}
							if (walked)
							{
								++t.walked;
								t.walkedArea += hArea;
							}
						}
					}
					for (const kworld::WorldShape& s : shapes)
					{
						if (s.use != kworld::NavUse::Walkable || s.closed || s.solid)
							continue;
						const std::size_t nTri = s.indices.size() / 3;
						std::vector<double> up(nTri, 0.0);
						std::vector<int> parent(nTri);
						std::iota(parent.begin(), parent.end(), 0);
						const auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
						std::map<Key, int> owner;
						std::map<Key, std::vector<int>> flat;
						for (std::size_t k = 0; k < nTri; ++k)
						{
							const float* v[3] = { &s.vertices[s.indices[3 * k] * 3], &s.vertices[s.indices[3 * k + 1] * 3], &s.vertices[s.indices[3 * k + 2] * 3] };
							const double e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] }, e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
							const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
							const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
							up[k] = len < 1e-6 ? 0.0 : n[1] / len;
							const bool inBand = up[k] > upMin && up[k] <= upMax;
							for (const float* c : v)
							{
								if (up[k] > upMax)
									flat[keyOf(c)].push_back(static_cast<int>(k));
								if (!inBand)
									continue;
								const auto [it, fresh] = owner.emplace(keyOf(c), static_cast<int>(k));
								if (!fresh)
									parent[find(static_cast<int>(k))] = find(it->second);
							}
						}
						struct Agg { int faces = 0; double area = 0, slope = 0, lo = 1e30, hi = -1e30; std::vector<const float*> pts; double cx = 0, cz = 0; };
						std::map<int, Agg> comps;
						for (std::size_t k = 0; k < nTri; ++k)
						{
							if (!(up[k] > upMin && up[k] <= upMax))
								continue;
							Agg& g = comps[find(static_cast<int>(k))];
							const float* v[3] = { &s.vertices[s.indices[3 * k] * 3], &s.vertices[s.indices[3 * k + 1] * 3], &s.vertices[s.indices[3 * k + 2] * 3] };
							++g.faces;
							g.area += std::fabs((v[1][0] - v[0][0]) * (v[2][2] - v[0][2]) - (v[1][2] - v[0][2]) * (v[2][0] - v[0][0])) / 2.0;
							g.slope += std::acos(std::min(up[k], 1.0)) * 180.0 / kPi;
							for (const float* c : v)
							{
								g.lo = std::min<double>(g.lo, c[1]);
								g.hi = std::max<double>(g.hi, c[1]);
								g.pts.push_back(c);
								g.cx += c[0] / 3.0;
								g.cz += c[2] / 3.0;
							}
						}
						for (auto& [root, g] : comps)
						{
							bool topGround = false, botGround = false, topFlat = false, botFlat = false, topWalked = false, botWalked = false;
							const auto triWalked = [&](int k)
							{
								const float* v[3] = { &s.vertices[s.indices[3 * k] * 3], &s.vertices[s.indices[3 * k + 1] * 3], &s.vertices[s.indices[3 * k + 2] * 3] };
								const double cx = (v[0][0] + v[1][0] + v[2][0]) / 3.0, cy = (v[0][1] + v[1][1] + v[2][1]) / 3.0, cz = (v[0][2] + v[1][2] + v[2][2]) / 3.0;
								const int tx = static_cast<int>(std::floor((cx - kworld::kWorldMin) / kworld::kZoneSize));
								const int tz = static_cast<int>(std::floor((cz - kworld::kWorldMin) / kworld::kZoneSize));
								for (const knav::native::Mesh& m : tile(tx, tz))
									for (const knav::native::Face& f : m.faces)
									{
										double dy = 0.0;
										if (polygonAt(m, f, cx, cz, cy, dy) && dy <= kYSlack)
											return true;
									}
								return false;
							};
							for (const float* c : g.pts)
							{
								const bool onGround = std::fabs(terrain.terrainHeight(c[0], c[2]) - c[1]) <= 3.0;
								topGround = topGround || (onGround && c[1] >= g.hi - 1.0);
								botGround = botGround || (onGround && c[1] <= g.lo + 1.0);
								const auto isFlat = flat.find(keyOf(c));
								if (isFlat == flat.end())
									continue;
								const bool top = c[1] >= g.hi - 1.0, bot = c[1] <= g.lo + 1.0;
								topFlat = topFlat || top;
								botFlat = botFlat || bot;
								if ((top && !topWalked) || (bot && !botWalked))
									for (const int k : isFlat->second)
										if (triWalked(k))
										{
											topWalked = topWalked || top;
											botWalked = botWalked || bot;
											break;
										}
							}
							const double mx = g.cx / g.faces, mz = g.cz / g.faces;
							const int fx = static_cast<int>(std::floor((mx - kworld::kWorldMin) / kworld::kZoneSize));
							const int fz = static_cast<int>(std::floor((mz - kworld::kWorldMin) / kworld::kZoneSize));
							if (fx < min.x || fx > max.x || fz < min.z || fz > max.z)
								continue;
							double wa = 0.0;
							const Tile& st = tile(fx, fz);
							for (std::size_t k = 0; k < nTri; ++k)
							{
								if (!(up[k] > upMin && up[k] <= upMax) || find(static_cast<int>(k)) != root)
									continue;
								const float* v[3] = { &s.vertices[s.indices[3 * k] * 3], &s.vertices[s.indices[3 * k + 1] * 3], &s.vertices[s.indices[3 * k + 2] * 3] };
								const double cx = (v[0][0] + v[1][0] + v[2][0]) / 3.0, cy = (v[0][1] + v[1][1] + v[2][1]) / 3.0, cz = (v[0][2] + v[1][2] + v[2][2]) / 3.0;
								bool walked = false;
								for (const knav::native::Mesh& m : st)
								{
									for (const knav::native::Face& f : m.faces)
									{
										double dy = 0.0;
										if (polygonAt(m, f, cx, cz, cy, dy) && dy <= kYSlack)
										{
											walked = true;
											break;
										}
									}
									if (walked)
										break;
								}
								if (walked)
									wa += std::fabs((v[1][0] - v[0][0]) * (v[2][2] - v[0][2]) - (v[1][2] - v[0][2]) * (v[2][0] - v[0][0])) / 2.0;
							}
							strips << id << '\t' << fx << '\t' << fz << '\t' << g.faces << '\t' << g.area << '\t' << wa << '\t' << g.slope / g.faces << '\t'
								<< g.hi - g.lo << '\t' << g.lo << '\t' << topFlat << '\t' << botFlat << '\t' << nTri << '\t' << topWalked << '\t' << botWalked << '\t' << topGround << '\t' << botGround << '\n';
						}
					}
					if (t.faces == 0)
						return;
					++shapesHit;
					out << id << '\t' << zx << '\t' << zz << '\t' << yMean << '\t' << t.faces << '\t' << t.area << '\t' << t.walked << '\t'
						<< t.walkedArea << '\t' << missing << '\n';
					total.faces += t.faces;
					total.area += t.area;
					total.walked += t.walked;
					total.walkedArea += t.walkedArea;
					noTile += missing;
				};
				for (const knav::AreaBuilding& building : area.buildings)
					if (building.shapes)
						scan(building.id, *building.shapes);
				if (area.foliage)
					scan("foliage@" + std::to_string(rx) + "," + std::to_string(rz), *area.foliage);
			}
			std::cerr << "row " << rz << " done\n";
		}
		std::cout << "shapes " << shapesHit << ", faces " << total.faces << " (" << total.area << " u2 horizontal), shipped-walked " << total.walked
			<< " (" << total.walkedArea << " u2), faces in zones with no shipped tile " << noTile << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_slope_probe: " << e.what() << "\n";
		return 1;
	}
}
