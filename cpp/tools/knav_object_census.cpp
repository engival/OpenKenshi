// knav_object_census: every placed world object class that is not a building with an interior mask, measured against our ground grid
// (kenshi-k16.7.187). Facts only: where it comes from, its collision, how it sits on the terrain, what the ground cut does with it.
//   knav_object_census OUT_DIR [DATA_DIR]
// Writes OUT_DIR/{records,classes,features,foliage}.tsv. The terrain is knav::ground's grid (the original's 36 u zone samples, the diagonal
// with the smaller height step) and the cut test replays knav::ground's cutter rule (ground + 0.1 .. ground + 18) at sample points.
#include "kdata/gamedata.hpp"
#include "knav/ground.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld/zones.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kworld::WorldShape;

// The ground the cutter rule sees: knav::ground::makeGrid's samples (stride 2 = 36 u), two triangles a cell split on the fixed a-d diagonal
// (ground.cpp cellSplit).
struct Ground
{
	const kworld::Heightmap& t;

	double sample(int i, int j) const
	{
		const int cx = t.sampleCountX() - 1, cz = t.sampleCountZ() - 1;
		return t.sampleHeight(std::clamp(2 * i, 0, cx), std::clamp(2 * j, 0, cz));
	}

	double at(double x, double z) const
	{
		const double s = 2 * kworld::kHeightmapSpacing;
		const double fx = (x - kworld::kWorldMin) / s, fz = (z - kworld::kWorldMin) / s;
		const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fz));
		const double u = fx - i, v = fz - j;
		const double a = sample(i, j), b = sample(i + 1, j), c = sample(i, j + 1), d = sample(i + 1, j + 1);
		// a-d diagonal: (a, b, d) below it (u >= v), (a, d, c) above
		return u >= v ? a + u * (b - a) + v * (d - b) : a + u * (d - c) + v * (c - a);
	}
};

struct Meas
{
	std::size_t shapes = 0, boxes = 0, convex = 0, meshes = 0, tris = 0;
	std::size_t useWalk = 0, useObst = 0, useProj = 0;
	std::size_t upTris = 0;
	double upArea = 0, upAreaWalkUse = 0, upAreaLow = 0;	// up-facing (<= 60 deg) triangle area: all, in Walkable-use shapes, at <= 18 u over the ground
	double upHmin = 1e30, upHmax = -1e30, upHmed = 0;	// height of up-facing triangle centroids over the ground; upHmed area-weighted median
	double baseMean = 0, baseMin = 0, baseMax = 0;	// lowest vertices (within 1 u of the lowest) minus the ground at their xz
	double minY = 0, maxY = 0, height = 0;
	double footprint = 0, cutArea = 0, coverArea = 0;	// bbox xz area, the area the ground cut drops, the xz area any shape covers
	double originDelta = 0;	// the placement's y minus the ground at its x, z
	// per covered sample (a vertical line through the object): its lowest / highest surface minus the ground there; the median over samples
	double baseRel = 0, topRel = 0;
	double pUnder = 0, pThrough = 0, pOver = 0;	// share of samples: whole object below the ground / the ground inside it / the whole object above it
	std::string state;	// by baseRel: "sunk" (< -0.5), "floating" (> 0.5), else "flush"
	bool any = false;
};

double median(std::vector<double> v)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

