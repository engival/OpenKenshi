#include "world/worldbuildings.hpp"
#include "world/worldbuildings_internal.hpp"
#include "render/drawstats.hpp"
#include "dev/lightterms.hpp"
#include "render/ao.hpp"
#include "render/speccube.hpp"
#include "world/weathermap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "app/config.hpp"
#include "interiormask.glsl.h"
#include "kworld/coords.hpp"
#include "mesh.glsl.h"
#include "stb_image.h"


using namespace wbi;

void WorldBuildings::setEditor(bool on, const double pos[3])
{
	editorOn_ = on;
	if (on) { for (int k = 0; k < 3; k++) { editorPos_[k] = pos[k]; } }
}

bool WorldBuildings::propRay(uint64_t id, const double origin[3], const double dir[3], double& t, bool drawn) const
{
	const auto it = propMeshes_.find(id);
	return it != propMeshes_.end() && instanceRay(it->second, origin, dir, t, drawn);
}

// The nearest drawn surface the ray meets within t in [tMin, tMax] on a building shell or a layout piece standing in the world box lo..hi (a collision solid's):
// a solid is backed by what is drawn there, or it is an undrawn proxy volume (the staircase body of Storm House reaches 10 u above its treads, k16.2.239.4.1).
bool WorldBuildings::drawnHitIn(const double origin[3], const double dir[3], const double lo[3], const double hi[3], double tMin, double tMax, double& t) const
{
	bool hit = false;
	const auto test = [&](uint32_t i)
	{
		double tt;
		if (instanceRay(i, origin, dir, tt, true, tMin, tMax) && (!hit || tt < t)) { t = tt; hit = true; }
	};
	for (const uint32_t i : topLevel_) { test(i); }
	for (double x = std::floor(lo[0] / 4) * 4 - 4; x <= hi[0] + 4; x += 4)
	{
		for (double z = std::floor(lo[2] / 4) * 4 - 4; z <= hi[2] + 4; z += 4)
		{
			const auto g = furnGrid_.find(furnKey(x, z));
			if (g != furnGrid_.end()) { for (const uint32_t i : g->second) { test(i); } }
		}
	}
	return hit;
}

bool WorldBuildings::groundDrawn(double x, double z, double wireY, double up, double down, double& y) const
{
	const double o[3] = { x, wireY, z }, dn[3] = { 0, -1, 0 }, upd[3] = { 0, 1, 0 };
	const double lo[3] = { x - 1, wireY - down, z - 1 }, hi[3] = { x + 1, wireY + up, z + 1 };
	double tDown = 0, tUp = 0;
	const bool d = drawnHitIn(o, dn, lo, hi, 0, down, tDown), u = drawnHitIn(o, upd, lo, hi, 0, up, tUp);
	if (!d && !u) { return false; }
	y = d && (!u || tDown <= tUp) ? wireY - tDown : wireY + tUp;
	return true;
}

int WorldBuildings::layoutPieceInstance(uint64_t building, uint32_t piece) const
{
	const auto it = layoutPieces_.find(std::make_pair(building, piece));
	return it == layoutPieces_.end() ? -1 : (int)it->second;
}

// One part's mesh box (the box instanceRay tests, mesh bmin/bmax plus the part's offset) as a world AABB. False: an effect part, an unloaded mesh.
bool WorldBuildings::partWorldBox(const BuildingInstance& b, uint32_t k, double lo[3], double hi[3]) const
{
	const uint32_t pid = data_.partRefs[b.firstPart + k];
	const BuildingPartDef& def = data_.parts[pid];
	const int mesh = partRt_[pid].mesh;
	if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { return false; }
	const MeshEntry& m = meshes_[mesh];
	double off[3];
	toMesh(offsetFrame, def.offset, off);
	for (int a = 0; a < 3; a++) { lo[a] = 1e30; hi[a] = -1e30; }
	for (int c = 0; c < 8; c++)	// the box's corners in the mesh frame, turned by the instance and moved to its world position
	{
		const double p[3] = { (c & 1 ? m.bmax[0] : m.bmin[0]) + off[0], (c & 2 ? m.bmax[1] : m.bmin[1]) + off[1], (c & 4 ? m.bmax[2] : m.bmin[2]) + off[2] };
		const float q[4] = { b.qw, b.qx, b.qy, b.qz };
		double r[3];
		rotate(q, p, r);
		for (int a = 0; a < 3; a++)
		{
			const double v = r[a] * b.scale * b.stretch[a] + (a == 0 ? b.x : a == 1 ? b.y : b.z);
			lo[a] = std::min(lo[a], v);
			hi[a] = std::max(hi[a], v);
		}
	}
	return true;
}

// The world box of everything a prop draws (the union of its parts' boxes, this frame), grown by `grow`. False: no such prop, or no part of it draws.
bool WorldBuildings::propBox(uint64_t id, double lo[3], double hi[3], double grow) const
{
	const int index = propInstance(id);
	if (index < 0) { return false; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	bool any = false;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		double plo[3], phi[3];
		if (!partWorldBox(b, k, plo, phi)) { continue; }
		for (int a = 0; a < 3; a++) { lo[a] = any ? std::min(lo[a], plo[a]) : plo[a]; hi[a] = any ? std::max(hi[a], phi[a]) : phi[a]; }
		any = true;
	}
	if (!any) { return false; }
	for (int a = 0; a < 3; a++) { lo[a] -= grow; hi[a] += grow; }
	return true;
}

// The instance a prop is drawn and picked by and, per part, the mesh box the ray tests (instanceRay) as a world AABB
std::string WorldBuildings::propReport(uint64_t id) const
{
	const int index = propInstance(id);
	if (index < 0) { return "prop " + std::to_string(id) + ": no instance\n"; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	std::string out = "prop " + std::to_string(id) + ": instance " + std::to_string(index) + " at " + std::to_string(b.x) + " " + std::to_string(b.y) + " " + std::to_string(b.z) +
		" scale " + std::to_string(b.scale) + " parts " + std::to_string(b.partCount) + (propFromLayout(id) ? " from-layout" : "") + "\n";
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		double lo[3], hi[3];
		if (!partWorldBox(b, k, lo, hi)) { continue; }
		char line[256];
		snprintf(line, sizeof line, "  part %u box %.3f %.3f %.3f - %.3f %.3f %.3f\n", k, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
		out += line;
	}
	return out;
}

// A bed (SIMLINK INTERACTABLE kind 5) is a layout furniture piece, not a prop: the piece whose oriented mesh box (grown by kBedSlack) holds the sleeping
// spot `at`, the nearest to it when several do; the ray enters its box at t. -1: no piece holds the spot, 0: the ray misses it, 1: hit.
int WorldBuildings::bedRay(const double at[3], const double origin[3], const double dir[3], double& t) const
{
	// The piece depends on the spot only (not the ray): bedPiece once per spot and generation, the ray test per call (kenshi-k16.2.205.12.111)
	uint64_t key = 1469598103934665603ull;
	for (int a = 0; a < 3; a++) { uint64_t bits; std::memcpy(&bits, &at[a], sizeof bits); key = (key ^ bits) * 1099511628211ull; }
	auto it = bedMemo_.find(key);
	if (it == bedMemo_.end() || it->second.dataGen != dataGen_ || it->second.holdGen != holdGen_ || it->second.at[0] != at[0] || it->second.at[1] != at[1] || it->second.at[2] != at[2])
	{
		if (it == bedMemo_.end() && bedMemo_.size() >= 4096) { bedMemo_.clear(); }	// beds of zones left behind
		BedMemo m = { { at[0], at[1], at[2] }, dataGen_, holdGen_, bedPiece(at) };
		it = bedMemo_.insert_or_assign(key, m).first;
	}
	const int found = it->second.piece;
	return found < 0 ? -1 : instanceRay((uint32_t)found, origin, dir, t) ? 1 : 0;
}

// Whether point p lies in one of the instance's part boxes, each grown by slack (mesh units)
bool WorldBuildings::instanceHolds(uint32_t index, const double p[3], double slack) const
{
	const BuildingInstance& b = data_.instances[index];
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double rel[3] = { p[0] - b.x, p[1] - b.y, p[2] - b.z };
	double o[3];
	rotate(conj, rel, o);
	for (int a = 0; a < 3; a++) { o[a] /= b.scale * b.stretch[a]; }
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		const MeshEntry& m = meshes_[mesh];
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		bool in = true;
		for (int a = 0; a < 3; a++) { in = in && o[a] >= m.bmin[a] + off[a] - slack && o[a] <= m.bmax[a] + off[a] + slack; }
		if (in) { return true; }
	}
	return false;
}

