// ksim_navcoll_export: dev-only dump of OUR baked navmesh and OUR collision shapes round one building, in the common format the original's
// export (kenshi-rwh.265) writes, so the two diff directly (kenshi-k16.7.122, review kenshi-k16.7.121). Off by default: built only with KSIM_DEV.
//
//   ksim_navcoll_export NAV OUTDIR [CASE...]       CASE = hubbar | longhouse (both when none is named)
//   ksim_navcoll_export NAV OUTDIR [--name N] [--aabb x0 y0 z0 x1 y1 z1] [--inst SUBSTR] --at X Z [RECORD]
//                                                  one case at the building nearest (X, Z) (of building record RECORD / whose instance id holds SUBSTR)
//   ksim_navcoll_export NAV OUTDIR [--name N] [--aabb ...] --place X Y Z QW QX QY QZ RECORD
//                                                  a building RECORD placed there (a player-town placement: not in the zone data); its collision
//                                                  through CollisionLibrary::addBuilding, the nav is whatever the mesh holds at that spot
//   ... --stand                                    with --place and a native NAV: the building is stood on the mesh first (the sim's own path),
//                                                  so the nav written is the patched one
//   ksim_navcoll_export --solve-yaw RECORD X Y Z QW QX QY QZ
//                                                  the placement quaternion (yaw about +y) whose placed convex shape at the actor origin (X, Y, Z)
//                                                  has the shape frame (QW..QZ): how the original's building pose is read from its shapes
//   ksim_navcoll_export --list X Z R               the buildings within R of (X, Z): id, record, position
//
// NAV: `native` = the runtime mesh (MeshNavigator::openNative: the shipped navtiles converted, stale tiles rebaked, nav cache under
// OUTDIR/_navcache), `native-shipped` = the same with the stale check off (the shipped navtiles as converted), a navworld bake directory
// (region files, loadRegion with 1 region of margin) or one .knav cache file. Writes OUTDIR/<case>/ (see the
// README.md it writes): region.txt nav_faces.tsv nav_edges.tsv nav_face_data.tsv nav_detail_tris.tsv col_shapes.tsv col_<n>.obj terrain.tsv.
// Game-derived geometry: never commit the output.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"
#include "kworld/loadorder.hpp"
#include "ksim/nav/navigator.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <numbers>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace fs = std::filesystem;
using kworld::WorldShape;

namespace {

struct CaseSpec
{
	const char* name;
	double x, z; // where to look for the building
	std::string record; // BUILDING record id the placement must have ("" = any)
	std::string instance; // substring of the placement's instance id ("" = any)
	bool hasAabb = false;
	double aabb[6] = {}; // min xyz, max xyz: the export region, overrides the building AABB + 50
	bool place = false; // a synthetic placement (--place) instead of a zone placement
	double pos[3] = {};
	double quat[4] = { 1, 0, 0, 0 }; // w x y z
};

// The Hub bar as the original's export has it (kenshi-rwh.265): zone 20,32 instance 4933-Newwworld-INGAME, Storm House 3384 at
// (-51181.1, 1576.2, 2639.0) yaw 313.6; the export region is the original's (region.txt of its export).
// The Desert Paradise Longhouse (rwh.270) is a player-town placement of the Storm_House_05 record 4028: no zone placement, so --place
// at the original's pose (quaternion from --solve-yaw, see the README); the quaternion is filled in main() from kLonghouseQuat.
const CaseSpec kCases[] = {
	{ "hubbar", -51181.133, 2638.970, "", "4933-Newwworld", true, { -51362.4, 1458.4, 2459.9, -51001.9, 1712.3, 2820.5 } },
	{ "longhouse", 29990.818, -83488.695, "4028-TwoStorey.mod", "", true, { 29738.5, 892.9, -83754.4, 30174.8, 1224.9, -83200.3 }, true, { 29990.818, 1016.181, -83488.695 }, { 0.5578, -0.0169, 0.8295, -0.0214 } },
};

struct Candidate
{
	std::vector<WorldShape> placed; // the model's shapes placed like the building's
	std::vector<bool> closed;
	std::string file, loader, part;
};

struct Owner
{
	std::string name, loader, file, kind;
};

std::string normalisePath(std::string path)
{
	for (char& c : path)
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	for (const char* prefix : { "./", "data/" })
		if (path.compare(0, std::string(prefix).size(), prefix) == 0)
			path.erase(0, std::string(prefix).size());
	return path;
}

class Files
{
public:
	Files()
	{
		const fs::path root{ std::string(KENSHI_DATA) };
		std::error_code ec;
		for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec))
				files_.emplace(normalisePath(it->path().lexically_relative(root).generic_string()), it->path().string());
	}
	const kworld::CollisionModel* model(const std::string& record)
	{
		const auto file = files_.find(normalisePath(record));
		if (record.empty() || file == files_.end())
			return nullptr;
		auto cached = models_.find(file->second);
		if (cached == models_.end())
		{
			std::ifstream in(file->second, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			std::unique_ptr<kworld::CollisionModel> parsed;
			try { parsed = std::make_unique<kworld::CollisionModel>(kworld::parseCollisionXml(text.str())); } catch (const kworld::Error&) {}
			cached = models_.emplace(file->second, std::move(parsed)).first;
		}
		return cached->second.get();
	}
private:
	std::unordered_map<std::string, std::string> files_;
	std::map<std::string, std::unique_ptr<kworld::CollisionModel>> models_;
};

