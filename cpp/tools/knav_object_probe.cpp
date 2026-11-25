// knav_object_probe: the object census' sample sites against the shipped navtiles (kenshi-k16.7.196, NAVMESH_GENERATOR.md section 12 O1-O4).
//   knav_object_probe CENSUS_RECORDS_TSV [--tol T] [--only KIND] [DATA_DIR]      one TSV row per site on stdout
// Each site's object is placed as the census placed it, a vertical-ray grid over its xz bound gives per cell the object's hits and the
// ground cut verdict (knav::ground's span rule); the shipped world meshes (interiors left out) are sampled at the cell: a face within
// `tol` of the object's top (O1/O2), of the terrain under a cut cell (O4, by the object's height there), and how many faces lie within 0.5 u of the
// terrain where the top is within 0.25 u of it (O3). A control ring outside the bound gives the shipped mesh's own offset from the terrain grid.
#include "kdata/gamedata.hpp"
#include "knav/native.hpp"
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
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kworld::WorldShape;

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
		if (std::fabs(a - d) <= std::fabs(b - c))
			return u >= v ? a + u * (b - a) + v * (d - b) : a + u * (d - c) + v * (c - a);
		return u + v <= 1 ? a + u * (b - a) + v * (c - a) : d + (1 - u) * (c - d) + (1 - v) * (b - d);
	}
};

int zoneOf(double v)
{
	return static_cast<int>(std::floor((v - kworld::kWorldMin) / kworld::kZoneSize));
}

// The shipped world meshes of the zones round (zx, zz): triangles with the polygon they came from, a hash over 64 u cells.
struct Native
{
	struct Tri
	{
		std::array<double, 9> p;
		int face;
	};
	std::vector<Tri> tris;
	std::map<std::pair<int, int>, std::vector<int>> hash;
	static constexpr double kCell = 64;

	Native(const std::string& dir, int zx, int zz, int radius)
	{
		int face = 0;
		for (int j = zz - radius; j <= zz + radius; ++j)
			for (int i = zx - radius; i <= zx + radius; ++i)
			{
				std::vector<knav::native::Mesh> meshes;
				try
				{
					meshes = knav::native::loadTile(dir, i, j);
				} catch (const std::exception&) {
					continue;
				}
				for (const auto& m : meshes)
				{
					if (m.interior())
						continue;
					for (const auto& f : m.faces)
					{
						++face;
						for (int k = 1; k + 1 < f.numEdges; ++k)
						{
							const auto& a = m.vertices[m.edges[f.startEdge].a];
							const auto& b = m.vertices[m.edges[f.startEdge + k].a];
							const auto& c = m.vertices[m.edges[f.startEdge + k + 1].a];
							tris.push_back({ { a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2] }, face });
						}
					}
				}
			}
		for (std::size_t t = 0; t < tris.size(); ++t)
		{
			const auto& p = tris[t].p;
			const int i0 = static_cast<int>(std::floor(std::min({ p[0], p[3], p[6] }) / kCell)), i1 = static_cast<int>(std::floor(std::max({ p[0], p[3], p[6] }) / kCell));
			const int j0 = static_cast<int>(std::floor(std::min({ p[2], p[5], p[8] }) / kCell)), j1 = static_cast<int>(std::floor(std::max({ p[2], p[5], p[8] }) / kCell));
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
					hash[{ i, j }].push_back(static_cast<int>(t));
		}
	}

	// heights of the faces over (x, z) (faces: their ids), all layers
	void at(double x, double z, std::vector<std::pair<double, int>>& out) const
	{
		out.clear();
		const auto it = hash.find({ static_cast<int>(std::floor(x / kCell)), static_cast<int>(std::floor(z / kCell)) });
		if (it == hash.end())
			return;
		for (const int t : it->second)
		{
			const auto& p = tris[static_cast<std::size_t>(t)].p;
			const double det = (p[5] - p[8]) * (p[0] - p[6]) + (p[6] - p[3]) * (p[2] - p[8]);
			if (std::fabs(det) < 1e-9)
				continue;
			const double l1 = ((p[5] - p[8]) * (x - p[6]) + (p[6] - p[3]) * (z - p[8])) / det;
			const double l2 = ((p[8] - p[2]) * (x - p[6]) + (p[0] - p[6]) * (z - p[8])) / det;
			const double l3 = 1 - l1 - l2;
			if (l1 >= -1e-9 && l2 >= -1e-9 && l3 >= -1e-9)
				out.push_back({ l1 * p[1] + l2 * p[4] + l3 * p[7], tris[static_cast<std::size_t>(t)].face });
		}
	}
};