// The nearest hit of the ray (mesh frame, already offset-free: `off` is the part's offset) on the mesh's triangles; a mesh whose triangles cannot be
// read keeps its box (true, t unchanged).
bool WorldBuildings::triangleRay(int mesh, const double o[3], const double d[3], const double off[3], double& t, double tMin, double tMax) const
{
	const std::string& path = meshes_[mesh].path;
	auto it = pickTris_.find(path);
	if (it == pickTris_.end())
	{
		std::vector<float> tris;
		MeshCpu cpu;
		if (decodeMesh(path, cpu))
		{
			for (const Sub& s : cpu.subs)
			{
				for (int i = 0; i + 2 < s.indexCount; i += 3)
				{
					for (int c = 0; c < 3; c++)
					{
						const float* v = &cpu.verts[((size_t)s.firstVertex + cpu.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
						tris.insert(tris.end(), v, v + 3);
					}
				}
			}
		}
		it = pickTris_.emplace(path, std::move(tris)).first;
	}
	const std::vector<float>& tr = it->second;
	if (tr.empty()) { return true; }
	double best = 1e30;
	const double ox = o[0] - off[0], oy = o[1] - off[1], oz = o[2] - off[2];
	for (size_t i = 0; i + 8 < tr.size(); i += 9)	// Moeller-Trumbore, both faces (a prop's back faces are drawn or hidden by the box anyway)
	{
		const float* a = &tr[i];
		const double e1[3] = { tr[i + 3] - (double)a[0], tr[i + 4] - (double)a[1], tr[i + 5] - (double)a[2] };
		const double e2[3] = { tr[i + 6] - (double)a[0], tr[i + 7] - (double)a[1], tr[i + 8] - (double)a[2] };
		const double p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
		const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
		if (std::abs(det) < 1e-14) { continue; }
		const double s[3] = { ox - a[0], oy - a[1], oz - a[2] };
		const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
		if (u < 0 || u > 1) { continue; }
		const double q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
		const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
		if (v < 0 || u + v > 1) { continue; }
		const double tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
		if (tt >= tMin && tt <= tMax && tt < best) { best = tt; }
	}
	if (best >= 1e30) { return false; }
	t = best;
	return true;
}

bool WorldBuildings::instanceRay(uint32_t index, const double origin[3], const double dir[3], double& t, bool drawn, double tMin, double tMax) const
{
	const BuildingInstance& b = data_.instances[index];
	// the ray in the instance's mesh frame; a linear map, so t is the same along both
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double rel[3] = { origin[0] - b.x, origin[1] - b.y, origin[2] - b.z };
	double o[3], d[3];
	rotate(conj, rel, o);
	rotate(conj, dir, d);
	for (int a = 0; a < 3; a++) { o[a] /= b.scale * b.stretch[a]; d[a] /= b.scale * b.stretch[a]; }
	bool hit = false;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		const MeshEntry& m = meshes_[mesh];
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		double t0 = 0, t1 = 1e30;
		for (int a = 0; a < 3 && t0 <= t1; a++)
		{
			const double lo = m.bmin[a] + off[a] - o[a], hi = m.bmax[a] + off[a] - o[a];
			if (std::abs(d[a]) < 1e-12) { if (lo > 0 || hi < 0) { t1 = -1; } continue; }
			const double ta = lo / d[a], tb = hi / d[a];
			t0 = std::max(t0, std::min(ta, tb));
			t1 = std::min(t1, std::max(ta, tb));
		}
		if (t0 > t1 || t1 < tMin || t0 > tMax) { continue; }	// the box's span along the ray misses the window
		if (drawn && !triangleRay(mesh, o, d, off, t0, tMin, tMax)) { continue; }	// the box is only the cheap test; the drawn triangles decide
		if (!hit || t0 < t) { t = t0; hit = true; }
	}
	return hit;
}


// ---- interiors: floors and the inside test ----

const WorldBuildings::BInfo* WorldBuildings::infoFor(uint32_t bi)
{
	if (const auto it = binfo_.find(bi); it != binfo_.end()) { return &it->second; }
	const BuildingInstance& b = data_.instances[bi];
	float maskMn[3] = { 1e30f, 1e30f, 1e30f }, maskMx[3] = { -1e30f, -1e30f, -1e30f }, groundMn[3] = { 1e30f, 1e30f, 1e30f }, groundMx[3] = { -1e30f, -1e30f, -1e30f };
	float interiorLow[4] = { 1e30f, 1e30f, 1e30f, 1e30f };	// lowest point of the interior parts of each floor
	bool mask = false, ground = false;
	BInfo info;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (mesh == -2) { return nullptr; }
		if (mesh < 0 || meshes_[mesh].state == Failed) { continue; }
		const MeshEntry& m = meshes_[mesh];
		if (m.state != Ready) { return nullptr; }
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		float lo[3], hi[3];
		for (int a = 0; a < 3; a++)
		{
			lo[a] = m.bmin[a] + (float)off[a];
			hi[a] = m.bmax[a] + (float)off[a];
		}
		for (int a = 0; a < 3; a++) { info.outerMn[a] = std::min(info.outerMn[a], lo[a]); info.outerMx[a] = std::max(info.outerMx[a], hi[a]); }
		if (def.kind == 2) { mask = true; for (int a = 0; a < 3; a++) { maskMn[a] = std::min(maskMn[a], lo[a]); maskMx[a] = std::max(maskMx[a], hi[a]); } }
		else if (def.kind == 1 || def.floor == 0) { ground = true; for (int a = 0; a < 3; a++) { groundMn[a] = std::min(groundMn[a], lo[a]); groundMx[a] = std::max(groundMx[a], hi[a]); } }
		if (def.kind == 1) { interiorLow[def.floor] = std::min(interiorLow[def.floor], lo[1]); }
	}
	info.hasBounds = mask || ground;
	for (int a = 0; a < 3; a++)
	{
		info.mn[a] = mask ? maskMn[a] : groundMn[a];
		info.mx[a] = mask ? maskMx[a] : groundMx[a];
	}
	// A floor starts at its interior slab. Most buildings have none (the interior mesh holds every storey with `building floor` 0, the shell parts of a floor start
	// at the ground), so the interior's height is split evenly between the floors instead.
	for (int f = 1; f <= b.maxFloor && f < 4; f++)
	{
		info.floorY[f] = interiorLow[f] < 1e29f ? interiorLow[f] : info.hasBounds ? info.mn[1] + (info.mx[1] - info.mn[1]) * f / (b.maxFloor + 1) : 1e30f;
	}
	return &binfo_.emplace(bi, info).first->second;
}