Meas measure(const std::vector<WorldShape>& shapes, const Ground& ground, double ox, double oy, double oz)
{
	Meas m;
	double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30, y0 = 1e30, y1 = -1e30;
	std::vector<const WorldShape*> use;
	for (const WorldShape& s : shapes)
	{
		if (s.use == kworld::NavUse::Door || s.indices.size() < 3)
			continue;
		use.push_back(&s);
		x0 = std::min<double>(x0, s.min[0]);
		x1 = std::max<double>(x1, s.max[0]);
		z0 = std::min<double>(z0, s.min[2]);
		z1 = std::max<double>(z1, s.max[2]);
		y0 = std::min<double>(y0, s.min[1]);
		y1 = std::max<double>(y1, s.max[1]);
	}
	m.originDelta = oy - ground.at(ox, oz);
	if (use.empty())
		return m;
	m.any = true;
	m.minY = y0;
	m.maxY = y1;
	m.height = y1 - y0;
	m.footprint = (x1 - x0) * (z1 - z0);
	std::vector<std::pair<double, double>> upH;	// height over the ground, area
	for (const WorldShape* s : use)
	{
		++m.shapes;
		if (s->half[0] > 0 || s->half[1] > 0 || s->half[2] > 0)
			++m.boxes;
		else if (s->closed || s->solid)
			++m.convex;
		else
			++m.meshes;
		m.useWalk += s->use == kworld::NavUse::Walkable;
		m.useObst += s->use == kworld::NavUse::Obstacle;
		m.useProj += s->use == kworld::NavUse::Projected;
		for (std::size_t k = 0; k + 2 < s->indices.size(); k += 3)
		{
			std::array<std::array<double, 3>, 3> p;
			for (int c = 0; c < 3; ++c)
				for (int a = 0; a < 3; ++a)
					p[c][a] = s->vertices[s->indices[k + c] * 3 + a];
			const double ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
			const double vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
			++m.tris;
			if (len < 1e-12)
				continue;
			// a box / hull has outward (CCW) winding: its up faces are the +y ones. A triangle mesh is a surface with whatever winding the cooker
			// wrote, so a sheet counts when its normal is within 60 deg of either vertical
			const bool oneSided = s->half[0] > 0 || s->half[1] > 0 || s->half[2] > 0 || s->closed || s->solid;
			if ((oneSided ? ny : std::fabs(ny)) / len < 0.5)
				continue;
			const double cx = (p[0][0] + p[1][0] + p[2][0]) / 3, cy = (p[0][1] + p[1][1] + p[2][1]) / 3, cz = (p[0][2] + p[1][2] + p[2][2]) / 3;
			const double h = cy - ground.at(cx, cz), area = len / 2;
			++m.upTris;
			m.upArea += area;
			if (s->use == kworld::NavUse::Walkable)
				m.upAreaWalkUse += area;
			if (h <= 18)
				m.upAreaLow += area;
			upH.push_back({ h, area });
			m.upHmin = std::min(m.upHmin, h);
			m.upHmax = std::max(m.upHmax, h);
		}
	}
	if (!upH.empty())
	{
		std::sort(upH.begin(), upH.end());
		double acc = 0;
		for (const auto& [h, a] : upH)
		{
			acc += a;
			if (acc >= m.upArea / 2)
			{
				m.upHmed = h;
				break;
			}
		}
	}
	// base: the vertices within 1 u of the lowest
	double sum = 0, lo = 1e30, hi = -1e30;
	std::size_t n = 0;
	for (const WorldShape* s : use)
		for (std::size_t i = 0; i + 2 < s->vertices.size(); i += 3)
			if (s->vertices[i + 1] <= y0 + 1)
			{
				const double d = s->vertices[i + 1] - ground.at(s->vertices[i], s->vertices[i + 2]);
				sum += d;
				lo = std::min(lo, d);
				hi = std::max(hi, d);
				++n;
			}
	m.baseMean = n ? sum / n : 0;
	m.baseMin = lo;
	m.baseMax = hi;

	// the ground cut (ground.cpp Gen::cut): per shape, the vertical line through a sample hits the shape's non-vertical triangles; spans are
	// consecutive hit pairs, an odd count a shell open at the bottom; dropped when a span reaches into (g + 0.1, g + 18). Projected: any hit.
	const double ext = std::max(x1 - x0, z1 - z0);
	const double step = std::clamp(ext / 48, 0.5, 6.0);
	const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / step))), nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / step)));
	std::vector<char> dropped(static_cast<std::size_t>(nx) * nz, 0), covered(dropped.size(), 0);
	std::vector<double> cellLo(dropped.size(), 1e300), cellHi(dropped.size(), -1e300);
	for (const WorldShape* s : use)
	{
		const int i0 = std::max(0, static_cast<int>(std::floor((s->min[0] - x0) / step))), i1 = std::min(nx - 1, static_cast<int>(std::floor((s->max[0] - x0) / step)));
		const int j0 = std::max(0, static_cast<int>(std::floor((s->min[2] - z0) / step))), j1 = std::min(nz - 1, static_cast<int>(std::floor((s->max[2] - z0) / step)));
		if (i1 < i0 || j1 < j0)
			continue;
		const int w = i1 - i0 + 1;
		std::vector<std::vector<double>> hits(static_cast<std::size_t>(w) * (j1 - j0 + 1));
		for (std::size_t k = 0; k + 2 < s->indices.size(); k += 3)
		{
			std::array<std::array<double, 3>, 3> p;
			for (int c = 0; c < 3; ++c)
				for (int a = 0; a < 3; ++a)
					p[c][a] = s->vertices[s->indices[k + c] * 3 + a];
			const double det = (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (p[1][2] - p[0][2]);
			if (std::fabs(det) < 1e-9)
				continue;	// vertical
			const double tx0 = std::min({ p[0][0], p[1][0], p[2][0] }), tx1 = std::max({ p[0][0], p[1][0], p[2][0] });
			const double tz0 = std::min({ p[0][2], p[1][2], p[2][2] }), tz1 = std::max({ p[0][2], p[1][2], p[2][2] });
			const int a0 = std::max(i0, static_cast<int>(std::floor((tx0 - x0) / step))), a1 = std::min(i1, static_cast<int>(std::floor((tx1 - x0) / step)));
			const int b0 = std::max(j0, static_cast<int>(std::floor((tz0 - z0) / step))), b1 = std::min(j1, static_cast<int>(std::floor((tz1 - z0) / step)));
			for (int b = b0; b <= b1; ++b)
				for (int a = a0; a <= a1; ++a)
				{
					const double qx = x0 + (a + 0.5) * step, qz = z0 + (b + 0.5) * step;
					const double l1 = ((qx - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (qz - p[0][2])) / det;
					const double l2 = ((p[1][0] - p[0][0]) * (qz - p[0][2]) - (qx - p[0][0]) * (p[1][2] - p[0][2])) / det;
					if (l1 < -1e-9 || l2 < -1e-9 || l1 + l2 > 1 + 1e-9)
						continue;
					hits[static_cast<std::size_t>(b - j0) * w + (a - i0)].push_back(p[0][1] + l1 * (p[1][1] - p[0][1]) + l2 * (p[2][1] - p[0][1]));
				}
		}
		for (int b = j0; b <= j1; ++b)
			for (int a = i0; a <= i1; ++a)
			{
				std::vector<double>& h = hits[static_cast<std::size_t>(b - j0) * w + (a - i0)];
				if (h.empty())
					continue;
				std::sort(h.begin(), h.end());
				const std::size_t cell = static_cast<std::size_t>(b) * nx + a;
				covered[cell] = 1;
				cellLo[cell] = std::min(cellLo[cell], h.front());
				cellHi[cell] = std::max(cellHi[cell], h.back());
				const double g = ground.at(x0 + (a + 0.5) * step, z0 + (b + 0.5) * step), lo2 = g + 0.1, hi2 = g + 18;
				bool drop = false;
				if (s->use == kworld::NavUse::Projected)
				{
					drop = true;
				} else {
					for (const double v : h)
						drop = drop || (v > lo2 && v < hi2);
					if (h.size() % 2)
						h.insert(h.begin(), -1e300);
					for (std::size_t k = 0; k + 1 < h.size() && !drop; k += 2)
						drop = h[k] < hi2 && h[k + 1] > lo2;
				}
				if (drop)
					dropped[cell] = 1;
			}
	}
	std::vector<double> lows, tops;
	std::size_t under = 0, through = 0, over = 0;
	for (std::size_t i = 0; i < dropped.size(); ++i)
	{
		m.cutArea += dropped[i] * step * step;
		m.coverArea += covered[i] * step * step;
		if (!covered[i])
			continue;
		const double g = ground.at(x0 + (i % nx + 0.5) * step, z0 + (i / nx + 0.5) * step);
		lows.push_back(cellLo[i] - g);
		tops.push_back(cellHi[i] - g);
		under += cellHi[i] < g;
		over += cellLo[i] > g;
		through += cellLo[i] <= g && cellHi[i] >= g;
	}
	if (!lows.empty())
	{
		m.baseRel = median(lows);
		m.topRel = median(tops);
		m.pUnder = 100.0 * under / lows.size();
		m.pThrough = 100.0 * through / lows.size();
		m.pOver = 100.0 * over / lows.size();
		m.state = m.baseRel < -0.5 ? "sunk" : m.baseRel > 0.5 ? "floating" : "flush";
	}
	return m;
}

std::string num(double v)
{
	char b[48];
	std::snprintf(b, sizeof b, "%.1f", v);
	return b;
}

// Aggregate of Meas over one record's (or class's) placements.
struct Agg
{
	std::size_t count = 0, measured = 0, empty = 0, cutting = 0, sunk = 0, flush = 0, floating = 0, withUp = 0;
	Meas first;
	std::vector<double> upArea, upAreaLow, upAreaWalk, cutArea, coverArea, baseMean, originDelta, height, footprint, baseRel, topRel, pUnder, pThrough, pOver, upHmed;
	double upHmin = 1e30, upHmax = -1e30, baseMin = 1e30, baseMax = -1e30;
	std::vector<std::string> sites;

	void add(const Meas& m)
	{
		++measured;
		if (!m.any)
		{
			++empty;
			return;
		}
		if (measured - empty == 1)
			first = m;
		upArea.push_back(m.upArea);
		upAreaLow.push_back(m.upAreaLow);
		upAreaWalk.push_back(m.upAreaWalkUse);
		cutArea.push_back(m.cutArea);
		coverArea.push_back(m.coverArea);
		baseMean.push_back(m.baseMean);
		originDelta.push_back(m.originDelta);
		baseRel.push_back(m.baseRel);
		if (m.upTris)
			upHmed.push_back(m.upHmed);
		topRel.push_back(m.topRel);
		pUnder.push_back(m.pUnder);
		pThrough.push_back(m.pThrough);
		pOver.push_back(m.pOver);
		height.push_back(m.height);
		footprint.push_back(m.footprint);
		cutting += m.cutArea > 0;
		withUp += m.upTris > 0;
		sunk += m.state == "sunk";
		flush += m.state == "flush";
		floating += m.state == "floating";
		if (m.upTris)
		{
			upHmin = std::min(upHmin, m.upHmin);
			upHmax = std::max(upHmax, m.upHmax);
		}
		baseMin = std::min(baseMin, m.baseMin);
		baseMax = std::max(baseMax, m.baseMax);
	}

	std::string row() const
	{
		std::ostringstream o;
		const std::size_t n = measured - empty;
		o << measured << '\t' << empty << '\t' << first.shapes << '\t' << first.boxes << '/' << first.convex << '/' << first.meshes << '\t'
			<< first.useWalk << '/' << first.useObst << '/' << first.useProj << '\t' << first.tris << '\t'
			<< withUp << '\t' << num(median(upArea)) << '\t' << num(median(upAreaWalk)) << '\t' << num(median(upAreaLow)) << '\t'
			<< (withUp ? num(upHmin) : "-") << '\t' << (withUp ? num(median(upHmed)) : "-") << '\t' << (withUp ? num(upHmax) : "-") << '\t'
			<< num(median(footprint)) << '\t' << num(median(height)) << '\t'
			<< cutting << '\t' << num(median(cutArea)) << '\t' << num(median(coverArea)) << '\t'
			<< sunk << '/' << flush << '/' << floating << '\t' << num(median(baseRel)) << '\t' << num(median(topRel)) << '\t' << num(median(pUnder)) << '/'
			<< num(median(pThrough)) << '/' << num(median(pOver)) << '\t' << num(median(baseMean)) << '\t' << (n ? num(baseMin) : "-") << '\t'
			<< (n ? num(baseMax) : "-") << '\t' << num(median(originDelta));
		return o.str();
	}
};

const char* kAggHeader = "measured\tno_geometry\tshapes\tbox/convex/mesh\tuse_walk/obst/proj\ttris\tn_with_up\tup_area_med\tup_area_walkuse_med\tup_area_le18_med"
	"\tup_h_min\tup_h_med\tup_h_max\tfootprint_med\theight_med\tn_cut\tcut_area_med\tcover_area_med\tsunk/flush/floating\tbase_rel_med\ttop_rel_med\tpct_under/through/over_med\tlowvert_mean_med\tlowvert_min\tlowvert_max\torigin_delta_med";

std::string site(const std::string& zone, double x, double z)
{
	return zone + " (" + num(x) + ", " + num(z) + ")";
}

std::string joinSites(const std::vector<std::string>& s)
{
	std::string out;
	for (const std::string& v : s)
		out += (out.empty() ? "" : "; ") + v;
	return out;
}

// Case-insensitive record path -> file, as CollisionLibrary does it.
struct Files
{
	std::map<std::string, std::string> byName;
	std::map<std::string, std::unique_ptr<kworld::CollisionModel>> models;

	static std::string norm(std::string p)
	{
		for (char& c : p)
			c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (const char* pre : { "./", "data/" })
			if (p.compare(0, std::char_traits<char>::length(pre), pre) == 0)
				p.erase(0, std::char_traits<char>::length(pre));
		return p;
	}

	explicit Files(const std::string& root)
	{
		std::error_code ec;
		for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec))
				byName.emplace(norm(it->path().lexically_relative(root).generic_string()), it->path().string());
	}

	const kworld::CollisionModel* model(const std::string& recordPath)
	{
		if (recordPath.empty())
			return nullptr;
		const auto f = byName.find(norm(recordPath));
		if (f == byName.end())
			return nullptr;
		auto c = models.find(f->second);
		if (c == models.end())
		{
			std::ifstream in(f->second, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			std::unique_ptr<kworld::CollisionModel> parsed;
			try
			{
				parsed = std::make_unique<kworld::CollisionModel>(kworld::parseCollisionXml(text.str()));
			} catch (const std::exception&) {
			}
			c = models.emplace(f->second, std::move(parsed)).first;
		}
		return c->second.get();
	}
};