struct Hit
{
	double y;
	bool up;	// normal within 60 deg of +y (a mesh: either vertical)
	kworld::NavUse use;
};

// O4 buckets by the object's height over the terrain in the cut cell
constexpr double kBucket[] = { 0.1, 0.25, 0.5, 1, 2, 4, 8, 18, 1e30 };
constexpr int kBuckets = 9;
// O1/O2: tops by height over the ground
constexpr double kTopB[] = { 2, 6, 18, 60, 1e30 };
constexpr int kTopBuckets = 5;
// the clearance under the object: its lowest hit over the ground
constexpr double kClear[] = { 0.1, 4, 8, 12, 16, 18, 20, 24, 32, 64, 1e30 };
constexpr int kClears = 11;

struct Result
{
	std::size_t cells = 0, cut = 0;
	std::size_t topN[2] = { 0, 0 }, topHit[2] = { 0, 0 };	// [obstacle, walkable]: cells whose highest hit is an up face > 0.25 over the ground; native face within tol of it
	std::size_t topAnyAbove[2] = { 0, 0 };	// ... native has any face above the ground + tol in the cell
	std::size_t topLow[2] = { 0, 0 }, topLowHit[2] = { 0, 0 };	// the same restricted to tops <= 18 u over the ground
	std::size_t buckN[kBuckets] = {}, buckHole[kBuckets] = {};	// cut cells
	std::size_t keepN[kBuckets] = {}, keepHole[kBuckets] = {};	// covered cells the rule keeps (the object under the ground, or its span clear of it)
	std::size_t tbN[2][kTopBuckets] = {}, tbHit[2][kTopBuckets] = {};
	std::size_t clN[kClears] = {}, clHole[kClears] = {};	// covered cells with the lowest hit above 0.1 over the ground, by that clearance	// cut cells by object height; no native face within tol of the terrain
	std::size_t coN = 0, coFaces[3] = {};	// O3: cells whose highest up hit is within 0.25 u of the terrain; native faces within 0.5 of the terrain: 0 / 1 / 2+ (distinct polygons)
	std::size_t coGround = 0;
	std::size_t ctrlN = 0, ctrlHit = 0;
	double ctrlSum = 0;
	double hMax = 0, hMin = 1e30;
	double topMed = 0;
};

double nearest(const std::vector<std::pair<double, int>>& v, double h)
{
	double best = 1e30;
	for (const auto& e : v)
		best = std::min(best, std::fabs(e.first - h));
	return best;
}