std::string WorldBuildings::recordOf(uint64_t key) const
{
	const auto it = key ? data_.byKey.find(key) : data_.byKey.end();
	return it == data_.byKey.end() ? std::string() : data_.names[data_.instances[it->second].nameId];
}

std::string WorldBuildings::nameOfInstance(int index) const
{
	if (index < 0 || (size_t)index >= data_.instances.size()) { return std::string(); }
	const uint32_t n = data_.instances[(size_t)index].nameId;
	return n < data_.names.size() ? data_.names[n] : std::string();
}

// The layout furniture piece a bed's sleeping spot `at` lies in: the piece whose oriented mesh box (grown by kBedSlack) holds the spot, the nearest
// to it when several do; -1 none. bedRay's search (through its memo) and the F12 shot's pick lines (kenshi-k16.2.234.1).
int WorldBuildings::bedPiece(const double at[3]) const
{
	constexpr double kBedSlack = 2, kBedSearch = 12;
	double bestD = 1e30;
	int found = -1;
	for (int dx = -3; dx <= 3; dx++)
	{
		for (int dz = -3; dz <= 3; dz++)
		{
			const auto g = furnGrid_.find(furnKey(at[0] + 4 * dx, at[2] + 4 * dz));
			if (g == furnGrid_.end()) { continue; }
			for (const uint32_t i : g->second)
			{
				const BuildingInstance& b = data_.instances[i];
				const double d = std::hypot(b.x - at[0], b.z - at[2]);
				if (d > kBedSearch || d >= bestD || !instanceHolds(i, at, kBedSlack)) { continue; }
				bestD = d;
				found = (int)i;
			}
		}
	}
	return found;
}

// One line on an instance for the F12 shot: its BUILDING name, placement, parent, kind and the part mesh files with the union of their loaded boxes
// in mesh units (the oriented box the pick uses, before the instance's rotation and scale)
std::string WorldBuildings::describeInstance(int index) const
{
	if (index < 0 || (size_t)index >= data_.instances.size()) { return "no instance"; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	std::string name = nameOfInstance(index);
	if (name.empty()) { name = "no record name"; }
	char head[256];
	snprintf(head, sizeof head, "instance %d %s at %.1f %.1f %.1f scale %.2f%s%s%s", index, name.c_str(), b.x, b.y, b.z, b.scale, b.parent >= 0 ? " (layout piece in building instance " : "",
		b.parent >= 0 ? std::to_string(b.parent).c_str() : "", b.parent >= 0 ? ")" : b.prop ? " (prop)" : b.feature ? " (feature)" : "");
	std::string files;
	double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		if (def.kind == 2) { continue; }
		if (files.size() < 160) { files += (files.empty() ? "" : ", ") + def.mesh; }
		const int mesh = partRt_[pid].mesh;
		if (mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], meshes_[mesh].bmin[a] + off[a]); hi[a] = std::max(hi[a], meshes_[mesh].bmax[a] + off[a]); }
	}
	char box[160] = "";
	if (lo[0] < 1e29) { snprintf(box, sizeof box, " mesh box %.0f %.0f %.0f - %.0f %.0f %.0f", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); }
	return std::string(head) + "; mesh " + (files.empty() ? "none" : files) + box;
}

// The floorable building instance with the wire's `building` key, -1 = none (outside, or a building without floors)
int WorldBuildings::floorableOf(uint64_t key) const
{
	const auto it = key ? data_.byKey.find(key) : data_.byKey.end();
	return it != data_.byKey.end() && data_.instances[it->second].maxFloor > 0 ? (int)it->second : -1;
}


WorldBuildings::Stats WorldBuildings::stats() const { return stats_; }

std::string WorldBuildings::fitReport() const
{
	auto pct = [](std::vector<float> v, double p) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0f : v[(size_t)(p * (v.size() - 1))]; };
	char buf[256];
	snprintf(buf, sizeof buf, "n=%zu; lowest mesh point - terrain under origin p10/p50/p90 = %.1f/%.1f/%.1f; record relative y p10/p50/p90 = %.1f/%.1f/%.1f",
		fitGround_.size(), pct(fitGround_, 0.1), pct(fitGround_, 0.5), pct(fitGround_, 0.9), pct(fitRel_, 0.1), pct(fitRel_, 0.5), pct(fitRel_, 0.9));
	return buf;
}

std::string WorldBuildings::materialReport() const
{
	std::string out;
	for (const MeshEntry& m : meshes_)
	{
		if (m.state != Ready) { continue; }
		bool odd = false;
		for (const Sub& s : m.subs) { odd = odd || lower(s.material) != "staticobject"; }
		if (!odd) { continue; }
		out += m.path + ":";
		for (const Sub& s : m.subs) { out += " " + s.material; }
		out += "\n";
	}
	return out;
}

// ---- light probes (kenshi-jj3.1.1) ----