bool near3(const float* a, const float* b)
{
	return std::fabs(a[0] - b[0]) < 2e-3f && std::fabs(a[1] - b[1]) < 2e-3f && std::fabs(a[2] - b[2]) < 2e-3f;
}

// Every part collision the record can have (any chance roll), placed as `placed` places it.
std::vector<Candidate> candidatesOf(const kdata::GameData& gd, Files& files, const kworld::PlacedInstance& placed)
{
	std::vector<Candidate> out;
	const kdata::Record* building = gd.find(placed.buildingID);
	if (building == nullptr)
		return out;
	const auto add = [&](const kdata::Record& part, const char* loader, const kdata::Record* owner)
	{
		const std::string& file = part.fileOr("xml collision");
		const std::string& destroyed = part.fileOr("destroyed collision");
		for (const std::string* path : { &file, &destroyed })
		{
			const kworld::CollisionModel* model = files.model(*path);
			if (model == nullptr)
				continue;
			Candidate c;
			c.file = *path;
			c.loader = loader;
			c.part = part.stringID + " \"" + part.name + "\"" + (owner != nullptr ? " of door " + owner->stringID : "");
			kworld::placeModel(*model, { placed.x, placed.y, placed.z }, { placed.qw, placed.qx, placed.qy, placed.qz }, placed.scale,
				kworld::NavUse::Walkable, true, c.placed);
			for (const kworld::CollisionShape& s : model->shapes)
				if (!s.indices.empty())
					c.closed.push_back(s.closed);
			out.push_back(std::move(c));
		}
	};
	for (const char* category : { "parts", "interior" })
		if (const auto* refs = building->findRefs(category))
			for (const auto& entry : refs->entries)
				if (const kdata::Record* part = gd.find(entry.targetID))
					add(*part, category[0] == 'p' ? "CollisionLibrary::addBuilding parts" : "CollisionLibrary::addBuilding interior", nullptr);
	if (const auto* doors = building->findRefs("doors"))
		for (const auto& entry : doors->entries)
			if (const kdata::Record* door = gd.find(entry.targetID))
				if (const auto* refs = door->findRefs("parts"))
					for (const auto& partRef : refs->entries)
						if (const kdata::Record* part = gd.find(partRef.targetID))
							add(*part, "CollisionLibrary::addBuilding doors", door);
	return out;
}

struct Frame
{
	std::array<double, 4> q; // w x y z
};

Frame quatOf(const std::array<float, 9>& a)
{
	// columns of R are the three axes triples
	const double m00 = a[0], m10 = a[1], m20 = a[2], m01 = a[3], m11 = a[4], m21 = a[5], m02 = a[6], m12 = a[7], m22 = a[8];
	const double tr = m00 + m11 + m22;
	double w, x, y, z;
	if (tr > 0)
	{
		const double s = std::sqrt(tr + 1.0) * 2;
		w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
		w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
	} else if (m11 > m22) {
		const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
		w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
	} else {
		const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
		w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
	}
	return { { w, x, y, z } };
}

std::string flagsOf(const WorldShape& s)
{
	static const char* use[] = { "Walkable", "Obstacle", "Projected", "Door" };
	std::string f = use[static_cast<int>(s.use)];
	for (const auto& [on, name] : { std::pair{ s.closed, "closed" }, { s.solid, "solid" }, { s.foliage, "foliage" }, { s.upperPart, "upper" },
			 { s.furniture, "furniture" }, { s.keepInside != nullptr, "clipped" }, { s.carveTerrain != nullptr, "carves_terrain" } })
		if (on)
			f += std::string("+") + name;
	return f;
}