// The census class of a placed BUILDING record (rules in order; the first that fits).
std::string classOf(const kdata::GameData& gd, const kdata::Record& r, bool mask, const std::vector<int>& functions, bool turret)
{
	const std::string cat = r.stringOr("building category");
	const auto has = [&](int f) { return std::find(functions.begin(), functions.end(), f) != functions.end(); };
	if (r.boolOr("is wall gate", false))
		return "wall gate";
	if (cat == "WALLS" && r.stringOr("building group") == "Ramp")
		return "wall ramp";
	if (cat == "WALLS")
		return r.boolOr("links together", false) ? "wall / fence" : "wall other";
	if (r.boolOr("is foliage", false) && cat == "MINING")
		return "ore node (foliage building)";
	if (cat == "MINING")
		return "mine / quarry machine";
	if (turret)
		return "turret";
	if (has(5))
		return r.stringOr("building group") == "Wind Generator" || r.name.find("Wind") != std::string::npos ? "windmill" : "generator";
	if (cat == "CAMPING")
		return "camp (bed, fire, tent)";
	if (r.name == "Outhouse")
		return "outhouse";
	if (has(15) || cat == "LIGHTS")
		return "light / lamp";
	if (cat == "FARMING")
		return "farm / well / water";
	if (cat == "CANNIBAL" || cat == "INTERIOR" || cat == "TRAINING")
		return "cage / training / camp prop";
	if (mask)
		return "building with interior mask (out of scope)";
	const int pm = r.intOr("path mode", 2);
	if (cat == "BUILDINGS")
		return pm == 3 ? "building without mask (walkable)" : "building without mask (pm" + std::to_string(pm) + ")";
	(void)gd;
	return "town prop / misc (pm" + std::to_string(pm) + ")";
}