namespace
{
constexpr float kProbeSpacing = 6.0f;	// mesh units between probes, as the lab (the Storm House came to 45 x 21 x 50)
constexpr float kProbeMargin = 12.0f;	// the grid reaches this far past the building's interior hull
constexpr float kProbeGroundAlbedo = 0.3f;	// what the bake's ground bounces; the shader's open-field reference (lightvol) is the same number
constexpr uint64_t kProbeFormat = 1;	// bump when the bake's output changes: it is part of the cache key
constexpr double kSunStep = 5.0 * 3.14159265358979 / 180.0;	// render.sun_bounce: re-bake when the sun moved this far (the task's 5 degrees; the lab dragged it by hand)

const uint8_t* texelAt(const std::vector<uint8_t>& rgba, int w, int h, float u, float v)
{
	if (rgba.empty()) { return nullptr; }
	const int x = std::clamp((int)((u - std::floor(u)) * (float)w), 0, w - 1), y = std::clamp((int)((v - std::floor(v)) * (float)h), 0, h - 1);
	return &rgba[((size_t)y * w + x) * 4];
}
float lumaAt(const std::vector<uint8_t>& rgba, int w, int h, float u, float v)
{
	const uint8_t* p = texelAt(rgba, w, h, u, v);
	return p ? (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f : kProbeGroundAlbedo;
}

}

float WorldBuildings::lightGroundAlbedo() { return kProbeGroundAlbedo; }

// The parts of building bi that the lamp occlusion traces (probeParts' set); empty until every mesh is loaded.
std::vector<WorldBuildings::ProbePart> WorldBuildings::probePartsFor(uint32_t bi)
{
	std::vector<ProbePart> parts;
	std::array<float, 3> mn, mx;
	float groundY = 0;
	if (!probeParts(bi, parts, mn, mx, groundY)) { parts.clear(); }
	return parts;
}

// The baked sky transfer of building bi's type: null until its bake finishes. The first ask starts the bake on the worker thread (or loads the cache) once the meshes are loaded;
// one bake runs at a time, so a caller asking nearest first gets the nearest baked first.
std::shared_ptr<const lightbake::Grid> WorldBuildings::gridFor(uint32_t bi)
{
	const BuildingInstance& b = data_.instances[bi];
	std::shared_ptr<const lightbake::Grid> grid;
	bool known;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		const auto it = probes_.find(b.nameId);
		known = it != probes_.end() && it->second.openWindows == kOpenWindows.get();	// a render.open_windows change re-bakes
		if (known) { grid = it->second.grid; }
		else if (it != probes_.end() && !probeBusy_) { probes_.erase(it); }
	}
	if (!known && !probeBusy_)
	{
		std::vector<ProbePart> parts;
		std::array<float, 3> mn, mx;
		float groundY = 0;
		if (probeParts(bi, parts, mn, mx, groundY))
		{
			if (probeThread_.joinable()) { probeThread_.join(); }	// the previous one has finished (probeBusy_ was false)
			{
				std::lock_guard<std::mutex> lk(probeMu_);
				probes_[b.nameId] = {};
				probes_[b.nameId].openWindows = kOpenWindows.get();
			}
			probeBusy_ = true;
			probeThread_ = std::thread([this, id = b.nameId, name = data_.names[b.nameId], parts = std::move(parts), mn, mx, groundY]() mutable { bakeProbes(id, name, std::move(parts), mn, mx, groundY); });
		}
	}
	return grid;
}

// The parts a bake traces, the grid bounds and the ground height of building bi: the same set for the sky and the sun bake. False until every mesh is loaded.
bool WorldBuildings::probeParts(uint32_t bi, std::vector<ProbePart>& parts, std::array<float, 3>& mn, std::array<float, 3>& mx, float& groundY)
{
	const BuildingInstance& b = data_.instances[bi];
	for (uint32_t k = 0; k < b.partCount; k++) { ensurePart(data_.partRefs[b.firstPart + k]); }	// queue the meshes (update() does for the ones in view)
	const BInfo* info = infoFor(bi);	// null until every mesh is loaded
	if (!info) { return false; }
	// render.open_windows: the bake is keyed by the switch, not by what the parts turned out to hold, so it waits until every part's barrier
	// classification is final (resolveOpenWindows waits for the atlases); retried next call.
	bool anyOpen = false;
	if (kOpenWindows.get())
	{
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const PartRt& rt = partRt_[data_.partRefs[b.firstPart + k]];
			if (!rt.openDone) { return false; }
			if (rt.openHas) { anyOpen = true; }
		}
	}
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const BuildingPartDef& def = data_.parts[data_.partRefs[b.firstPart + k]];
		const int mesh = partRt_[data_.partRefs[b.firstPart + k]].mesh;
		// no mask hulls (kind 2), black cut caps, position markers, glass / foliage materials (they let the light through); furniture is not a part
		if (mesh < 0 || def.kind == 2 || def.blackCap || def.marker || def.featureMode >= 0 || def.materialType == 1 || def.materialType == 2) { continue; }
		if (anyOpen && partRt_[data_.partRefs[b.firstPart + k]].openHide) { continue; }	// the black lining behind an opening this switch opens
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		parts.push_back({ meshes_[mesh].path, def, { (float)off[0], (float)off[1], (float)off[2] } });
	}
	const float* lo = info->hasBounds ? info->mn : info->outerMn;
	const float* hi = info->hasBounds ? info->mx : info->outerMx;
	groundY = info->outerMn[1];
	mn = { lo[0] - kProbeMargin, groundY, lo[2] - kProbeMargin };
	mx = { hi[0] + kProbeMargin, hi[1] + kProbeMargin, hi[2] + kProbeMargin };
	return true;
}

// The camera inside a building's interior bounds (BInfo, the mask hulls): the same bounds probeParts sizes the grid with.
bool WorldBuildings::camInBuilding(uint32_t bi, const double cam[3])
{
	const BInfo* info = infoFor(bi);
	if (!info || !info->hasBounds) { return false; }
	double l[3];
	toLocal(data_.instances[bi], cam, l);
	for (int k = 0; k < 3; k++) { if (l[k] < info->mn[k] || l[k] > info->mx[k]) { return false; } }
	return true;
}

// The volumes for render.interior_light (lightvol.hpp): the nearest buildings with interiors within `radius` of the camera whose bake has finished, nearest first, at most max.
// render.sun_bounce (sunOn): each volume also carries the sun transfer of its type (sunNew, and sunOld while the new bake fades in), and the buildings that hold the
// camera or a player character start a sun (re-)bake, one at a time, nearest first.
bool WorldBuildings::lightVolumes(const double cam[3], double radius, size_t max, std::vector<lightvol::Volume>& out, bool sunOn, const float* sunDir, const float* groundAlbedo)
{
	out.clear();
	if (data_.instances.empty()) { return false; }
	if (volScan_++ % 30 == 0)
	{
		std::vector<std::pair<double, uint32_t>> near;
		const double r2 = radius * radius;
		for (size_t i = 0; i < data_.instances.size(); i++)
		{
			const BuildingInstance& b = data_.instances[i];
			if (b.parent >= 0 || b.feature || b.prop || b.maxFloor == 0) { continue; }
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < r2) { near.push_back({ d, (uint32_t)i }); }
		}
		std::sort(near.begin(), near.end());
		volNear_.clear();
		for (size_t i = 0; i < near.size() && i < 3 * max; i++) { volNear_.push_back(near[i].second); }	// a few spare: a bake not finished yet is skipped
	}
	const bool sun = sunOn && sunDir && sunDir[1] > 0.0f && groundAlbedo;	// below the horizon the term is zero and nothing is baked
	// One sun bake at a time, for the nearest building that holds the camera or a player character (the task's scope: those are the rooms anyone sees).
	if (sun)
	{
		for (const uint32_t bi : volNear_)
		{
			if (bi >= data_.instances.size()) { continue; }
			bool held = camInBuilding(bi, cam);
			for (size_t k = 0; !held && k < occupants.size(); k++)
			{
				const int inst = occupants[k].instance >= 0 ? occupants[k].instance : floorableOf(occupants[k].building);
				held = inst == (int)bi;
			}
			if (!held) { continue; }
			std::shared_ptr<const lightbake::Grid> cur, old;
			sunFor(bi, sunDir, groundAlbedo, cur, old);
			if (sunBusy_) { break; }	// one at a time: the next candidate waits
		}
	}
	for (const uint32_t bi : volNear_)
	{
		if (bi >= data_.instances.size()) { continue; }
		const BuildingInstance& b = data_.instances[bi];
		std::shared_ptr<const lightbake::Grid> g = gridFor(bi);
		if (!g) { continue; }
		lightvol::Volume v;
		v.key = bi;
		v.grid = std::move(g);
		if (sun)
		{
			std::lock_guard<std::mutex> lk(sunMu_);
			const auto it = sun_.find(b.nameId);
			if (it != sun_.end()) { v.sunNew = it->second.cur; v.sunOld = it->second.old; }
		}
		v.origin[0] = b.x; v.origin[1] = b.y; v.origin[2] = b.z;
		v.q[0] = b.qw; v.q[1] = b.qx; v.q[2] = b.qy; v.q[3] = b.qz;
		for (int k = 0; k < 3; k++) { v.scale[k] = (float)(b.scale * b.stretch[k]); }
		if (lampOcclusion)	// the lamp visibility the worker published (kickLamps starts the jobs on the logic side; lampReg_ is never read here)
		{
			std::lock_guard<std::mutex> lk(probeMu_);
			const auto it = lampVis_.find(bi);
			if (it != lampVis_.end()) { v.lamps = it->second.data; v.lampCount = it->second.count; }
		}
		if (sliced((int)bi))	// the same height aboveSlab hides parts at: the air above it belongs to the floors the cut hides (kenshi-jj3.26.11)
		{
			const int s = shownOf(bi);
			const BInfo* info = s < std::min<int>(b.maxFloor, 3) ? infoFor(bi) : nullptr;
			if (info && info->hasBounds) { v.cutY = info->floorY[s + 1] - 4; }
		}
		out.push_back(std::move(v));
		if (out.size() >= max) { break; }
	}
	return !out.empty();
}