struct Region
{
	double min[3], max[3];
};

bool overlaps(const WorldShape& s, const Region& r)
{
	for (int a = 0; a < 3; ++a)
		if (s.max[a] < r.min[a] || s.min[a] > r.max[a])
			return false;
	return true;
}

void writeObj(const fs::path& path, const WorldShape& s)
{
	std::ofstream out(path);
	out << "# world coordinates, game units, y up\n";
	for (std::size_t i = 0; i + 2 < s.vertices.size(); i += 3)
		out << "v " << s.vertices[i] << " " << s.vertices[i + 1] << " " << s.vertices[i + 2] << "\n";
	for (std::size_t i = 0; i + 2 < s.indices.size(); i += 3)
		out << "f " << s.indices[i] + 1 << " " << s.indices[i + 1] + 1 << " " << s.indices[i + 2] + 1 << "\n";
}

struct Context
{
	std::string dataDir = KENSHI_DATA;
	kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	kdata::GameData gd = kworld::loadMergedGameData(dataDir);
	kworld::InteriorLayouts layouts = kworld::InteriorLayouts::load(dataDir);
	Files files;
	kworld::CollisionLibrary library{ dataDir, gd };
};

// Names the shapes of one gathered building: the shell's from the record's parts by geometry, each furniture piece by its layout item.
std::vector<Owner> ownersOf(Context& ctx, const kworld::AreaBuilding& b)
{
	std::vector<Owner> owners;
	const kdata::Record* record = ctx.gd.find(b.placed.buildingID);
	const std::string name = (record != nullptr ? record->name : std::string("?")) + " [" + b.placed.buildingID + " @" + b.id + "]";
	std::vector<WorldShape> scratch;
	kworld::CollisionCoverage coverage;
	ctx.library.addBuilding(b.placed, scratch, coverage);
	const std::size_t shell = scratch.size();
	std::vector<Candidate> candidates = candidatesOf(ctx.gd, ctx.files, b.placed);
	for (std::size_t i = 0; i < shell && i < b.shapes->size(); ++i)
	{
		Owner o{ name, "CollisionLibrary::addBuilding", "?", "" };
		for (const Candidate& c : candidates)
			for (std::size_t k = 0; k < c.placed.size(); ++k)
			{
				const WorldShape& w = c.placed[k];
				const WorldShape& s = (*b.shapes)[i];
				if (w.vertices.size() == s.vertices.size() && w.indices.size() == s.indices.size() && near3(&w.vertices[0], &s.vertices[0])
					&& near3(&w.vertices[w.vertices.size() - 3], &s.vertices[s.vertices.size() - 3]))
				{
					o.name = name + " part " + c.part;
					o.loader = c.loader;
					o.file = c.file;
					o.kind = (w.half[0] > 0 && w.half[1] > 0 && w.half[2] > 0) ? "box" : (c.closed[k] ? "convex" : "trimesh");
					goto found;
				}
			}
		found:
		owners.push_back(o);
	}
	if (const kworld::InteriorLayout* layout = ctx.layouts.of(b.placed))
		for (const kworld::LayoutItem& item : layout->items)
		{
			const kworld::PlacedInstance piece = kworld::placeFurniture(b.placed, item, ctx.gd);
			std::vector<WorldShape> own;
			ctx.library.addBuilding(piece, own, coverage, false, true);
			const kdata::Record* r = ctx.gd.find(piece.buildingID);
			const std::string pieceName = "furniture " + piece.buildingID + " \"" + (r != nullptr ? r->name : "?") + "\" in " + name;
			std::vector<Candidate> cs = candidatesOf(ctx.gd, ctx.files, piece);
			for (const WorldShape& s : own)
			{
				Owner o{ pieceName, "CollisionLibrary::addFurniture", "?", "" };
				for (const Candidate& c : cs)
					for (std::size_t k = 0; k < c.placed.size(); ++k)
					{
						const WorldShape& w = c.placed[k];
						if (w.vertices.size() == s.vertices.size() && w.indices.size() == s.indices.size() && near3(&w.vertices[0], &s.vertices[0]))
						{
							o.file = c.file;
							o.kind = (w.half[0] > 0 && w.half[1] > 0 && w.half[2] > 0) ? "box" : (c.closed[k] ? "convex" : "trimesh");
							goto foundPiece;
						}
					}
				foundPiece:
				owners.push_back(o);
			}
		}
	if (owners.size() != b.shapes->size())
		std::cerr << "warning: " << b.id << ": " << owners.size() << " attributed owners for " << b.shapes->size() << " shapes (names may be shifted)\n";
	owners.resize(b.shapes->size(), Owner{ name, "?", "?", "" });
	return owners;
}

