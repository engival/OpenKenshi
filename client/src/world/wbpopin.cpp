// Building pop-in measurement (kenshi-k16.2.258.2): the timelines and asset costs behind the report on "the buildings pop in" (the user, on
// the BDC fly launch). Dev only: the collector is the PopIn member in worldbuildings.hpp, filled in wbdraw.cpp's update() (the load cell pass
// and the first drawn frame) and wbload.cpp (each decode's read + parse on a worker, its drawn-triangle BVH, each GPU create in the render
// queue). `dump popin` prints it; nothing here draws or changes a frame.
//
// The numbers answer three questions: how long a building takes from entering the load radius to being first drawn and to being fully
// textured; what that delay is made of (disk read vs CPU decode vs the render queue's create budgets); and what a set of common buildings
// would cost to precache (bytes, decode seconds, create seconds), from this build's own measurements scaled by file size.
#include "world/worldbuildings.hpp"
#include "world/worldbuildings_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KCLIENT_DEV_TOOLS

using namespace wbi;

namespace
{
// The percentiles a timeline is summarised by: p50 / p90 / max of one column over a row set.
struct Pct { double p50 = 0, p90 = 0, max = 0; size_t n = 0; };

Pct percentiles(std::vector<double> v)
{
	Pct p;
	p.n = v.size();
	if (v.empty()) { return p; }
	std::sort(v.begin(), v.end());
	p.p50 = v[v.size() / 2];
	p.p90 = v[std::min(v.size() - 1, v.size() * 9 / 10)];
	p.max = v.back();
	return p;
}

std::string bytes(size_t b)
{
	char buf[32];
	if (b >= (1u << 20)) { snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0); }
	else if (b >= 1024) { snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0); }
	else { snprintf(buf, sizeof buf, "%zu B", b); }
	return buf;
}

std::string ms(double s) { char buf[32]; snprintf(buf, sizeof buf, "%.1f ms", s * 1000.0); return buf; }

std::string secs(double s) { char buf[32]; snprintf(buf, sizeof buf, "%.2f s", s); return buf; }
}

void WorldBuildings::popinMark(uint32_t ii, const BuildingInstance& b)
{
	if (ii >= data_.instances.size() || b.partCount == 0) { return; }
	PopIn::Row r;
	r.name = b.nameId;
	r.x = b.x; r.z = b.z;
	const double dx = b.x - cam_[0], dy = b.y - cam_[1], dz = b.z - cam_[2];
	r.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	r.in = popin.now;
	r.parts = b.partCount;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const int kind = data_.parts[data_.partRefs[b.firstPart + k]].kind;
		if (kind == 0) { r.shells++; }
		else if (kind == 1) { r.interiors++; }
	}
	popin.rows.emplace(ii, r);
	popin.inRange++;
}

// Called for an instance the cull pass found visible this frame. The first such frame is its "first drawn"; once every shell part has its
// textures too (all of them, not only the visible ones) it is "complete" — the whole building lit, which is what `unlitAtDraw` counts the
// steps between.
void WorldBuildings::popinDrawn(uint32_t ii)
{
	const auto it = popin.rows.find(ii);
	if (it == popin.rows.end()) { return; }
	PopIn::Row& r = it->second;
	const BuildingInstance& b = data_.instances[ii];
	if (r.draw < 0)
	{
		r.draw = popin.now;
		popin.drawn++;
		uint32_t unlit = 0;
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const uint32_t pid = data_.partRefs[b.firstPart + k];
			if (data_.parts[pid].kind == 0 && !partTexturesReady(pid)) { unlit++; }
		}
		r.unlitAtDraw = unlit;
	}
	if (r.complete >= 0) { return; }
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		if (data_.parts[pid].kind == 0 && !partTexturesReady(pid)) { return; }
	}
	r.complete = popin.now;
	popin.full++;
}