// Logic side (end of update()): the nearest building whose lamp list is newer than its published visibility gets a trace job (one at a time). The grid is the baked one
// (lightVolumes starts that bake); a building without it yet waits.
void WorldBuildings::kickLamps(const double cam[3])
{
	if (occBusy_) { return; }
	uint32_t best = 0xffffffffu;
	double bestD = 1e300;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		for (const auto& e : lampReg_)
		{
			const auto v = lampVis_.find(e.first);
			if (v != lampVis_.end() && v->second.version == e.second.version) { continue; }
			const BuildingInstance& b = data_.instances[e.first];
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < bestD) { bestD = d; best = e.first; }
		}
	}
	if (best == 0xffffffffu) { return; }
	const BuildingInstance& b = data_.instances[best];
	std::shared_ptr<const lightbake::Grid> grid;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		const auto it = probes_.find(b.nameId);
		if (it != probes_.end() && it->second.openWindows == kOpenWindows.get()) { grid = it->second.grid; }
	}
	if (!grid) { return; }
	std::vector<ProbePart> parts = probePartsFor(best);
	if (parts.empty()) { return; }
	const LampReg& reg = lampReg_[best];
	if (occThread_.joinable()) { occThread_.join(); }
	occBusy_ = true;
	occThread_ = std::thread([this, owner = best, id = b.nameId, parts = std::move(parts), grid = std::move(grid), lamps = reg.local, ver = reg.version]() mutable { lampJob(owner, id, std::move(parts), std::move(grid), std::move(lamps), ver); });
}

// The worker thread of the lamp occlusion: the type's occluder (kept for the last few types), then one ray per probe per lamp of the placement.
void WorldBuildings::lampJob(uint32_t owner, uint32_t nameId, std::vector<ProbePart> parts, std::shared_ptr<const lightbake::Grid> grid, std::vector<std::array<float, 3>> lamps, int version)
{
	std::shared_ptr<lightbake::Occluder> occ;
	const auto it = lampOccluders_.find(nameId);
	if (it != lampOccluders_.end()) { occ = it->second; }
	else
	{
		std::vector<lightbake::Tri> tris;
		tris = buildTris(parts, kOpenWindows.get());
		if (!tris.empty())
		{
			occ = std::make_shared<lightbake::Occluder>(std::move(tris));
			if (lampOccluders_.size() >= 4) { lampOccluders_.clear(); }
			lampOccluders_[nameId] = occ;
		}
	}
	LampVis out;
	out.version = version;
	out.count = (int)lamps.size();
	if (occ) { out.data = std::make_shared<const std::vector<uint8_t>>(occ->lampVisibility(*grid, lamps)); }
	if (out.data)
	{
		size_t seen[lightbake::kMaxLamps] = {};
		for (size_t i = 0; i < out.data->size(); i++) { seen[i % lightbake::kMaxLamps] += (*out.data)[i] > 127; }
		std::string per;
		for (size_t k = 0; k < lamps.size(); k++) { per += " " + std::to_string((int)(100.0 * (double)seen[k] / (double)std::max<size_t>(1, grid->usableCount()))) + "%"; }
		fprintf(stderr, "lamp_occlusion: %s: %zu lamps, share of usable probes that see each:%s\n", data_.names[nameId].c_str(), lamps.size(), per.c_str());
	}
	std::lock_guard<std::mutex> lk(probeMu_);
	lampVis_[owner] = std::move(out);
	occBusy_ = false;
}

// steady-clock seconds at the first call (the log's t= stamps)
static double sunClock0() { static const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); return t0; }

// The sun transfer of building bi's type for `sunDir` (world): copies the latest bake out; when it is missing, failed or more than kSunStep degrees off, starts a new
// one on sunThread_ (unless one runs). Returns false when there is nothing (yet).
bool WorldBuildings::sunFor(uint32_t bi, const float sunDir[3], const float groundAlbedo[3], std::shared_ptr<const lightbake::Grid>& cur, std::shared_ptr<const lightbake::Grid>& old)
{
	sunClock0();
	const BuildingInstance& b = data_.instances[bi];
	const float len = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
	if (len < 1e-6f) { return false; }
	const float wd[3] = { sunDir[0] / len, sunDir[1] / len, sunDir[2] / len };
	bool start = false;
	std::string trigger = "first bake";
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		const auto it = sun_.find(b.nameId);
		if (it != sun_.end())
		{
			cur = it->second.cur;
			old = it->second.old;
			if (it->second.pending || it->second.failed) { return cur != nullptr; }
			const float c = std::clamp(it->second.dir[0] * wd[0] + it->second.dir[1] * wd[1] + it->second.dir[2] * wd[2], -1.0f, 1.0f);
			const float deg = std::acos(c) * 180.0f / 3.14159265f;
			const bool stepped = std::acos(c) > kSunStep, windows = it->second.openWindows != kOpenWindows.get();
			start = !cur || stepped || windows;	// a render.open_windows change re-bakes
			if (start && cur && !windows)
			{
				// duty cycle: a sun-step bake waits until 2x the last bake's duration has passed since it finished (at most a third of the time on 4 threads)
				const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
				const double wait = 2.0 * sunLastSeconds_.load() - (now - sunFinishedAt_.load());
				if (wait > 0)
				{
					if (!sunDeferLogged_) { sunDeferLogged_ = true; fprintf(stderr, "kclient: sun_bake %s: deferred %.1f s (duty cycle) [t=%.1f]\n", data_.names[b.nameId].c_str(), wait, now - sunClock0()); }
					return cur != nullptr;
				}
			}
			char tb[120];
			snprintf(tb, sizeof tb, "%s, sun moved %.2f deg since the last bake (step %.0f)", !cur ? "no bake yet (previous failed or empty)" : stepped ? "sun step" : "open_windows changed", deg, (double)kSunStep * 180.0 / 3.14159265);
			trigger = tb;
		}
		else { start = true; }
	}
	if (!start || sunBusy_ || probeBusy_) { return cur != nullptr; }	// one bake at a time, with the sky bakes: retried next call
	std::vector<ProbePart> parts;
	std::array<float, 3> mn, mx;
	float groundY = 0;
	if (!probeParts(bi, parts, mn, mx, groundY)) { return cur != nullptr; }	// meshes still loading: retried next call
	// the world sun in the building's local frame (the bake traces the local meshes)
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double wdd[3] = { wd[0], wd[1], wd[2] };
	double local[3];
	rotate(conj, wdd, local);
	const float ld[3] = { (float)local[0], (float)local[1], (float)local[2] };
	float ground[3];
	for (int k = 0; k < 3; k++) { ground[k] = groundAlbedo[k]; }
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		sun_[b.nameId].pending = true;
	}
	if (sunThread_.joinable()) { sunThread_.join(); }
	sunBusy_ = true;
	sunDeferLogged_ = false;
	sunThread_ = std::thread([this, id = b.nameId, name = data_.names[b.nameId], parts = std::move(parts), mn, mx, groundY, wd, ld, ground, trigger]() mutable
	{
		bakeSun(id, name, std::move(parts), mn, mx, groundY, wd, ld, ground, trigger);
	});
	return cur != nullptr;
}