Result probe(const std::vector<WorldShape>& shapes, const Ground& ground, const Native& nat, double tol)
{
	Result r;
	double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
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
	}
	if (use.empty())
		return r;
	const double ext = std::max(x1 - x0, z1 - z0);
	const double step = std::clamp(ext / 40, 1.0, 12.0);
	const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / step))), nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / step)));
	std::vector<std::vector<Hit>> cellHits(static_cast<std::size_t>(nx) * nz);
	std::vector<char> dropped(cellHits.size(), 0);
	for (const WorldShape* s : use)
	{
		const bool oneSided = s->half[0] > 0 || s->half[1] > 0 || s->half[2] > 0 || s->closed || s->solid;
		const int i0 = std::max(0, static_cast<int>(std::floor((s->min[0] - x0) / step))), i1 = std::min(nx - 1, static_cast<int>(std::floor((s->max[0] - x0) / step)));
		const int j0 = std::max(0, static_cast<int>(std::floor((s->min[2] - z0) / step))), j1 = std::min(nz - 1, static_cast<int>(std::floor((s->max[2] - z0) / step)));
		if (i1 < i0 || j1 < j0)
			continue;
		const int w = i1 - i0 + 1;
		std::vector<std::vector<Hit>> hits(static_cast<std::size_t>(w) * (j1 - j0 + 1));
		for (std::size_t k = 0; k + 2 < s->indices.size(); k += 3)
		{
			std::array<std::array<double, 3>, 3> p;
			for (int c = 0; c < 3; ++c)
				for (int a = 0; a < 3; ++a)
					p[c][a] = s->vertices[s->indices[k + c] * 3 + a];
			const double det = (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (p[1][2] - p[0][2]);
			if (std::fabs(det) < 1e-9)
				continue;
			const double ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
			const double vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
			const double nxn = uy * vz - uz * vy, nyn = uz * vx - ux * vz, nzn = ux * vy - uy * vx;
			const double len = std::sqrt(nxn * nxn + nyn * nyn + nzn * nzn);
			const bool up = len > 1e-12 && (oneSided ? nyn : std::fabs(nyn)) / len >= 0.5;
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
					hits[static_cast<std::size_t>(b - j0) * w + (a - i0)].push_back({ p[0][1] + l1 * (p[1][1] - p[0][1]) + l2 * (p[2][1] - p[0][1]), up, s->use });
				}
		}
		for (int b = j0; b <= j1; ++b)
			for (int a = i0; a <= i1; ++a)
			{
				std::vector<Hit>& h = hits[static_cast<std::size_t>(b - j0) * w + (a - i0)];
				if (h.empty())
					continue;
				std::sort(h.begin(), h.end(), [](const Hit& l, const Hit& r2) { return l.y < r2.y; });
				const std::size_t cell = static_cast<std::size_t>(b) * nx + a;
				cellHits[cell].insert(cellHits[cell].end(), h.begin(), h.end());
				const double g = ground.at(x0 + (a + 0.5) * step, z0 + (b + 0.5) * step), lo2 = g + 0.1, hi2 = g + 18;
				bool drop = false;
				if (s->use == kworld::NavUse::Projected)
				{
					drop = true;
				} else {
					for (const Hit& v : h)
						drop = drop || (v.y > lo2 && v.y < hi2);
					std::vector<double> ys;
					for (const Hit& v : h)
						ys.push_back(v.y);
					if (ys.size() % 2)
						ys.insert(ys.begin(), -1e300);
					for (std::size_t k = 0; k + 1 < ys.size() && !drop; k += 2)
						drop = ys[k] < hi2 && ys[k + 1] > lo2;
				}
				if (drop)
					dropped[cell] = 1;
			}
	}
	std::vector<std::pair<double, int>> nf;
	std::vector<double> tops;
	for (std::size_t i = 0; i < cellHits.size(); ++i)
	{
		auto& h = cellHits[i];
		if (h.empty())
			continue;
		++r.cells;
		const double qx = x0 + (i % nx + 0.5) * step, qz = z0 + (i / nx + 0.5) * step;
		const double g = ground.at(qx, qz);
		std::sort(h.begin(), h.end(), [](const Hit& l, const Hit& r2) { return l.y < r2.y; });
		nat.at(qx + 0.0137, qz + 0.0071, nf);
		const double hTop = h.back().y - g;
		tops.push_back(hTop);
		r.hMax = std::max(r.hMax, hTop);
		r.hMin = std::min(r.hMin, hTop);
		{
			int b = 0;
			while (hTop > kBucket[b])
				++b;
			const bool hole = nearest(nf, g) > tol;
			const double low = h.front().y - g;
			if (low > 0.1 && !h.front().up)
			{
				int c = 0;
				while (low > kClear[c])
					++c;
				++r.clN[c];
				r.clHole[c] += hole;
			}
			if (dropped[i])
			{
				++r.cut;
				++r.buckN[b];
				r.buckHole[b] += hole;
			} else {
				++r.keepN[b];
				r.keepHole[b] += hole;
			}
		}
		if (h.back().up && hTop > 0.25)
		{
			const int u = h.back().use == kworld::NavUse::Walkable ? 1 : 0;
			++r.topN[u];
			const bool hit = nearest(nf, h.back().y) <= tol;
			r.topHit[u] += hit;
			int tb = 0;
			while (hTop > kTopB[tb])
				++tb;
			++r.tbN[u][tb];
			r.tbHit[u][tb] += hit;
			bool above = false;
			for (const auto& e : nf)
				above = above || e.first > g + tol;
			r.topAnyAbove[u] += above;
			if (hTop <= 18)
			{
				++r.topLow[u];
				r.topLowHit[u] += hit;
			}
		}
		// O3: the highest up hit within 0.25 of the terrain
		for (auto it = h.rbegin(); it != h.rend(); ++it)
			if (it->up)
			{
				if (std::fabs(it->y - g) <= 0.25)
				{
					++r.coN;
					std::vector<int> faces;
					for (const auto& e : nf)
						if (std::fabs(e.first - g) <= 0.5 && std::find(faces.begin(), faces.end(), e.second) == faces.end())
							faces.push_back(e.second);
					++r.coFaces[std::min<std::size_t>(faces.size(), 2)];
				}
				break;
			}
	}
	if (!tops.empty())
	{
		std::sort(tops.begin(), tops.end());
		r.topMed = tops[tops.size() / 2];
	}
	// control: the uncovered cells touching the covered ones (8-neighbours), the shipped mesh's ground beside the object
	for (int cj = -1; cj <= nz; ++cj)
		for (int ci = -1; ci <= nx; ++ci)
		{
			const auto covered = [&](int a, int b) { return a >= 0 && b >= 0 && a < nx && b < nz && !cellHits[static_cast<std::size_t>(b) * nx + a].empty(); };
			if (covered(ci, cj))
				continue;
			bool touch = false;
			for (int dj = -1; dj <= 1; ++dj)
				for (int di = -1; di <= 1; ++di)
					touch = touch || covered(ci + di, cj + dj);
			if (!touch)
				continue;
			const double qx = x0 + (ci + 0.5) * step, qz = z0 + (cj + 0.5) * step;
			nat.at(qx + 0.0137, qz + 0.0071, nf);
			++r.ctrlN;
			const double d = nearest(nf, ground.at(qx, qz));
			r.ctrlHit += d <= tol;
			r.ctrlSum += std::min(d, 100.0);
		}
	return r;
}