std::string buildingType(const kdata::GameData& gd, const kdata::Record* r)
{
	std::string out;
	if (r != nullptr)
		if (const auto* c = r->findRefs("building type"))
			for (const auto& e : c->entries)
				if (const kdata::Record* b = gd.find(e.targetID))
					out += (out.empty() ? "" : ",") + b->name;
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	std::vector<std::array<double, 4>> orePlacements;	// x, z, y, record index of the zone-placed ore nodes (class "ore node")
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: knav_object_census OUT_DIR [DATA_DIR]\n");
		return 2;
	}
	const std::string out = argv[1];
	const std::string dataDir = argc > 2 ? argv[2] : std::string(KENSHI_DATA);
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const Ground ground{ terrain };
	const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
	Files files(dataDir);

	// ---- zone placements (INSTANCE_COLLECTION 0-buildinglist) ----
	{
		const auto zones = kworld::Zones::load(dataDir);
		const auto placed = zones.placeMerged(terrain, gd);
		kworld::CollisionLibrary lib(dataDir, gd);
		kworld::CollisionCoverage cov;
		std::map<std::string, std::vector<const kworld::PlacedInstance*>> byRecord;
		for (const auto& p : placed)
			byRecord[p.buildingID].push_back(&p);
		std::map<std::string, Agg> classAgg;
		std::map<std::string, std::size_t> classCount, classRecords;
		std::ofstream outliers(out + "/outliers.tsv");
		outliers << "record_id\tname\tinstance\tzone\tx\ty\tz\torigin_delta\tlowvert_min\tup_h_max\tlayer\n";
		std::ofstream f(out + "/records.tsv");
		f << "class\tcount\tname\tid\tgroup\tcategory\tpath_mode\tmask\tflags\tfunctions\tturret\tcollision_parts\t" << kAggHeader << "\tsample_sites\n";
		for (const auto& [id, list] : byRecord)
		{
			const kdata::Record* r = gd.find(id);
			if (r == nullptr)
				continue;
			std::string flags, fn;
			std::vector<int> functions;
			for (const char* k : { "is feature", "is foliage", "is gateway", "is wall gate", "links together", "is sign", "is exterior furniture",
				"is interior furniture", "match slope", "interior terrain", "is shelter", "has inventory" })
				if (r->boolOr(k, false))
					flags += std::string(k) + ",";
			if (const auto* c = r->findRefs("functionality"))
				for (const auto& e : c->entries)
					if (const kdata::Record* fr = gd.find(e.targetID))
					{
						fn += fr->name + "(" + std::to_string(fr->intOr("function", -1)) + "),";
						functions.push_back(fr->intOr("function", -1));
					}
			const auto* masks = r->findRefs("interior mask");
			const bool mask = masks != nullptr && !masks->entries.empty();
			const bool turret = r->findRefs("gun turret") != nullptr;
			std::size_t collisionParts = 0, parts = 0;
			if (const auto* c = r->findRefs("parts"))
				for (const auto& e : c->entries)
					if (const kdata::Record* pr = gd.find(e.targetID))
					{
						++parts;
						collisionParts += files.model(pr->fileOr("xml collision")) != nullptr;
					}
			const std::string cls = classOf(gd, *r, mask, functions, turret);
			Agg agg;
			agg.count = list.size();
			if (cls.starts_with("ore node"))
				for (const auto* p : list)
					orePlacements.push_back({ p->x, p->z, p->y, r->stringOr("building group") == "Copper Resource" ? 1.0 : 0.0 });
			const bool skip = cls.starts_with("building with interior mask");
			if (!skip)
				for (const auto* p : list)
				{
					std::vector<WorldShape> shapes;
					const auto nav = lib.addBuilding(*p, shapes, cov);
					(void)nav;
					const Meas m = measure(shapes, ground, p->x, p->y, p->z);
					agg.add(m);
					if (m.any && (m.baseMin < -2000 || m.upHmax > 5000))
						outliers << p->buildingID << '\t' << r->name << '\t' << p->instanceID << '\t' << p->positionCell.x << '.' << p->positionCell.z << '\t' << num(p->x) << '\t'
							<< num(p->y) << '\t' << num(p->z) << '\t' << num(m.originDelta) << '\t' << num(m.baseMin) << '\t' << num(m.upHmax) << '\t' << p->layer << '\n';
				}
			for (const std::size_t k : { std::size_t(0), list.size() / 2, list.size() - 1 })
				if (agg.sites.size() < 3 && (agg.sites.empty() || agg.sites.size() < list.size()))
				{
					const auto& p = *list[k];
					const std::string s = site("zone " + std::to_string(p.positionCell.x) + "." + std::to_string(p.positionCell.z), p.x, p.z);
					if (std::find(agg.sites.begin(), agg.sites.end(), s) == agg.sites.end())
						agg.sites.push_back(s);
				}
			f << cls << '\t' << list.size() << '\t' << r->name << '\t' << id << '\t' << r->stringOr("building group") << '\t' << r->stringOr("building category")
				<< '\t' << r->intOr("path mode", 2) << '\t' << mask << '\t' << flags << '\t' << fn << '\t' << turret << '\t' << collisionParts << "/" << parts
				<< '\t' << agg.row() << '\t' << joinSites(agg.sites) << '\n';
			classCount[cls] += list.size();
			++classRecords[cls];
			Agg& ca = classAgg[cls];
			ca.count += list.size();
			// class rollups: merge the vectors
			ca.measured += agg.measured;
			ca.empty += agg.empty;
			ca.cutting += agg.cutting;
			ca.sunk += agg.sunk;
			ca.flush += agg.flush;
			ca.floating += agg.floating;
			ca.withUp += agg.withUp;
			for (const auto* v : { &agg.cutArea })
				ca.cutArea.insert(ca.cutArea.end(), v->begin(), v->end());
			ca.baseMean.insert(ca.baseMean.end(), agg.baseRel.begin(), agg.baseRel.end());
			ca.upAreaLow.insert(ca.upAreaLow.end(), agg.upAreaLow.begin(), agg.upAreaLow.end());
			ca.baseMin = std::min(ca.baseMin, agg.baseMin);
			ca.baseMax = std::max(ca.baseMax, agg.baseMax);
			ca.upHmin = std::min(ca.upHmin, agg.upHmin);
			ca.upHmax = std::max(ca.upHmax, agg.upHmax);
		}
		std::ofstream c(out + "/classes.tsv");
		c << "class\trecords\tplacements\tmeasured\tno_geometry\tn_with_up_faces\tn_cut\tsunk\tflush\tfloating\tbase_rel_med\tlowvert_min\tlowvert_max\tcut_area_med\tup_h_min\tup_h_max\n";
		for (const auto& [cls, a] : classAgg)
			c << cls << '\t' << classRecords[cls] << '\t' << a.count << '\t' << a.measured << '\t' << a.empty << '\t' << a.withUp << '\t' << a.cutting << '\t' << a.sunk
				<< '\t' << a.flush << '\t' << a.floating << '\t' << num(median(a.baseMean)) << '\t' << (a.baseMin < 1e29 ? num(a.baseMin) : "-") << '\t'
				<< (a.baseMax > -1e29 ? num(a.baseMax) : "-") << '\t' << num(median(a.cutArea)) << '\t' << (a.upHmin < 1e29 ? num(a.upHmin) : "-") << '\t'
				<< (a.upHmax > -1e29 ? num(a.upHmax) : "-") << '\n';
		std::printf("placements %zu records %zu\n", placed.size(), byRecord.size());
	}

	// ---- features.dat map features ----
	const kworld::Features features = kworld::Features::join(gd, dataDir);
	{
		std::map<std::string, Agg> agg;
		std::map<std::string, std::vector<const kworld::FeaturePlacement*>> byRecord;
		for (const auto& p : features.placements())
			byRecord[p.recordID].push_back(&p);
		std::ofstream f(out + "/features.tsv");
		f << "record_id\tname\tcount\tcollision_file\tnonuniform_scale\tunresolved\t" << kAggHeader << "\tsample_sites\n";
		for (const auto& [id, list] : byRecord)
		{
			const kdata::Record* r = gd.find(id);
			const std::string coll = r ? r->fileOr("collision") : "";
			const kworld::CollisionModel* model = files.model(coll);
			Agg a;
			std::size_t nonuniform = 0;
			const std::size_t cap = 400;
			for (std::size_t i = 0; i < list.size() && model; i += std::max<std::size_t>(1, list.size() / cap))
			{
				const kworld::FeaturePlacement& p = *list[i];
				kworld::CollisionModel scaled = *model;
				const bool uni = p.scaleX == p.scaleY && p.scaleY == p.scaleZ;
				nonuniform += !uni;
				if (!uni)
					for (kworld::CollisionShape& s : scaled.shapes)
					{
						for (std::size_t v = 0; v + 2 < s.vertices.size(); v += 3)
						{
							s.vertices[v] *= p.scaleX;
							s.vertices[v + 1] *= p.scaleY;
							s.vertices[v + 2] *= p.scaleZ;
						}
					}
				std::vector<WorldShape> shapes;
				kworld::placeModel(scaled, { p.x, p.y, p.z }, { p.qw, p.qx, p.qy, p.qz }, uni ? p.scaleX : 1.0, kworld::NavUse::Walkable, false, shapes);
				a.add(measure(shapes, ground, p.x, p.y, p.z));
			}
			for (const std::size_t k : { std::size_t(0), list.size() / 2, list.size() - 1 })
			{
				const auto& p = *list[k];
				const std::string s = site("zone " + std::to_string(p.positionCell.x) + "." + std::to_string(p.positionCell.z), p.x, p.z);
				if (std::find(a.sites.begin(), a.sites.end(), s) == a.sites.end())
					a.sites.push_back(s);
			}
			f << id << '\t' << (r ? r->name : "(missing)") << '\t' << list.size() << '\t' << coll << '\t' << nonuniform << '\t' << (model == nullptr) << '\t' << a.row() << '\t' << joinSites(a.sites) << '\n';
		}
		std::printf("features %zu placements, %zu records\n", features.placements().size(), byRecord.size());
	}

	// ---- generated foliage (rocks, trees): FOLIAGE_MESH instances with collision or a navmesh cutter ----
	{
		const auto biomes = kworld::BiomeMap::load(dataDir + "/newland/land/biomemap.png");
		const auto towns = kworld::Towns::join(gd, dataDir);
		kworld::CollisionLibrary lib(dataDir, gd);
		struct Fol
		{
			std::size_t count = 0, kept = 0;
			std::vector<kworld::FoliageInstance> samples;
			std::vector<std::string> zoneOf;
		};
		std::map<std::string, Fol> fol;
		std::map<std::pair<int, int>, std::vector<std::array<double, 3>>> oreCells;	// 200 u cells: x, z, y of every foliage instance with a `building type`
		std::map<std::string, bool> hasType;
		std::size_t oreFoliage = 0;
		for (int z = 0; z < kworld::kZoneCellsPerAxis; ++z)
			for (int x = 0; x < kworld::kZoneCellsPerAxis; ++x)
				for (const auto& inst : kworld::generateFoliage({ x, z }, terrain, biomes, gd, features, towns))
				{
					auto ht = hasType.find(inst.recordID);
					if (ht == hasType.end())
						ht = hasType.emplace(inst.recordID, !buildingType(gd, gd.find(inst.recordID)).empty()).first;
					if (ht->second)
					{
						oreCells[{ static_cast<int>(std::floor(inst.x / 200)), static_cast<int>(std::floor(inst.z / 200)) }].push_back({ inst.x, inst.z, inst.y });
						++oreFoliage;
					}
					Fol& e = fol[inst.recordID];
					++e.count;
					if (e.samples.size() < 60 && (e.samples.size() < 10 || e.count % 211 == 0))
					{
						e.samples.push_back(inst);
						e.zoneOf.push_back("zone " + std::to_string(x) + "." + std::to_string(z));
					}
				}
		std::ofstream f(out + "/foliage.tsv");
		f << "record_id\tname\tcount\tcollision_file\twalkable\tnavmesh_cutter\tbuilding_type\t" << kAggHeader << "\tsample_sites\n";
		for (const auto& [id, e] : fol)
		{
			const kdata::Record* r = gd.find(id);
			const std::string coll = r ? r->fileOr("collision") : "";
			Agg a;
			for (std::size_t i = 0; i < e.samples.size(); ++i)
			{
				std::vector<WorldShape> shapes;
				kworld::CollisionCoverage cov;
				lib.addFoliage(e.samples[i], shapes, cov);
				a.add(measure(shapes, ground, e.samples[i].x, e.samples[i].y, e.samples[i].z));
			}
			for (const std::size_t k : { std::size_t(0), e.samples.size() / 2, e.samples.size() - 1 })
				if (!e.samples.empty() && a.sites.size() < 3)
				{
					const std::string s = site(e.zoneOf[k], e.samples[k].x, e.samples[k].z);
					if (std::find(a.sites.begin(), a.sites.end(), s) == a.sites.end())
						a.sites.push_back(s);
				}
			f << id << '\t' << (r ? r->name : "(missing)") << '\t' << e.count << '\t' << coll << '\t' << (r && r->boolOr("walkable", false)) << '\t'
				<< (r ? num(r->floatOr("navmesh cutter", 0.0f)) : "0") << '\t' << buildingType(gd, r) << '\t' << a.row() << '\t' << joinSites(a.sites) << '\n';
		}
		std::printf("foliage records %zu\n", fol.size());
		// the zone-placed Iron / Copper Resource records against the foliage rocks that carry a `building type`: nearest rock in xz
		std::vector<double> dist, dy;
		for (const auto& o : orePlacements)
		{
			double best = 1e30, by = 0;
			const int cx = static_cast<int>(std::floor(o[0] / 200)), cz = static_cast<int>(std::floor(o[1] / 200));
			for (int dz = -2; dz <= 2; ++dz)
				for (int dx = -2; dx <= 2; ++dx)
				{
					const auto it = oreCells.find({ cx + dx, cz + dz });
					if (it == oreCells.end())
						continue;
					for (const auto& q : it->second)
					{
						const double d = std::hypot(q[0] - o[0], q[1] - o[1]);
						if (d < best)
						{
							best = d;
							by = o[2] - q[2];
						}
					}
				}
			dist.push_back(best);
			dy.push_back(by);
		}
		std::ofstream ore(out + "/ore_pairing.txt");
		std::sort(dist.begin(), dist.end());
		const auto pct = [&](double q) { return dist.empty() ? 0.0 : dist[std::min(dist.size() - 1, static_cast<std::size_t>(q * dist.size()))]; };
		std::size_t within = 0;
		for (const double d : dist)
			within += d <= 40;
		ore << "zone-placed ore nodes (Iron / Copper Resource records): " << orePlacements.size() << "; foliage instances with a building type: " << oreFoliage
			<< "\nnearest such foliage rock in xz (u): p10 " << num(pct(0.1)) << " p50 " << num(pct(0.5)) << " p90 " << num(pct(0.9)) << " p99 " << num(pct(0.99))
			<< " max " << num(pct(1)) << "; within 40 u: " << within << "\n";
	}
	return 0;
}