// The baking thread: traces the sun transfer of the type and publishes it (the build starts fading in). The world direction it was baked for is what the next call
// measures the sun's movement against.
void WorldBuildings::bakeSun(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY,
	const float worldDir[3], const float localDir[3], const float ground[3], std::string trigger)
{
	const bool openWindows = kOpenWindows.get();
	std::vector<lightbake::Tri> tris = buildTris(parts, openWindows);
	lightbake::Grid g;
	if (tris.empty()) { g.sunDir[0] = localDir[0]; g.sunDir[1] = localDir[1]; g.sunDir[2] = localDir[2]; }
	else { g = lightbake::bakeSun(std::move(tris), mn.data(), mx.data(), kProbeSpacing, groundY, ground, localDir, 4); }
	if (lightbake::cancelled())
	{
		{ std::lock_guard<std::mutex> lk(sunMu_); sun_[nameId].pending = false; }	// nothing published
		sunBusy_ = false;
		fprintf(stderr, "kclient: sun_bake %s: cancelled (quit) after %.2f s, nothing published (trigger: %s)\n", name.c_str(), g.sunSeconds, trigger.c_str());
		return;
	}
	const bool hasSun = g.sunValid();	// before the move below empties g
	const double seconds = g.sunSeconds;
	const size_t vpls = g.sunVpls;
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		SunEntry& e = sun_[nameId];
		if (e.cur) { e.old = e.cur; }	// the bake this one replaces fades out over the new one
		e.cur = std::make_shared<const lightbake::Grid>(std::move(g));
		for (int k = 0; k < 3; k++) { e.dir[k] = worldDir[k]; e.ground[k] = ground[k]; }
		e.pending = false;
		e.failed = false;
		e.openWindows = openWindows;
	}
	sunLastSeconds_ = seconds;
	sunFinishedAt_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	sunBakes_++;
	sunBusy_ = false;
	fprintf(stderr, "kclient: sun_bake %s: dir local %.2f %.2f %.2f, ground %.2f %.2f %.2f, %zu vpls, %.2f s on 4 threads%s; trigger: %s [finished t=%.1f]\n", name.c_str(),
		localDir[0], localDir[1], localDir[2], ground[0], ground[1], ground[2], vpls, seconds, hasSun ? "" : " (sun below the horizon: empty)", trigger.c_str(), sunFinishedAt_.load() - sunClock0());
}

bool WorldBuildings::lightProbes(const double cam[3], double radius, std::vector<ProbePoint>& out)
{
	out.clear();
	if (data_.instances.empty()) { return false; }
	if (probeScan_++ % 30 == 0 || probeTarget_ < 0)
	{
		probeTarget_ = -1;
		double best = (interiorRadius + kInteriorSlack) * (interiorRadius + kInteriorSlack);
		for (size_t i = 0; i < data_.instances.size(); i++)
		{
			const BuildingInstance& b = data_.instances[i];
			if (b.parent >= 0 || b.feature || b.prop || b.maxFloor == 0) { continue; }
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < best) { best = d; probeTarget_ = (int)i; }
		}
	}
	if (probeTarget_ < 0) { return false; }
	const uint32_t bi = (uint32_t)probeTarget_;
	const BuildingInstance& b = data_.instances[bi];
	std::shared_ptr<const lightbake::Grid> grid;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		probeAsked_ = b.nameId;
	}
	grid = gridFor(bi);
	if (!grid) { return false; }
	probeAt_[0] = b.x; probeAt_[1] = b.y; probeAt_[2] = b.z;
	probeDist_ = std::sqrt((b.x - cam[0]) * (b.x - cam[0]) + (b.y - cam[1]) * (b.y - cam[1]) + (b.z - cam[2]) * (b.z - cam[2]));
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	const double r2 = radius * radius;
	for (size_t i = 0; i < grid->count(); i++)
	{
		float local[3];
		grid->position(i, local);
		const double lc[3] = { (double)local[0] * b.scale * b.stretch[0], (double)local[1] * b.scale * b.stretch[1], (double)local[2] * b.scale * b.stretch[2] };
		double rc[3];
		rotate(q, lc, rc);
		const double w[3] = { b.x + rc[0], b.y + rc[1], b.z + rc[2] };
		const double dx = w[0] - cam[0], dy = w[1] - cam[1], dz = w[2] - cam[2];
		if (dx * dx + dy * dy + dz * dz > r2) { continue; }
		out.push_back({ { w[0], w[1], w[2] }, grid->level(i), grid->usable(i) });
	}
	probeDrawn_ = out.size();
	return true;
}

std::string WorldBuildings::lightBakeSlice(int layer) const
{
	std::lock_guard<std::mutex> lk(probeMu_);
	const auto it = probes_.find(probeAsked_);
	if (it == probes_.end() || !it->second.grid) { return "light_slice: no baked building"; }
	const lightbake::Grid& g = *it->second.grid;
	if (layer < 0 || layer >= g.dim[1]) { return "light_slice: layer 0.." + std::to_string(g.dim[1] - 1); }
	std::string out = "light_slice: " + data_.names[probeAsked_] + " layer " + std::to_string(layer) + " (y " + std::to_string((int)(g.min[1] + g.spacing * (float)layer)) + " above the building origin's frame), x across, z down\n";
	for (int z = 0; z < g.dim[2]; z++)
	{
		for (int x = 0; x < g.dim[0]; x++)
		{
			const size_t i = ((size_t)z * g.dim[1] + (size_t)layer) * g.dim[0] + (size_t)x;
			out += !g.usable(i) ? '#' : (char)('0' + std::clamp((int)(g.level(i) * 9.0f + 0.5f), 0, 9));
		}
		out += '\n';
	}
	return out;
}