void exportCase(Context& ctx, const knav::NavMesh& mesh, const std::string& navSource, const fs::path& dir, const std::string& caseName,
	const kworld::AreaGeometry& area, const kworld::AreaBuilding& target, const CaseSpec& spec)
{
	fs::create_directories(dir);
	Region r{};
	if (spec.hasAabb)
		for (int a = 0; a < 3; ++a) { r.min[a] = spec.aabb[a]; r.max[a] = spec.aabb[3 + a]; }
	else
	{
		for (int a = 0; a < 3; ++a) { r.min[a] = 1e30; r.max[a] = -1e30; }
		for (const WorldShape& s : *target.shapes)
			for (int a = 0; a < 3; ++a) { r.min[a] = std::min<double>(r.min[a], s.min[a]); r.max[a] = std::max<double>(r.max[a], s.max[a]); }
		for (int a = 0; a < 3; ++a) { r.min[a] -= 50; r.max[a] += 50; }
	}
	const kdata::Record* rec = ctx.gd.find(target.placed.buildingID);
	{
		std::ofstream out(dir / "region.txt");
		const auto& p = target.placed;
		const double yaw = std::atan2(2 * (p.qw * p.qy + p.qx * p.qz), 1 - 2 * (p.qy * p.qy + p.qx * p.qx)) * 180 / std::numbers::pi;
		out << "case " << caseName << "\nbuilding " << (rec != nullptr ? rec->name : "?") << "\nid " << p.buildingID << "\ninstance " << target.id
			<< "\nposition " << p.x << " " << p.y << " " << p.z << "\nyaw_deg " << yaw << "\nquat_wxyz " << p.qw << " " << p.qx << " " << p.qy << " " << p.qz
			<< "\nscale " << p.scale << "\nlayout " << p.interiorLayout
			<< "\naabb_min " << r.min[0] << " " << r.min[1] << " " << r.min[2] << "\naabb_max " << r.max[0] << " " << r.max[1] << " " << r.max[2]
			<< "\naabb_rule " << (spec.hasAabb ? "the original's export AABB (its region.txt)" : "building world AABB (all its shapes) grown by 50 u on both sides, all three axes")
			<< "\nnav_source " << navSource << "\n";
	}

	// ---- navmesh
	const dtNavMesh& dt = mesh.detour();
	std::unordered_map<dtPolyRef, int> ids;
	const auto idOf = [&](dtPolyRef ref) { return ids.emplace(ref, static_cast<int>(ids.size())).first->second; };
	std::ofstream faces(dir / "nav_faces.tsv"), edges(dir / "nav_edges.tsv"), data(dir / "nav_face_data.tsv"), detail(dir / "nav_detail_tris.tsv");
	faces << "# face_id vertex_count then x y z per vertex, winding as stored by Detour\n";
	edges << "# face_id edge_index neighbour_face_id (-1 boundary; edge i runs vertex i -> i+1); a neighbour id outside nav_faces.tsv is a face outside the export AABB\n";
	data << "# face_id key value\n";
	detail << "# face_id then the 3 vertices (x y z) of one detail-mesh triangle (the heights Detour walks on)\n";
	std::size_t exported = 0, tilesHit = 0;
	std::map<int, std::size_t> bySize;
	for (int t = 0; t < dt.getMaxTiles(); ++t)
	{
		const dtMeshTile* tile = std::as_const(dt).getTile(t);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		const dtPolyRef base = dt.getPolyRefBase(tile);
		const knav::BakedTile* baked = mesh.tile(tile->header->x, tile->header->y);
		bool hit = false;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() != DT_POLYTYPE_GROUND)
				continue;
			bool inside = false;
			for (int v = 0; v < poly.vertCount && !inside; ++v)
			{
				const float* q = &tile->verts[3 * poly.verts[v]];
				inside = q[0] >= r.min[0] && q[0] <= r.max[0] && q[1] >= r.min[1] && q[1] <= r.max[1] && q[2] >= r.min[2] && q[2] <= r.max[2];
			}
			if (!inside)
				continue;
			hit = true;
			const dtPolyRef ref = base | static_cast<dtPolyRef>(p);
			const int id = idOf(ref);
			++exported;
			++bySize[poly.vertCount];
			faces << id << "\t" << static_cast<int>(poly.vertCount);
			for (int v = 0; v < poly.vertCount; ++v)
			{
				const float* q = &tile->verts[3 * poly.verts[v]];
				faces << "\t" << q[0] << "\t" << q[1] << "\t" << q[2];
			}
			faces << "\n";
			for (int e = 0; e < poly.vertCount; ++e)
			{
				bool any = false;
				for (unsigned k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
					if (tile->links[k].edge == e)
					{
						edges << id << "\t" << e << "\t" << idOf(tile->links[k].ref) << "\n";
						any = true;
					}
				if (!any)
					edges << id << "\t" << e << "\t-1\n";
			}
			data << id << "\tarea\t" << static_cast<int>(poly.getArea()) << "\n" << id << "\tflags\t" << poly.flags << "\n"
				<< id << "\ttile\t" << tile->header->x << "," << tile->header->y << "\n" << id << "\tpoly_index\t" << p << "\n";
			if (baked != nullptr)
				for (const knav::DoorPoly& d : baked->doors)
					if (d.poly == p)
						data << id << "\tdoor_key\t" << d.door << "\n";
			const dtPolyDetail& dm = tile->detailMeshes[p];
			for (int k = 0; k < dm.triCount; ++k)
			{
				const unsigned char* tri = &tile->detailTris[(dm.triBase + k) * 4];
				detail << id;
				for (int c = 0; c < 3; ++c)
				{
					const float* q = tri[c] < poly.vertCount ? &tile->verts[3 * poly.verts[tri[c]]]
															 : &tile->detailVerts[3 * (dm.vertBase + tri[c] - poly.vertCount)];
					detail << "\t" << q[0] << "\t" << q[1] << "\t" << q[2];
				}
				detail << "\n";
			}
		}
		tilesHit += hit;
	}

	// ---- collision
	std::ofstream shapes(dir / "col_shapes.tsv");
	shapes << "# shape_id owner kind group(use+flags) pos_x pos_y pos_z rot_w rot_x rot_y rot_z kind_data source_file loader\n"
		   << "# pos: box = centre, others = the actor origin; rot = the shape frame (actor pose x shape pose); box kind_data = half extents hx hy hz,\n"
		   << "# convex/trimesh = v=<verts> t=<tris> obj=<file> (world coordinates). Our loader folds capsules and spheres into convex hulls (kind convex).\n";
	int shapeId = 0;
	std::size_t counts[3] = {};
	const auto emit = [&](const WorldShape& s, const Owner& o)
	{
		if (!overlaps(s, r))
			return;
		std::string kind = o.kind;
		if (kind.empty())
			kind = (s.half[0] > 0 && s.half[1] > 0 && s.half[2] > 0) ? "box" : (s.closed || s.solid ? "convex" : "trimesh");
		const Frame f = quatOf(s.axes);
		std::array<double, 3> pos = { s.actor[0], s.actor[1], s.actor[2] };
		if (kind == "box")
		{
			pos = { 0, 0, 0 };
			const std::size_t n = s.vertices.size() / 3;
			for (std::size_t i = 0; i < n; ++i)
				for (int a = 0; a < 3; ++a)
					pos[a] += s.vertices[3 * i + a] / static_cast<double>(n);
		}
		const int id = shapeId++;
		shapes << id << "\t" << o.name << "\t" << kind << "\t" << flagsOf(s) << "\t" << pos[0] << "\t" << pos[1] << "\t" << pos[2] << "\t" << f.q[0] << "\t"
			   << f.q[1] << "\t" << f.q[2] << "\t" << f.q[3] << "\t";
		if (kind == "box")
			shapes << s.half[0] << " " << s.half[1] << " " << s.half[2];
		else
			shapes << "v=" << s.vertices.size() / 3 << " t=" << s.indices.size() / 3 << " obj=col_" << id << ".obj";
		shapes << "\t" << o.file << "\t" << o.loader << "\n";
		writeObj(dir / ("col_" + std::to_string(id) + ".obj"), s);
		++counts[kind == "box" ? 0 : kind == "convex" ? 1 : 2];
	};
	for (const kworld::AreaBuilding& b : area.buildings)
	{
		bool any = false;
		for (const WorldShape& s : *b.shapes)
			any = any || overlaps(s, r);
		if (!any)
			continue;
		const std::vector<Owner> owners = ownersOf(ctx, b);
		for (std::size_t i = 0; i < b.shapes->size(); ++i)
			emit((*b.shapes)[i], owners[i]);
	}
	if (area.foliage != nullptr)
		for (const WorldShape& s : *area.foliage)
			emit(s, Owner{ "foliage", "CollisionLibrary::addFoliage", "?", "" });

	// ---- terrain under the box
	{
		std::ofstream out(dir / "terrain.tsv");
		out << "# x z y: the heightmap every 4 u inside the export AABB (xz)\n";
		for (double z = r.min[2]; z <= r.max[2]; z += 4)
			for (double x = r.min[0]; x <= r.max[0]; x += 4)
				out << x << "\t" << z << "\t" << ctx.terrain.height(x, z) << "\n";
	}
	{
		std::ofstream out(dir / "README.md");
		out << "# ours / " << caseName << "\n\nWritten by `ksim_navcoll_export` (kenshi-k16.7.122); format of kenshi-k16.7.121. Game units, world coordinates, y up.\n\n"
			<< "- navmesh source: " << navSource << ". Detour tiles; the bake recipe in docs/formats/NAVMESH_CACHE.md: cell 2 u x 1 u, agent height 18, radius 4,\n"
			<< "  climb 5, slope 40, tiles 256 u). Faces with any vertex inside the AABB: " << exported << " in " << tilesHit << " tiles. `nav_detail_tris.tsv` holds the detail mesh\n"
			<< "  (the surface heights actually walked on); `nav_faces.tsv` polygons are the flat-ish Detour polygons.\n"
			<< "- collision: the WorldShapes the bake and the sim use (kworld CollisionLibrary), every shape overlapping the AABB: " << counts[0] << " box, " << counts[1]
			<< " convex, " << counts[2] << " trimesh. Owners/files are matched by geometry against the record's parts.\n"
			<< "- building-local transform: world = P + Q * local (Q the quaternion w x y z of region.txt, a rotation about +y by yaw_deg; P = `position`), so\n"
			<< "  local = conj(Q) * (world - P). Used: P = " << target.placed.x << " " << target.placed.y << " " << target.placed.z << ", Q = " << target.placed.qw << " "
			<< target.placed.qx << " " << target.placed.qy << " " << target.placed.qz << (spec.place ? " (placed by --place, not a zone placement)" : "")
			<< ". A shape's rot in col_shapes.tsv is in the world frame (actor pose x shape pose); its building-local rotation is conj(Q) * rot.\n"
			<< "- Not game-data to commit: keep out of the repo.\n";
	}
	std::cout << caseName << ": " << target.id << " " << target.placed.buildingID << " at " << target.placed.x << " " << target.placed.y << " " << target.placed.z
			  << "; nav faces " << exported << " (tiles " << tilesHit << ", by vertices:";
	for (const auto& [n, c] : bySize)
		std::cout << " " << n << "v=" << c;
	std::cout << "), collision shapes " << shapeId << " (box " << counts[0] << ", convex " << counts[1] << ", trimesh " << counts[2] << ")\n";
}