std::string WorldBuildings::popinReport() const
{
	std::string out;
	char line[1024];
	auto add = [&out](const std::string& s) { out += s; out += '\n'; };

	const size_t curveRows = popin.curve.size();
	const double span = curveRows ? popin.curve.back().t : popin.now;
	snprintf(line, sizeof line, "popin: %s, cold disk %d, %zu curve rows over %.2f s (%.0f frames/s), world %zu static instances, %zu names",
		popin.streaming ? "streaming (a live run's budgets)" : "blocking (every decode waited for each frame)",
		(int)popin.cold, curveRows, span, span > 0 ? curveRows / span : 0.0, data_.instances.size(), data_.names.size());
	add(line);
	snprintf(line, sizeof line, "popin: load radius %d cells (%.0f u), far ring to %.0f u, interior radius %.0f u; %zu buildings entered range, %zu drawn, %zu complete",
		radius, radius * kCellSize, farDist_, interiorRadius, popin.inRange, popin.drawn, popin.full);
	add(line);

	// ---- the gate: what the load screen left for the player to see (kenshi-k16.2.258.3) ----
	if (popin.liftAt >= 0)
	{
		size_t beforeFull = 0, beforeDrawn = 0, after = 0, afterUnlit = 0;
		std::vector<double> late;
		for (const auto& [ii, r] : popin.rows)
		{
			if (r.draw >= 0 && r.draw <= popin.liftAt) { beforeDrawn++; }
			if (r.complete >= 0 && r.complete <= popin.liftAt) { beforeFull++; }
			if (r.draw > popin.liftAt) { after++; late.push_back(r.draw - popin.liftAt); if (r.unlitAtDraw) { afterUnlit++; } }
		}
		const Pct p = percentiles(late);
		snprintf(line, sizeof line, "gate: load screen lifted at %.2f s; by then %zu of %zu buildings in range were drawn and %zu complete; %zu first drawn after the lift (%zu of them with shell parts untextured), lift -> drawn p50 %.3f s p90 %.3f s max %.3f s",
			popin.liftAt, beforeDrawn, popin.rows.size(), beforeFull, after, afterUnlit, p.p50, p.p90, p.max);
		add(line);
	}

	// the loading stand-in (kenshi-k16.2.258.4): packs summed over instances. mixed = instance-packs that drew black and real parts together outside the switch: must be 0.
	{
		size_t stood = 0, switched = 0, rowMixed = 0;
		for (const auto& [ii, r] : popin.rows) { stood += r.black >= 0; switched += r.swtch >= 0; rowMixed += r.mixedFrames; }
		snprintf(line, sizeof line, "popin: building stand-in (render.loading_standin): %zu instances stood in black, %zu switched whole to the real look; instance-packs black %llu, in the switch %llu, MIXED (black + real parts together) %llu (per-instance sum %zu)",
			stood, switched, (unsigned long long)popin.standingInstanceFrames, (unsigned long long)popin.fadingInstanceFrames, (unsigned long long)popin.mixedInstanceFrames, rowMixed);
		add(line);
	}

	// ---- the curve: how much of the world was loaded / drawn, per event ----
	add("");
	add("curve: t(s) frame inRange drawn full meshesReady texturesReady pending");
	const size_t step = curveRows > 400 ? (curveRows + 399) / 400 : 1;
	for (size_t i = 0; i < curveRows; i++)
	{
		if (i % step != 0 && i + 1 != curveRows) { continue; }
		const PopIn::Curve& c = popin.curve[i];
		snprintf(line, sizeof line, "curve: %6.3f %6llu %6zu %6zu %6zu %6zu %6zu %6zu", c.t, (unsigned long long)c.f, c.inRange, c.drawn, c.full,
			c.meshesReady, c.texturesReady, c.pending);
		add(line);
	}

	// ---- per-building timelines ----
	// Every row in the near range a player sees (10 * the interior radius covers a town in view), sorted by when it entered.
	constexpr double kNearRow = 8000.0;
	std::vector<std::pair<uint32_t, const PopIn::Row*>> near, all;
	for (const auto& [ii, r] : popin.rows)
	{
		all.push_back({ ii, &r });
		if (r.dist <= kNearRow) { near.push_back({ ii, &r }); }
	}
	std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second->in < b.second->in; });
	std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.second->in < b.second->in; });
	snprintf(line, sizeof line, "timelines: %zu buildings within %.0f u at their in-range, of %zu in all", near.size(), kNearRow, all.size());
	add(line);
	add("building: name x z dist in(s) draw(s) complete(s) draw-in complete-in shells interiors unlitAtDraw");
	auto nameOf = [this](uint32_t n) { return n < data_.names.size() ? data_.names[n] : std::string("?"); };
	for (const auto& [ii, r] : near)
	{
		snprintf(line, sizeof line, "building: %s %.0f %.0f %.0f %.3f %.3f %.3f %.3f %.3f %u %u %u", nameOf(r->name).c_str(), r->x, r->z, r->dist, r->in,
			r->draw, r->complete, r->draw >= 0 ? r->draw - r->in : -1.0, r->complete >= 0 ? r->complete - r->in : -1.0, r->shells, r->interiors, r->unlitAtDraw);
		add(line);
	}
	// the same statistics over every row that was drawn
	std::vector<double> toDraw, toComplete, drawnOnly;
	for (const auto& [ii, r] : all)
	{
		if (r->draw < 0) { continue; }
		toDraw.push_back(r->draw - r->in);
		drawnOnly.push_back(r->in);
		if (r->complete >= 0) { toComplete.push_back(r->complete - r->in); }
	}
	const Pct pd = percentiles(toDraw), pc = percentiles(toComplete);
	snprintf(line, sizeof line, "timelines: in-range -> drawn p50 %.3f s p90 %.3f s max %.3f s (%zu buildings); -> complete p50 %.3f s p90 %.3f s max %.3f s (%zu)",
		pd.p50, pd.p90, pd.max, pd.n, pc.p50, pc.p90, pc.max, pc.n);
	add(line);
	std::vector<double> bands[4];
	for (const auto& [ii, r] : all)
	{
		if (r->draw < 0) { continue; }
		const int b = r->dist < 2500 ? 0 : r->dist < 13824 ? 1 : r->dist < 60000 ? 2 : 3;
		bands[b].push_back(r->draw - r->in);
	}
	for (int b = 0; b < 4; b++)
	{
		const Pct p = percentiles(bands[b]);
		snprintf(line, sizeof line, "timelines: band %s: %zu drawn, in-range -> drawn p50 %.3f s p90 %.3f s max %.3f s", b == 0 ? "< 2500 u" : b == 1 ? "2500-13824 u" : b == 2 ? "13824-60000 u" : "> 60000 u",
			p.n, p.p50, p.p90, p.max);
		add(line);
	}

	// ---- assets: what one mesh / texture cost, and where ----
	size_t meshFiles = 0, meshFileB = 0, meshCpuB = 0;
	double meshDecode = 0, meshPick = 0, meshCreate = 0, meshDecodeMax = 0;
	{
		std::vector<std::pair<double, std::string>> byDecode, byCreate;
		for (const auto& [path, m] : popin.meshes)
		{
			meshFiles++; meshFileB += m.fileBytes; meshCpuB += m.cpuBytes;
			meshDecode += m.decode; meshPick += m.pick; meshCreate += m.create;
			meshDecodeMax = std::max(meshDecodeMax, m.decode);
			byDecode.push_back({ m.decode, path });
			byCreate.push_back({ m.create, path });
		}
		auto top = [&](std::vector<std::pair<double, std::string>>& v, const char* what, size_t n) {
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (size_t i = 0; i < v.size() && i < n; i++)
			{
				const auto m = popin.meshes.find(v[i].second);
				const size_t fileB = m != popin.meshes.end() ? m->second.fileBytes : 0;
				snprintf(line, sizeof line, "mesh: slowest %s %.1f ms %s (%s on disk)", what, v[i].first * 1000.0, std::filesystem::path(v[i].second).filename().c_str(), bytes(fileB).c_str());
				add(line);
			}
		};
		snprintf(line, sizeof line, "meshes: %zu read+parsed: %s on disk, %s decoded CPU, decode %.2f s total (mean %.1f ms, max %.1f ms), pick BVH %.2f s, GPU create %.2f s",
			meshFiles, bytes(meshFileB).c_str(), bytes(meshCpuB).c_str(), meshDecode, meshFiles ? meshDecode / meshFiles * 1000 : 0.0, meshDecodeMax * 1000, meshPick, meshCreate);
		add(line);
		snprintf(line, sizeof line, "meshes: sums over every decode and create in this run, so a mesh read once is counted once; %zu mesh entries are held (a Ready mesh stays in VRAM)", meshes_.size());
		add(line);
		top(byDecode, "decode", 6);
		top(byCreate, "create", 6);
	}
	size_t texFiles = 0, texFileB = 0, texGpuB = 0;
	double texDecode = 0, texCreate = 0;
	{
		std::vector<std::pair<double, std::string>> byDecode, byCreate, bySize;
		for (const auto& [path, t] : popin.textures)
		{
			texFiles++; texFileB += t.fileBytes; texGpuB += t.gpuBytes;
			texDecode += t.decode; texCreate += t.create;
			byDecode.push_back({ t.decode, path });
			byCreate.push_back({ t.create, path });
			bySize.push_back({ (double)t.gpuBytes, path });
		}
		auto top = [&](std::vector<std::pair<double, std::string>>& v, const char* what, size_t n, bool asMs) {
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (size_t i = 0; i < v.size() && i < n; i++)
			{
				const auto& t = popin.textures.at(v[i].second);
				if (asMs) { snprintf(line, sizeof line, "texture: slowest %s %.1f ms %s (%dx%d mips %d, %s GPU)", what, v[i].first * 1000.0, std::filesystem::path(v[i].second).filename().c_str(), t.w, t.h, t.mips, bytes(t.gpuBytes).c_str()); }
				else { snprintf(line, sizeof line, "texture: largest %s %s %dx%d mips %d %s GPU (%s read %.1f ms, create %.1f ms)", what, std::filesystem::path(v[i].second).filename().c_str(), t.w, t.h, t.mips,
					bytes(t.gpuBytes).c_str(), bytes(t.fileBytes).c_str(), t.decode * 1000.0, t.create * 1000.0); }
				add(line);
			}
		};
		snprintf(line, sizeof line, "textures: %zu read (BC data kept as is): %s on disk = %s GPU, read %.2f s total (mean %.1f ms), GPU create %.2f s total (mean %.1f ms)",
			texFiles, bytes(texFileB).c_str(), bytes(texGpuB).c_str(), texDecode, texFiles ? texDecode / texFiles * 1000 : 0.0, texCreate, texFiles ? texCreate / texFiles * 1000 : 0.0);
		add(line);
		top(byDecode, "read", 6, true);
		top(byCreate, "create", 6, true);
		top(bySize, "by size", 8, false);
	}

	// ---- per-name cost, and the precache sets ----
	// The distinct meshes and textures each BUILDING name places, by file. A texture is a part's MATERIAL_SPEC `texture map` and its `normal`
	// (the pair resolvePart loads for every submesh; the DUAL extras and the submesh script textures are not resolved here: the 298.1 wait
	// also holds for those, so a real set can be a little larger).
	struct NameRow { size_t count = 0, shells = 0; std::unordered_set<std::string> meshes, texs; };
	std::unordered_map<uint32_t, NameRow> byName;
	std::unordered_map<std::string, size_t> meshBytes, texBytes;	// file path -> bytes (the same file can be in several names)
	// Over the WHOLE world data (every placed building in every zone, loaded from the world layout whatever the camera's range): the question
	// is what a precache of the common buildings would cost everywhere, not in the one region this run streamed.
	for (uint32_t ii = 0; ii < data_.instances.size(); ii++)
	{
		const BuildingInstance& b = data_.instances[ii];
		if (b.parent >= 0 || b.feature || b.prop || b.partCount == 0) { continue; }
		NameRow& n = byName[b.nameId];
		n.count++;
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const BuildingPartDef& def = data_.parts[data_.partRefs[b.firstPart + k]];
			if (def.kind != 0) { continue; }
			n.shells++;
			if (!def.mesh.empty())
			{
				const std::string f = res_->locate(def.mesh);
				if (!f.empty())
				{
					if (n.meshes.insert(f).second && meshBytes.find(f) == meshBytes.end())
					{
						std::error_code ec;
						const auto sz = std::filesystem::file_size(f, ec);
						meshBytes[f] = ec ? 0 : (size_t)sz;
					}
				}
			}
			for (const std::string& rec : { def.texture, def.normal, def.texture2, def.normal2 })
			{
				if (rec.empty()) { continue; }
				const std::string f = recordTexture(rec, true);
				if (f.empty()) { continue; }
				if (n.texs.insert(f).second && texBytes.find(f) == texBytes.end())
				{
					std::error_code ec;
					const auto sz = std::filesystem::file_size(f, ec);
					texBytes[f] = ec ? 0 : (size_t)sz;
				}
			}
		}
	}
	std::vector<std::pair<uint32_t, NameRow*>> order;
	for (auto& [name, n] : byName) { order.push_back({ name, &n }); }
	std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second->count != b.second->count ? a.second->count > b.second->count : a.first < b.first; });
	size_t totalCount = 0;
	for (const auto& [name, n] : order) { totalCount += n->count; }
	add("");
	snprintf(line, sizeof line, "names: %zu names placed in the world data, %zu building instances; a name's set is its distinct shell meshes and MATERIAL_SPEC textures", order.size(), totalCount);
	add(line);
	add("name: name count cum% shells meshes meshMB textures texMB totalMB");
	double cum = 0;
	for (const auto& [name, n] : order)
	{
		size_t mb = 0, tb = 0;
		for (const auto& f : n->meshes) { mb += meshBytes[f]; }
		for (const auto& f : n->texs) { tb += texBytes[f]; }
		cum += n->count;
		snprintf(line, sizeof line, "name: %s %zu %.1f%% %zu %zu %.1f %zu %.1f %.1f", nameOf(name).c_str(), n->count, totalCount ? 100.0 * cum / totalCount : 0.0, n->shells,
			n->meshes.size(), mb / 1048576.0, n->texs.size(), tb / 1048576.0, (mb + tb) / 1048576.0);
		add(line);
	}

	// The precache estimate: a union of the top N names' files. Decode and create are this run's measured per-byte rates where a file was
	// measured; for a file this run never read, the run's mean rate over the files it did read (a lower rate when the disk was cold).
	const double decodeRate = meshFileB + texFileB > 0 ? (meshDecode + texDecode) / (double)(meshFileB + texFileB) : 0;	// s per byte
	const double createRate = texGpuB > 0 ? texCreate / (double)texGpuB : 0;	// s per GPU byte (textures dominate)
	for (const size_t N : { size_t(10), size_t(25), size_t(50), size_t(100), order.size() })
	{
		std::unordered_set<std::string> unionM, unionT;
		size_t inst = 0;
		for (size_t i = 0; i < std::min(N, order.size()); i++)
		{
			inst += order[i].second->count;
			unionM.insert(order[i].second->meshes.begin(), order[i].second->meshes.end());
			unionT.insert(order[i].second->texs.begin(), order[i].second->texs.end());
		}
		size_t mb = 0, tb = 0, measuredB = 0;
		double decodeS = 0, createS = 0;
		for (const auto& f : unionM)
		{
			const size_t b = meshBytes[f];
			mb += b;
			if (const auto m = popin.meshes.find(f); m != popin.meshes.end()) { decodeS += m->second.decode; createS += m->second.create; measuredB += b; }
			else { decodeS += b * decodeRate; createS += b * createRate; }
		}
		for (const auto& f : unionT)
		{
			const size_t b = texBytes[f];
			tb += b;
			if (const auto t = popin.textures.find(f); t != popin.textures.end()) { decodeS += t->second.decode; createS += t->second.create; measuredB += b; }
			else { decodeS += b * decodeRate; createS += b * createRate; }
		}
		snprintf(line, sizeof line, "precache: top %zu names = %zu buildings (%.1f%% of the world's), %zu meshes %s + %zu textures %s = %s (VRAM the same as the BC files); decode %s, create %s (%.0f%% of the bytes measured, the rest at this run's mean rate)",
			N, inst, totalCount ? 100.0 * inst / totalCount : 0.0, unionM.size(), bytes(mb).c_str(), unionT.size(), bytes(tb).c_str(), bytes(mb + tb).c_str(),
			secs(decodeS).c_str(), secs(createS).c_str(), mb + tb > 0 ? 100.0 * measuredB / (mb + tb) : 0.0);
		add(line);
	}
	snprintf(line, sizeof line, "precache: measured rates: read+parse %.1f MB/s (cold %d), GPU create %.1f MB/s of texture", decodeRate > 0 ? 1.0 / decodeRate / 1048576.0 : 0.0,
		(int)popin.cold, createRate > 0 ? 1.0 / createRate / 1048576.0 : 0.0);
	add(line);
	return out;
}

#endif