// `dump open_windows [MAXLUMA]` (render.open_windows survey, kenshi-jj3.10.1): every building type with interiors placed in the world, its shell and
// interior parts' small detached islands, the UV rectangle each samples, its brightest sampled texel and the dark verdict. Slow and synchronous: it
// decodes the meshes and atlases again (a findings run, not a per-frame query). One line per island; a building's header has its dark count.
std::string WorldBuildings::openWindowsReport(float maxLuma)
{
	struct Albedo { std::vector<uint8_t> rgba; int w = 0, h = 0; bool tried = false; };
	std::unordered_map<std::string, MeshCpu> meshes;
	std::unordered_map<std::string, Albedo> albedos;
	char buf[512];
	std::string out;
	size_t types = 0, islands = 0, dark = 0;
	std::vector<char> seen(data_.names.size(), 0);
	for (const uint32_t ii : data_.floorable)
	{
		const BuildingInstance& b = data_.instances[ii];
		if (b.parent >= 0 || b.feature || b.prop || b.nameId >= seen.size() || seen[b.nameId]) { continue; }
		seen[b.nameId] = 1;
		types++;
		const std::string& name = data_.names[b.nameId];
		size_t bi = 0, bd = 0;
		std::string detail;
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const uint32_t pid = data_.partRefs[b.firstPart + k];
			const BuildingPartDef& def = data_.parts[pid];
			if (def.kind != 0 && def.kind != 1) { continue; }
			if (def.blackCap || def.marker || def.materialType != 0 || def.featureMode >= 0) { continue; }
			const std::string file = res_->locate(def.mesh);
			if (file.empty()) { continue; }
			auto mi = meshes.find(file);
			if (mi == meshes.end())
			{
				MeshCpu mesh;
				if (!decodeMesh(file, mesh)) { continue; }
				mi = meshes.emplace(file, std::move(mesh)).first;
			}
			const MeshCpu& mesh = mi->second;
			for (size_t s = 0; s < mesh.subs.size(); s++)
			{
				const Sub& sub = mesh.subs[s];
				if (sub.islands.empty()) { continue; }
				// no black default here: a missing diffuse is no albedo to the barrier verdict, while black.dds would read every detached island dark and open it
				const std::string texFile = textureFile(def, sub.material, nullptr, false);
				Albedo* alb = nullptr;
				if (!texFile.empty())
				{
					auto ai = albedos.find(texFile);
					if (ai == albedos.end())
					{
						Albedo a;
						a.tried = true;
						DdsImage dds;
						std::string err;
						if (dds_load(texFile, dds, err))
						{
							int level = 0;
							while (level + 1 < dds.mips && (std::max(dds.width, dds.height) >> level) > 128) { level++; }
							if (!dds_decode(dds, level, a.rgba, a.w, a.h)) { a.rgba.clear(); }
						}
						ai = albedos.emplace(texFile, std::move(a)).first;
					}
					alb = &ai->second;
				}
				const openwindows::Texel tex = { nullptr, alb && !alb->rgba.empty() ? alb->rgba.data() : nullptr, alb ? alb->w : 0, alb ? alb->h : 0 };
				const std::vector<uint32_t> barrierTris = openwindows::triangles(sub.islands, def.tileX, def.tileY, tex, maxLuma);	// what the switch leaves out
				for (const openwindows::Island& is : sub.islands)
				{
					float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f }, worst = 0;
					for (size_t i = 0; i + 1 < is.uv.size(); i += 2)
					{
						const float u = is.uv[i] * (def.tileX > 0 ? def.tileX : 1.0f), v = is.uv[i + 1] * (def.tileY > 0 ? def.tileY : 1.0f);
						mn[0] = std::min(mn[0], u); mx[0] = std::max(mx[0], u);
						mn[1] = std::min(mn[1], v); mx[1] = std::max(mx[1], v);
					}
					for (int gj = 0; gj < 5; gj++)	// the grid openwindows::dark samples
					{
						for (int gi = 0; gi < 5; gi++)
						{
							const float u = mn[0] + (mx[0] - mn[0]) * ((float)gi + 0.5f) / 5.0f, v = mn[1] + (mx[1] - mn[1]) * ((float)gj + 0.5f) / 5.0f;
							worst = std::max(worst, tex.at(u, v));
						}
					}
					const bool isDark = openwindows::dark(is, def.tileX, def.tileY, tex, maxLuma);
					const bool isBarrier = std::find_first_of(is.tris.begin(), is.tris.end(), barrierTris.begin(), barrierTris.end()) != is.tris.end();
					islands++;
					bi++;
					bd += isBarrier;
					out += "    ";
					out += isDark ? "DARK " : isBarrier ? "back " : "bright ";
					const size_t slash = file.find_last_of('/');
					snprintf(buf, sizeof buf, "%s sub%zu tri", (slash == std::string::npos ? file : file.substr(slash + 1)).c_str(), s);
					out += buf;
					for (const uint32_t t : is.tris) { snprintf(buf, sizeof buf, " %u", t); out += buf; }
					snprintf(buf, sizeof buf, " (kind %d floor %d) %.1fx%.1fx%.1f u uv %.3f..%.3f x %.3f..%.3f max texel %.2f", def.kind, def.floor,
						is.mx[0] - is.mn[0], is.mx[1] - is.mn[1], is.mx[2] - is.mn[2], mn[0], mx[0], mn[1], mx[1], worst);
					out += buf;
					out += "\n";
				}
			}
		}
		snprintf(buf, sizeof buf, "open_windows: %s: %zu islands, %zu barrier\n", name.c_str(), bi, bd);
		out += buf;
		dark += bd;
	}
	std::string head;
	snprintf(buf, sizeof buf, "open_windows: %zu building types with interiors (first placement each), %zu islands, %zu barrier at max texel %.2f\n", types, islands, dark, maxLuma);
	head += buf;
	return head + out;
}

std::string WorldBuildings::lightBakeLine() const
{
	std::lock_guard<std::mutex> lk(probeMu_);
	const auto it = probes_.find(probeAsked_);
	if (it == probes_.end()) { return "light_bake: no building asked about yet (debug.light_probes on, a building with interiors near the camera)"; }
	const std::string& name = data_.names[probeAsked_];
	if (it->second.failed) { return "light_bake: " + name + " failed: " + it->second.why; }
	const std::shared_ptr<const lightbake::Grid>& g = it->second.grid;
	if (!g) { return "light_bake: " + name + " pending"; }
	// the sky level of the usable probes: in the open (enclosure < 0.1) it should be about 1, indoors (> 0.5) lower, near an opening above 0
	double open = 0, in = 0;
	size_t nOpen = 0, nIn = 0, bright = 0;
	for (size_t i = 0; i < g->count(); i++)
	{
		if (!g->usable(i)) { continue; }
		if (g->enclosure(i) < 0.1f) { open += g->level(i); nOpen++; }
		else if (g->enclosure(i) > 0.5f) { in += g->level(i); nIn++; bright += g->level(i) > 0.15f; }
	}
	char tail[160];
	snprintf(tail, sizeof tail, "; mean sky level open %.2f (%zu probes), indoors %.3f (%zu probes, %zu above 0.15)", nOpen ? open / (double)nOpen : 0.0, nOpen, nIn ? in / (double)nIn : 0.0, nIn, bright);
	char buf[560];
	snprintf(buf, sizeof buf, "light_bake: %s grid %dx%dx%d spacing %.0f probes %zu usable %zu triangles %zu seconds %.2f %s; %zu probes within the draw radius, the camera %.0f from the building at %.0f %.0f %.0f", name.c_str(), g->dim[0], g->dim[1], g->dim[2], g->spacing,
		g->count(), g->usableCount(), g->triangles, g->seconds, g->fromCache ? "(from the cache)" : "(baked)", probeDrawn_, probeDist_, probeAt_[0], probeAt_[1], probeAt_[2]);
	std::string sun;
	{
		std::lock_guard<std::mutex> slk(sunMu_);
		const auto si = sun_.find(probeAsked_);
		if (si == sun_.end()) { sun = "sun_bake: none yet"; }
		else if (si->second.pending) { sun = "sun_bake: pending"; }
		else if (!si->second.cur) { sun = "sun_bake: none"; }
		else
		{
			const lightbake::Grid& s = *si->second.cur;
			char sb[220];
			snprintf(sb, sizeof sb, "sun_bake: world dir %.2f %.2f %.2f, %zu probes, %zu vpls, %.2f s on 4 threads%s, %zu bakes since start", si->second.dir[0], si->second.dir[1], si->second.dir[2],
				s.count(), s.sunVpls, s.sunSeconds, s.sunValid() ? "" : " (below the horizon)", (size_t)sunBakes_);
			sun = sb;
		}
	}
	return std::string(buf) + tail + "; " + sun;
}