struct Job
{
	std::string kind, cls, name, site;
	double x, z;
	std::function<std::vector<WorldShape>()> shapes;
};

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

std::string num(double v)
{
	char b[48];
	std::snprintf(b, sizeof b, "%.1f", v);
	return b;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: knav_object_probe CENSUS_RECORDS_TSV [--tol T] [--only zone|feature|foliage] [DATA_DIR]\n");
		return 2;
	}
	double tol = 3.0;
	std::string only, dataDir = KENSHI_DATA;
	for (int i = 2; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--tol" && i + 1 < argc)
			tol = std::atof(argv[++i]);
		else if (a == "--only" && i + 1 < argc)
			only = argv[++i];
		else
			dataDir = a;
	}
	std::map<std::string, std::string> classOfRecord;	// census records.tsv: class (col 0) by record id (col 3)
	{
		std::ifstream in(argv[1]);
		std::string line;
		std::getline(in, line);
		while (std::getline(in, line))
		{
			std::vector<std::string> col;
			std::stringstream ss(line);
			for (std::string c; std::getline(ss, c, '\t');)
				col.push_back(c);
			if (col.size() > 3)
				classOfRecord[col[3]] = col[0];
		}
	}
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const Ground ground{ terrain };
	const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
	Files files(dataDir);
	kworld::CollisionLibrary lib(dataDir, gd);
	kworld::CollisionCoverage cov;
	std::vector<Job> jobs;

	if (only.empty() || only == "zone")
	{
		const auto zones = kworld::Zones::load(dataDir);
		static const auto placed = zones.placeMerged(terrain, gd);
		std::map<std::string, std::vector<const kworld::PlacedInstance*>> byRecord;
		for (const auto& p : placed)
			byRecord[p.buildingID].push_back(&p);
		for (const auto& [id, list] : byRecord)
		{
			const auto cls = classOfRecord.find(id);
			const kdata::Record* r = gd.find(id);
			if (cls == classOfRecord.end() || r == nullptr || cls->second.starts_with("building with interior mask"))
				continue;
			std::vector<std::size_t> idx;
			for (const std::size_t k : { std::size_t(0), list.size() / 2, list.size() - 1 })
				if (std::find(idx.begin(), idx.end(), k) == idx.end())
					idx.push_back(k);
			for (const std::size_t k : idx)
			{
				const kworld::PlacedInstance* p = list[k];
				jobs.push_back({ "zone", cls->second, r->name, "zone " + std::to_string(p->positionCell.x) + "." + std::to_string(p->positionCell.z), p->x, p->z,
					[&lib, &cov, p]
					{
						std::vector<WorldShape> shapes;
						lib.addBuilding(*p, shapes, cov);
						return shapes;
					} });
			}
		}
	}
	const kworld::Features features = kworld::Features::join(gd, dataDir);
	if (only.empty() || only == "feature")
	{
		std::map<std::string, std::vector<const kworld::FeaturePlacement*>> byRecord;
		for (const auto& p : features.placements())
			byRecord[p.recordID].push_back(&p);
		for (const auto& [id, list] : byRecord)
		{
			const kdata::Record* r = gd.find(id);
			const kworld::CollisionModel* model = files.model(r ? r->fileOr("collision") : "");
			if (model == nullptr)
				continue;
			std::vector<std::size_t> idx;
			for (const std::size_t k : { std::size_t(0), list.size() / 2, list.size() - 1 })
				if (std::find(idx.begin(), idx.end(), k) == idx.end())
					idx.push_back(k);
			for (const std::size_t k : idx)
			{
				const kworld::FeaturePlacement* p = list[k];
				jobs.push_back({ "feature", "map feature", r->name, "zone " + std::to_string(p->positionCell.x) + "." + std::to_string(p->positionCell.z), p->x, p->z,
					[model, p]
					{
						kworld::CollisionModel scaled = *model;
						const bool uni = p->scaleX == p->scaleY && p->scaleY == p->scaleZ;
						if (!uni)
							for (kworld::CollisionShape& s : scaled.shapes)
								for (std::size_t v = 0; v + 2 < s.vertices.size(); v += 3)
								{
									s.vertices[v] *= p->scaleX;
									s.vertices[v + 1] *= p->scaleY;
									s.vertices[v + 2] *= p->scaleZ;
								}
						std::vector<WorldShape> shapes;
						kworld::placeModel(scaled, { p->x, p->y, p->z }, { p->qw, p->qx, p->qy, p->qz }, uni ? p->scaleX : 1.0, kworld::NavUse::Walkable, false, shapes);
						return shapes;
					} });
			}
		}
	}
	static std::map<std::string, std::vector<std::pair<kworld::FoliageInstance, std::string>>> fol;
	if (only.empty() || only == "foliage")
	{
		const auto biomes = kworld::BiomeMap::load(dataDir + "/newland/land/biomemap.png");
		const auto towns = kworld::Towns::join(gd, dataDir);
		std::map<std::string, std::size_t> seen;
		for (int z = 0; z < kworld::kZoneCellsPerAxis; ++z)
			for (int x = 0; x < kworld::kZoneCellsPerAxis; ++x)
				for (const auto& inst : kworld::generateFoliage({ x, z }, terrain, biomes, gd, features, towns))
				{
					const kdata::Record* r = gd.find(inst.recordID);
					if (r == nullptr || (r->fileOr("collision").empty() && r->floatOr("navmesh cutter", 0.0f) <= 0))
						continue;
					const std::size_t n = ++seen[inst.recordID];
					auto& v = fol[inst.recordID];
					if (v.size() < 10 && (v.size() < 2 || n % 97 == 0))
						v.push_back({ inst, "zone " + std::to_string(x) + "." + std::to_string(z) });
				}
		for (const auto& [id, v] : fol)
		{
			const kdata::Record* r = gd.find(id);
			for (const std::size_t k : { std::size_t(0), v.size() / 2, v.size() - 1 })
			{
				if (k > 0 && k == v.size() / 2 && v.size() / 2 == v.size() - 1)
					continue;
				const auto* e = &v[k];
				jobs.push_back({ "foliage", r->boolOr("walkable", false) ? "foliage (walkable)" : r->fileOr("collision").empty() ? "foliage (cutter disc)" : "foliage (collision)", r->name,
					e->second, e->first.x, e->first.z,
					[&lib, e]
					{
						std::vector<WorldShape> shapes;
						kworld::CollisionCoverage c;
						lib.addFoliage(e->first, shapes, c);
						return shapes;
					} });
			}
		}
	}
	std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b)
		{ return std::make_pair(zoneOf(a.z), zoneOf(a.x)) < std::make_pair(zoneOf(b.z), zoneOf(b.x)); });
	std::fprintf(stderr, "%zu sites\n", jobs.size());
	std::printf("kind\tclass\trecord\tsite\tx\tz\tcells\tcut\ttop_n_obst\ttop_hit_obst\ttop_n_walk\ttop_hit_walk\ttop_anyabove_obst\ttop_anyabove_walk\t"
		"toplow_n_obst\ttoplow_hit_obst\ttoplow_n_walk\ttoplow_hit_walk\tco_n\tco_0\tco_1\tco_2\th_min\th_med\th_max\tctrl_hit\tctrl_n\tctrl_mean_delta");
	for (int b = 0; b < kBuckets; ++b)
		std::printf("\tb%d_n\tb%d_hole\tk%d_n\tk%d_hole", b, b, b, b);
	for (int u = 0; u < 2; ++u)
		for (int b = 0; b < kTopBuckets; ++b)
			std::printf("\tt%d%d_n\tt%d%d_hit", u, b, u, b);
	for (int b = 0; b < kClears; ++b)
		std::printf("\tc%d_n\tc%d_hole", b, b);
	std::printf("\n");
	std::unique_ptr<Native> nat;
	std::pair<int, int> have{ -999, -999 };
	const std::string tiles = knav::native::navtilesDir(dataDir);
	for (const Job& j : jobs)
	{
		const std::pair<int, int> zone{ zoneOf(j.x), zoneOf(j.z) };
		std::vector<WorldShape> shapes = j.shapes();
		if (shapes.empty())
			continue;
		double span = 0;
		for (const WorldShape& s : shapes)
			span = std::max<double>(span, std::max(s.max[0] - s.min[0], s.max[2] - s.min[2]));
		if (!nat || zone != have)
		{
			try
			{
				nat = std::make_unique<Native>(tiles, zone.first, zone.second, 1);
			} catch (const std::exception& e) {
				std::fprintf(stderr, "native: %s\n", e.what());
				continue;
			}
			have = zone;
		}
		const Result r = probe(shapes, ground, *nat, tol);
		if (r.cells == 0)
			continue;
		std::ostringstream o;
		o << j.kind << '\t' << j.cls << '\t' << j.name << '\t' << j.site << " (" << num(j.x) << ", " << num(j.z) << ")\t" << num(j.x) << '\t' << num(j.z) << '\t' << r.cells << '\t' << r.cut;
		for (const std::size_t v : { r.topN[0], r.topHit[0], r.topN[1], r.topHit[1], r.topAnyAbove[0], r.topAnyAbove[1], r.topLow[0], r.topLowHit[0], r.topLow[1], r.topLowHit[1],
			r.coN, r.coFaces[0], r.coFaces[1], r.coFaces[2] })
			o << '\t' << v;
		o << '\t' << num(r.hMin) << '\t' << num(r.topMed) << '\t' << num(r.hMax) << '\t' << r.ctrlHit << '\t' << r.ctrlN << '\t' << num(r.ctrlSum / r.ctrlN);
		std::fputs(o.str().c_str(), stdout);
		for (int b = 0; b < kBuckets; ++b)
			std::printf("\t%zu\t%zu\t%zu\t%zu", r.buckN[b], r.buckHole[b], r.keepN[b], r.keepHole[b]);
		for (int u = 0; u < 2; ++u)
			for (int b = 0; b < kTopBuckets; ++b)
				std::printf("\t%zu\t%zu", r.tbN[u][b], r.tbHit[u][b]);
		for (int b = 0; b < kClears; ++b)
			std::printf("\t%zu\t%zu", r.clN[b], r.clHole[b]);
		std::printf("\n");
		std::fflush(stdout);
	}
	return 0;
}