knav::NavMesh loadNav(const std::string& source, kworld::ZoneCell cell)
{
	if (fs::is_directory(source))
		return knav::NavMesh::loadRegion(source, cell.x / knav::kRegionZones, cell.z / knav::kRegionZones, 1);
	return knav::NavMesh::load(source);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc >= 5 && std::string(argv[1]) == "--list")
		{
			Context ctx;
			const double x = std::atof(argv[2]), z = std::atof(argv[3]), radius = std::atof(argv[4]);
			const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
			const knav::AreaGeometry area = knav::gatherArea(ctx.dataDir, ctx.terrain, cell, cell);
			for (const kworld::AreaBuilding& b : area.buildings)
				if (std::hypot(b.placed.x - x, b.placed.z - z) <= radius)
				{
					const kdata::Record* rec = ctx.gd.find(b.placed.buildingID);
					std::cout << b.id << "\t" << b.placed.buildingID << "\t" << (rec != nullptr ? rec->name : "?") << "\t" << b.placed.x << " " << b.placed.y << " "
							  << b.placed.z << "\tlayout=" << b.placed.interiorLayout << "\towner=" << b.placed.ownerFaction << "\tshapes=" << b.shapes->size() << "\n";
				}
			return 0;
		}
		if (argc >= 3 && std::string(argv[1]) == "--find")
		{
			Context ctx;
			const kdata::GameData& gd = ctx.gd;
			for (const kdata::Record& rec : gd.records())
				if (rec.type == kworld::itemTypeByName("BUILDING") && rec.name.find(argv[2]) != std::string::npos)
					std::cout << rec.stringID << "\t" << rec.name << "\n";
			return 0;
		}
		if (argc >= 10 && std::string(argv[1]) == "--solve-yaw")
		{
			Context ctx;
			kworld::PlacedInstance p;
			p.buildingID = argv[2];
			p.x = std::atof(argv[3]); p.y = std::atof(argv[4]); p.z = std::atof(argv[5]);
			const double target[4] = { std::atof(argv[6]), std::atof(argv[7]), std::atof(argv[8]), std::atof(argv[9]) };
			const float origin[3] = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
			// The placed frame is R_place * R_local; with the identity placement the shapes give R_local, so R_place = target * conj(R_local).
			// A building placed with a tilt (the original's placement quaternions carry small x / z parts) is not a pure yaw.
			p.qw = 1; p.qx = p.qy = p.qz = 0;
			std::map<std::string, std::size_t> votes;
			for (const Candidate& c : candidatesOf(ctx.gd, ctx.files, p))
				for (const WorldShape& s : c.placed)
				{
					const float actor[3] = { static_cast<float>(s.actor[0]), static_cast<float>(s.actor[1]), static_cast<float>(s.actor[2]) };
					if (s.indices.empty() || !near3(actor, origin))
						continue;
					const std::array<double, 4> l = quatOf(s.axes).q; // w x y z
					const double cw = l[0], cx = -l[1], cy = -l[2], cz = -l[3]; // conj
					double q[4] = { target[0] * cw - target[1] * cx - target[2] * cy - target[3] * cz,
						target[0] * cx + target[1] * cw + target[2] * cz - target[3] * cy,
						target[0] * cy - target[1] * cz + target[2] * cw + target[3] * cx,
						target[0] * cz + target[1] * cy - target[2] * cx + target[3] * cw };
					if (q[0] < 0)
						for (double& v : q) v = -v;
					char key[160];
					std::snprintf(key, sizeof key, "%.4f %.4f %.4f %.4f", q[0], q[1], q[2], q[3]);
					++votes[key];
				}
			for (const auto& [key, n] : votes)
				std::printf("placement quaternion w x y z = %s  (%zu convex shapes)\n", key.c_str(), n);
			return 0;
		}
		if (argc < 3)
		{
			std::cerr << "usage: ksim_navcoll_export NAV OUTDIR [hubbar|longhouse|--at X Z [ID]]  |  --list X Z R\n";
			return 2;
		}
		const std::string nav = argv[1];
		const fs::path out = argv[2];
		std::vector<CaseSpec> cases;
		std::list<std::string> names; // stable storage for CaseSpec::name
		CaseSpec pending{ "at", 0, 0, "" };
		for (int i = 3; i < argc; ++i)
		{
			const std::string a = argv[i];
			const auto num = [&](int k) { return std::atof(argv[i + k]); };
			if (a == "--name" && i + 1 < argc)
			{
				names.push_back(argv[++i]);
				pending.name = names.back().c_str();
			}
			else if (a == "--inst" && i + 1 < argc) pending.instance = argv[++i];
			else if (a == "--aabb" && i + 6 < argc)
			{
				pending.hasAabb = true;
				for (int k = 0; k < 6; ++k) pending.aabb[k] = num(1 + k);
				i += 6;
			}
			else if (a == "--at" && i + 2 < argc)
			{
				pending.x = num(1);
				pending.z = num(2);
				i += 2;
				if (i + 1 < argc && argv[i + 1][0] != '-') pending.record = argv[++i];
				cases.push_back(pending);
				pending = CaseSpec{ "at", 0, 0, "" };
			}
			else if (a == "--place" && i + 8 < argc)
			{
				pending.place = true;
				for (int k = 0; k < 3; ++k) pending.pos[k] = num(1 + k);
				for (int k = 0; k < 4; ++k) pending.quat[k] = num(4 + k);
				pending.x = pending.pos[0];
				pending.z = pending.pos[2];
				pending.record = argv[i + 8];
				i += 8;
				cases.push_back(pending);
				pending = CaseSpec{ "at", 0, 0, "" };
			}
			else
				for (const CaseSpec& c : kCases)
					if (a == c.name)
					{
						cases.push_back(c);
						if (std::string(pending.name) != "at") // --name / --aabb before a named case override its own
							cases.back().name = pending.name;
						if (pending.hasAabb)
						{
							cases.back().hasAabb = true;
							std::copy(std::begin(pending.aabb), std::end(pending.aabb), cases.back().aabb);
						}
						pending = CaseSpec{ "at", 0, 0, "" };
					}
		}
		if (cases.empty())
			cases.assign(std::begin(kCases), std::end(kCases));
		Context ctx;
		std::shared_ptr<ksim::MeshNavigator> navigator;
		bool stand = false; // --stand: a --place building is stood on the native mesh first (MeshNavigator::standBuilding, kenshi-k16.7.125)
		for (int i = 3; i < argc; ++i)
			stand = stand || std::string(argv[i]) == "--stand";
		for (CaseSpec& c : cases)
		{
			const kworld::ZoneCell cell = kworld::zoneCellOf(c.x, c.z);
			kworld::AreaGeometry area = knav::gatherArea(ctx.dataDir, ctx.terrain, cell, cell);
			const kworld::AreaBuilding* best = nullptr;
			if (c.place)
			{
				kworld::AreaBuilding synthetic;
				kworld::PlacedInstance& p = synthetic.placed;
				p.instanceID = std::string("placed-") + c.name;
				p.buildingID = c.record;
				p.x = c.pos[0]; p.y = c.pos[1]; p.z = c.pos[2];
				p.qw = c.quat[0]; p.qx = c.quat[1]; p.qy = c.quat[2]; p.qz = c.quat[3];
				p.scale = 1.0f;
				p.hasScale = ctx.gd.find(c.record) != nullptr;
				p.cell = p.positionCell = cell;
				std::vector<kworld::WorldShape> shapes;
				const auto navInfo = ctx.library.addBuilding(p, shapes, area.coverage);
				if (!navInfo)
				{
					std::cerr << c.name << ": record " << c.record << " has no collision\n";
					continue;
				}
				synthetic.nav = *navInfo;
				synthetic.id = "placed/" + c.record;
				synthetic.shapes = std::make_shared<const std::vector<kworld::WorldShape>>(std::move(shapes));
				area.buildings.push_back(std::move(synthetic));
				best = &area.buildings.back();
			} else {
				double bestDistance = 1e30;
				for (const kworld::AreaBuilding& b : area.buildings)
				{
					if (b.shapes->empty() || (!c.record.empty() && b.placed.buildingID != c.record)
						|| (!c.instance.empty() && b.placed.instanceID.find(c.instance) == std::string::npos))
						continue;
					const double d = std::hypot(b.placed.x - c.x, b.placed.z - c.z);
					if (d < bestDistance)
					{
						bestDistance = d;
						best = &b;
					}
				}
				if (best == nullptr)
				{
					std::cerr << c.name << ": no matching building in zone " << cell.x << "," << cell.z << " (try --list)\n";
					continue;
				}
				std::cout << c.name << ": nearest match " << bestDistance << " u away\n";
			}
			std::unique_ptr<knav::NavMesh> own;
			const knav::NavMesh* mesh = nullptr;
			if (nav == "native" || nav == "native-shipped")
			{
				ksim::NativeCacheOptions cache;
				cache.dir = (out / "_navcache").string();
				cache.worldKey = kworld::dataHash(kworld::resolveLoadOrder(KENSHI_DATA));
				cache.shippedOnly = nav == "native-shipped";
				std::vector<kworld::ZoneCell> zones;
				for (int dz = -1; dz <= 1; ++dz)
					for (int dx = -1; dx <= 1; ++dx)
						zones.push_back({ cell.x + dx, cell.z + dz });
				navigator = ksim::MeshNavigator::openNative(KENSHI_DATA, zones, {}, &ctx.gd, cache);
				std::size_t patched = 0;
				if (stand && c.place && navigator->standBuilding(best->placed, &patched))
					std::cout << c.name << ": stood on the mesh, " << patched << " tiles patched\n";
				mesh = &navigator->mesh();
			} else {
				own = std::make_unique<knav::NavMesh>(loadNav(nav, cell));
				mesh = own.get();
			}
			const std::string source = nav == "native" ? "the runtime mesh (MeshNavigator::openNative, the sim's default: shipped navtiles converted to Detour, stale tiles rebaked with our recipe)"
				: nav == "native-shipped" ? "the shipped navtiles converted to Detour, no stale-tile rebake (openNative shippedOnly)"
				: "navworld bake / cache file " + nav + " (knav_bake output, not the runtime default)";
			exportCase(ctx, *mesh, source, out / c.name, c.name, area, *best, c);
		}
	} catch (const std::exception& e) {
		std::cerr << "ksim_navcoll_export: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