// The worker thread of one type's bake: the cache by the content hash of its mesh files, else decode every part, bake, save.
void WorldBuildings::bakeProbes(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY)
{
	const bool openWindows = kOpenWindows.get();	// render.open_windows: the barrier quads are left out of the bake (a separate cache entry)
	auto publish = [&](lightbake::Grid&& g, const std::string& why)
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		ProbeEntry& e = probes_[nameId];
		e.openWindows = openWindows;
		if (why.empty()) { e.grid = std::make_shared<const lightbake::Grid>(std::move(g)); } else { e.failed = true; e.why = why; }
		probeBusy_ = false;
	};
	uint64_t key = lightbake::hashBytes(lightbake::kHashSeed, &kProbeFormat, sizeof kProbeFormat);
	key = lightbake::hashBytes(key, &kProbeSpacing, sizeof kProbeSpacing);
	key = lightbake::hashBytes(key, &openWindows, sizeof openWindows);
	key = lightbake::hashBytes(key, mn.data(), sizeof(float) * 3);
	key = lightbake::hashBytes(key, mx.data(), sizeof(float) * 3);
	for (const ProbePart& p : parts)
	{
		std::ifstream f(p.mesh, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		key = lightbake::hashBytes(key, bytes.data(), bytes.size());
		key = lightbake::hashBytes(key, p.off, sizeof p.off);
		key = lightbake::hashBytes(key, p.def.texture.data(), p.def.texture.size());
		key = lightbake::hashBytes(key, &p.def.tileX, sizeof(float) * 2);
	}
	const std::string path = lightbake::cachePath(name, key);
	lightbake::Grid g;
	if (lightbake::load(path, g)) { publish(std::move(g), ""); return; }

	std::vector<lightbake::Tri> tris = buildTris(parts, openWindows);
	if (tris.empty()) { publish({}, "no triangles"); return; }
	g = lightbake::bake(std::move(tris), mn.data(), mx.data(), kProbeSpacing, groundY, kProbeGroundAlbedo, 4);
	if (lightbake::cancelled()) { fprintf(stderr, "kclient: light_bake %s: cancelled (quit), nothing published or cached\n", name.c_str()); probeBusy_ = false; return; }
	lightbake::save(path, g);
	publish(std::move(g), "");
}

// The bake's triangles of one building type: position, face normal (facing the vertex normals' way), the diffuse texture's luma (the sky transfer) and RGB
// (the sun transfer's tint).
std::vector<lightbake::Tri> WorldBuildings::buildTris(const std::vector<ProbePart>& parts, bool openWindows) const
{
	std::vector<lightbake::Tri> tris;
	struct Albedo { std::vector<uint8_t> rgba; int w = 0, h = 0; };
	std::unordered_map<std::string, Albedo> albedo;
	for (const ProbePart& p : parts)
	{
		MeshCpu mesh;
		if (!decodeMesh(p.mesh, mesh)) { continue; }
		for (const Sub& s : mesh.subs)
		{
			const std::string file = textureFile(p.def, s.material);
			Albedo noAlbedo, *alb = &noAlbedo;
			if (!file.empty())
			{
				auto [it, fresh] = albedo.emplace(file, Albedo());
				if (fresh)
				{
					DdsImage dds;
					std::string err;
					if (dds_load(file, dds, err))
					{
						int level = 0;
						while (level + 1 < dds.mips && (std::max(dds.width, dds.height) >> level) > 128) { level++; }
						if (!dds_decode(dds, level, it->second.rgba, it->second.w, it->second.h)) { it->second.rgba.clear(); }
					}
				}
				alb = &it->second;
			}
			// render.open_windows: the submesh's barrier quads are left out of the bake (the same islands and dark test the draw uses)
			std::vector<char> drop((size_t)s.indexCount / 3, 0);
			if (openWindows && !alb->rgba.empty())
			{
				const openwindows::Texel tex = { nullptr, alb->rgba.data(), alb->w, alb->h };
				for (const uint32_t tri : openwindows::triangles(s.islands, p.def.tileX, p.def.tileY, tex)) { if (tri < drop.size()) { drop[tri] = 1; } }
			}
			for (int i = 0; i + 2 < s.indexCount; i += 3)
			{
				if (drop[(size_t)i / 3]) { continue; }
				lightbake::Tri t = {};
				float vn[3] = {}, uv[2] = {};
				for (int c = 0; c < 3; c++)
				{
					const float* v = &mesh.verts[((size_t)s.firstVertex + mesh.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
					for (int k = 0; k < 3; k++) { t.p[c][k] = v[k] + p.off[k]; vn[k] += v[3 + k]; }
					uv[0] += v[6] / 3;
					uv[1] += v[7] / 3;
				}
				const float u[3] = { t.p[1][0] - t.p[0][0], t.p[1][1] - t.p[0][1], t.p[1][2] - t.p[0][2] }, w[3] = { t.p[2][0] - t.p[0][0], t.p[2][1] - t.p[0][1], t.p[2][2] - t.p[0][2] };
				const float n[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len < 1e-9f) { continue; }
				const float flip = n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] < 0 ? -1.0f : 1.0f;	// the winding cannot be trusted: face the way the vertex normals do
				for (int k = 0; k < 3; k++) { t.n[k] = n[k] / len * flip; }
				const float tu = uv[0] * (p.def.tileX > 0 ? p.def.tileX : 1.0f), tv = uv[1] * (p.def.tileY > 0 ? p.def.tileY : 1.0f);
				t.albedo = lumaAt(alb->rgba, alb->w, alb->h, tu, tv);
				if (const uint8_t* px = texelAt(alb->rgba, alb->w, alb->h, tu, tv)) { for (int k = 0; k < 3; k++) { t.alb[k] = px[k] / 255.0f; } }
				else { for (int k = 0; k < 3; k++) { t.alb[k] = kProbeGroundAlbedo; } }
				tris.push_back(t);
			}
		}
	}
	return tris;
}
