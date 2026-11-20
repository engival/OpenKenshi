#include "world/foliage.hpp"

#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>

#include "app/config.hpp"
#include "render/framestats.hpp"
#include "render/renderqueue.hpp"
#include "kworld/coords.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"

namespace
{

#ifdef KCLIENT_DEV_TOOLS
const cfg::Bool kNoDiffuseGate{ cfg::Group{ "Scripted runs" }, "debug.foliage_nogate", false, "Dev: foliage parts draw on the grey fallback while their diffuse streams (the before of kenshi-k16.2.298.2; for measuring only)" };
inline bool diffuseGateOff() { return kNoDiffuseGate.get(); }
#else
inline bool diffuseGateOff() { return false; }
#endif

constexpr int kInstanceFloats = 12;	// must match WorldBuildings: position + scale, quaternion xyzw, per-axis stretch
constexpr int kInstanceBytes = kInstanceFloats * 4;
constexpr int kGenThreads = 3;
constexpr size_t kQueueAhead = 6;		// cells requested at once while streaming (nearest first)
constexpr int kUploadsPerFrame = 2;
constexpr double kSizeDistance = 400.0;	// a part stays drawn while its smallest instance radius / distance >= 1/400 (as for buildings)
constexpr double kLandmarkDistance = 250.0;	// ...and a big one past its class distance while its largest instance radius / distance >= 1/250 (about 7 px at 1385 p, fovy 45)
constexpr float kViewClass[4] = { 50.0f, 100.0f, 800.0f, 4000.0f };	// exe table, indexed by `visibility range` CLOSE..FEATURE
constexpr float kRangeScale = 10.0f;	// distance = foliage range x table x 10
constexpr float kFadeStart = 0.8f;	// the fade over the last 20% of a record's draw distance (blended; dithered in the shadow maps)
constexpr double kLooseMargin = 1500.0;	// culling margin before a group's mesh size is known
constexpr double kShadowMinTexels[4] = { 2.0, 2.0, 4.0, 14.0 };	// per cascade: foliage whose reach (radius x scale) is under this many shadow texels is not drawn into it
constexpr int kLevels = Foliage::kLevels;
constexpr struct { int maxCount; float maxExtent; } kLevel[kLevels] = { { 96, 500.0f }, { 200, 800.0f }, { 400, 1800.0f } };	// node size per level: instances, longer x/z side in world units
constexpr int kLeafMin = 4;		// a node with this many instances or fewer is never split, nor below kLeafDepth levels
constexpr int kLeafDepth = 9;
constexpr int kShadowLevel[4] = { 0, 0, 1, 2 };	// node list used by each shadow cascade (0 = the camera pass's fine nodes)
constexpr double kShadowListDrift = 256.0;	// metres the camera may move from a shadow list's origin before it is rebuilt

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

}

struct Foliage::Context
{
	kdata::GameData gd;
	kworld::Heightmap terrain;
	kworld::BiomeMap biomes;
	kworld::Features features;
	kworld::Towns towns;
	std::vector<kworld::FeatureRecord> records;	// every FOLIAGE_MESH record, resolved
	std::unordered_map<std::string, int> recordIndex;
	std::vector<float> viewClass;	// per record: draw distance in view-range units, before the `foliage range` multiplier (see viewClassOf)
	size_t drawable = 0;
	double loadMs = 0;
};

Foliage::Foliage() = default;
Foliage::~Foliage() = default;

void Foliage::init(WorldBuildings* wb, const std::string& dataDir)
{
	wb_ = wb;
	dataDir_ = dataDir;
	loader_ = std::thread([this]
	{
		const auto t0 = Clock::now();
		try
		{
			kdata::GameData gd = kworld::loadMergedGameData(dataDir_);
			kworld::Heightmap terrain = kworld::Heightmap::open(dataDir_ + "/newland/land/fullmap.tif");
			kworld::BiomeMap biomes = kworld::BiomeMap::load(dataDir_ + "/newland/land/biomemap.png");
			kworld::Features features = kworld::Features::join(gd, dataDir_);
			kworld::Towns towns = kworld::Towns::join(gd, dataDir_);
			std::unique_ptr<Context> c(new Context{ std::move(gd), std::move(terrain), std::move(biomes), std::move(features), std::move(towns), {}, {}, {}, 0, 0 });
			const std::int32_t type = kworld::itemTypeByName("FOLIAGE_MESH");
			for (const kdata::Record& r : c->gd.records())
			{
				if (r.type != type) { continue; }
				c->recordIndex.emplace(r.stringID, (int)c->records.size());
				c->records.push_back(kworld::resolveFeatureRecord(c->gd, r.stringID));
				c->drawable += !c->records.back().meshes.empty();
			}
			// Draw distance per record (FOLIAGE_SCALE.md section 3): the farthest `visibility range` class of a layer that lists it
			// (directly or through a parent's `meshes`); wind layers cap the class at FAR. Records in no layer count as FAR.
			c->viewClass.assign(c->records.size(), 0.0f);
			const std::int32_t layerType = kworld::itemTypeByName("FOLIAGE_LAYER");
			std::function<void(const std::string&, float, int)> mark = [&](const std::string& id, float dist, int depth)
			{
				const auto it = c->recordIndex.find(id);
				if (it == c->recordIndex.end() || depth > 8) { return; }
				c->viewClass[it->second] = std::max(c->viewClass[it->second], dist);
				if (const kdata::RefCategory* kids = c->gd.find(id)->findRefs("meshes")) { for (const auto& e : kids->entries) { mark(e.targetID, dist, depth + 1); } }
			};
			for (const kdata::Record& r : c->gd.records())
			{
				const kdata::RefCategory* meshes = r.type == layerType ? r.findRefs("meshes") : nullptr;
				if (meshes == nullptr) { continue; }
				const int vis = std::clamp((int)r.intOr("visibility range", 1), 0, r.boolOr("wind", false) ? 2 : 3);
				for (const auto& e : meshes->entries) { mark(e.targetID, kViewClass[vis] * kRangeScale, 0); }
			}
			for (float& d : c->viewClass) { if (d == 0.0f) { d = kViewClass[2] * kRangeScale; } }
			c->loadMs = msSince(t0);
			stats_.contextMs = c->loadMs;
			char buf[200];
			snprintf(buf, sizeof buf, "foliage context loaded in %.0f ms; %zu FOLIAGE_MESH records, %zu with a mesh", c->loadMs, c->records.size(), c->drawable);
			report_ = buf;
			ctx_ = std::move(c);
			recordParts_.resize(ctx_->records.size());
			recordPartsDone_.assign(ctx_->records.size(), 0);
			ready_ = true;
		}
		catch (const std::exception& e)
		{
			fprintf(stderr, "foliage: context load failed: %s\n", e.what());
			failed_ = true;
		}
	});
	for (int i = 0; i < kGenThreads; i++) { threads_.emplace_back([this] { worker(); }); }
}

void Foliage::shutdown()
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		quit_ = true;
	}
	jobCv_.notify_all();
	for (std::thread& t : threads_) { t.join(); }
	threads_.clear();
	if (loader_.joinable()) { loader_.join(); }
	for (auto& [idx, b] : gpu_) { if (b.id != SG_INVALID_ID) { sg_destroy_buffer(b); } }	// render side; cleanup() flushed the resource jobs first
	gpu_.clear();
	cells_.clear();
}

// ---- generation (worker threads) ----

void Foliage::worker()
{
	for (;;)
	{
		int idx;
		{
			std::unique_lock<std::mutex> lk(mu_);
			jobCv_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
			if (quit_) { return; }
			idx = jobs_.front();
			jobs_.pop_front();
		}
		const auto t0 = Clock::now();
		CellCpu out;
		out.cell = idx;
		try
		{
			const Context& c = *ctx_;
			const kworld::ZoneCell cell = kworld::zoneCellOfIndex(idx);
			const std::vector<kworld::FoliageInstance> inst = kworld::generateFoliage(cell, c.terrain, c.biomes, c.gd, c.features, c.towns);
			// bucket by record (counting sort), positions relative to the cell's min corner
			std::vector<int> rec(inst.size()), start(c.records.size() + 1, 0);
			for (size_t i = 0; i < inst.size(); i++)
			{
				const auto it = c.recordIndex.find(inst[i].recordID);
				rec[i] = it != c.recordIndex.end() && !c.records[it->second].meshes.empty() ? it->second : -1;
				if (rec[i] >= 0) { start[rec[i] + 1]++; } else { out.unresolved++; }
			}
			for (size_t r = 0; r < c.records.size(); r++) { start[r + 1] += start[r]; }
			out.data.resize((size_t)start.back() * kInstanceFloats);
			std::vector<int> fill(start.begin(), start.end() - 1);
			out.groups.resize(c.records.size());
			for (size_t r = 0; r < c.records.size(); r++)
			{
				out.groups[r] = { (int)r, start[r], start[r + 1] - start[r], { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, 1e30f, 0.0f };
			}
			const double ox = kworld::zoneMinX(cell.x), oz = kworld::zoneMinZ(cell.z);
			for (size_t i = 0; i < inst.size(); i++)
			{
				if (rec[i] < 0) { continue; }
				const kworld::FoliageInstance& f = inst[i];
				out.children += f.isChild;
				const float p[3] = { (float)(f.x - ox), (float)f.y, (float)(f.z - oz) };
				float* d = &out.data[(size_t)fill[rec[i]]++ * kInstanceFloats];
				const float v[kInstanceFloats] = { p[0], p[1], p[2], f.scale, (float)f.qx, (float)f.qy, (float)f.qz, (float)f.qw, 1.0f, 1.0f, 1.0f, wb_->detail ? wb_->detail->rowAt(f.x, f.z) : 0.0f };
				std::copy(v, v + kInstanceFloats, d);
				Group& g = out.groups[rec[i]];
				for (int k = 0; k < 3; k++) { g.bmin[k] = std::min(g.bmin[k], p[k]); g.bmax[k] = std::max(g.bmax[k], p[k]); }
				g.minScale = std::min(g.minScale, f.scale);
				g.maxScale = std::max(g.maxScale, f.scale);
			}
			// Split each record's group into spatial nodes (kd-style, on the longer x/z extent; a node is a contiguous range of instances) so culling, in
			// the camera pass and in the shadow cascades, works on boxes of a few dozen instances instead of a whole cell. Three node lists with
			// growing node size: kLevel[0] for the camera pass and the small cascades, [1] and [2] for the big ones (fewer draw calls). minScale stays
			// the record-wide value: it sets the dithered fade distance, which must not differ between nodes.
			struct Inst { float f[kInstanceFloats]; };
			Inst* const all = reinterpret_cast<Inst*>(out.data.data());
			std::vector<Group> nodes[kLevels];
			std::function<void(const Group&, int, int)> split = [&](const Group& g, int depth, int covered)	// covered: levels already emitted by an ancestor
			{
				const int axis = g.bmax[0] - g.bmin[0] >= g.bmax[2] - g.bmin[2] ? 0 : 2;
				const float extent = g.bmax[axis] - g.bmin[axis];
				const bool atomic = g.count <= kLeafMin || depth >= kLeafDepth;
				for (int l = kLevels - 1; l >= 0; l--)
				{
					if (!(covered >> l & 1) && (atomic || (g.count <= kLevel[l].maxCount && extent <= kLevel[l].maxExtent))) { nodes[l].push_back(g); covered |= 1 << l; }
				}
				if (covered & 1) { return; }
				const float mid = (g.bmin[axis] + g.bmax[axis]) * 0.5f;
				Inst* const m = std::partition(all + g.first, all + g.first + g.count, [&](const Inst& i) { return i.f[axis] < mid; });
				const int nl = (int)(m - (all + g.first));
				if (nl == 0 || nl == g.count)
				{
					for (int l = 0; l < kLevels; l++) { if (!(covered >> l & 1)) { nodes[l].push_back(g); } }
					return;
				}
				for (int side = 0; side < 2; side++)
				{
					Group h = { g.record, side ? g.first + nl : g.first, side ? g.count - nl : nl, { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, g.minScale, 0.0f };
					for (int i = h.first; i < h.first + h.count; i++)
					{
						for (int k = 0; k < 3; k++) { h.bmin[k] = std::min(h.bmin[k], all[i].f[k]); h.bmax[k] = std::max(h.bmax[k], all[i].f[k]); }
						h.maxScale = std::max(h.maxScale, all[i].f[3]);
					}
					split(h, depth + 1, covered);
				}
			};
			for (const Group& g : out.groups) { if (g.count > 0) { split(g, 0, 0); } }
			out.groups.swap(nodes[0]);
			out.shadowGroups[0].swap(nodes[1]);
			out.shadowGroups[1].swap(nodes[2]);
		}
		catch (const std::exception& e)
		{
			fprintf(stderr, "foliage: cell %d.%d failed: %s\n", idx % 64, idx / 64, e.what());
			out.data.clear();
			out.groups.clear();
		}
		out.ms = msSince(t0);
		{
			std::lock_guard<std::mutex> lk(mu_);
			results_.push_back(std::move(out));
		}
		resCv_.notify_all();
	}
}

// ---- main thread ----

// FOLIAGE_MESH record -> parts of WorldBuildings (the `mesh`, and its `leaves mesh` as a second, alpha-tested part), like the hand-placed features.
const std::vector<uint32_t>& Foliage::partsOf(int record)
{
	std::vector<uint32_t>& parts = recordParts_[record];
	if (recordPartsDone_[record]) { return parts; }
	recordPartsDone_[record] = 1;
	for (const kworld::FeatureMesh& m : ctx_->records[record].meshes)
	{
		BuildingPartDef d;
		d.mesh = m.mesh;
		d.texture = m.texture;
		if (uvMapped((int)m.mode)) { d.normal = m.normalMap; }	// the lit normal; EMISSIVE: glow; FOLIAGE: the cut-out mask
		if (m.mode == kworld::FeatureMode::DualTexture || m.mode == kworld::FeatureMode::DualTriplanar) { d.texture2 = m.texture2; d.normal2 = m.normalMap2; }
		d.tileX = m.tileX;
		d.tileY = m.tileY;
		d.featureMode = (int)m.mode;
		d.alphaCut = m.alphaThreshold;
		d.sway = m.leaves;	// the leaves sway, the trunk stays (mesh.glsl inst_vs; a deliberate deviation: the original's leaves are static)
		parts.push_back(wb_->addPart(d));
	}
	return parts;
}

void Foliage::accept(CellCpu& cpu)
{
	queued_.erase(cpu.cell);
	Cell& c = cells_[cpu.cell];
	c.groups = std::move(cpu.groups);
	for (int l = 0; l < 2; l++) { c.shadowGroups[l] = std::move(cpu.shadowGroups[l]); }
	c.instances = cpu.data.size() / kInstanceFloats;	// (before the data moves into the job below)
	c.children = cpu.children;
	c.lastUsed = frame_;
	cellGen_++;
	if (!cpu.data.empty())	// the buffer is a resource job: the render side makes it before the packet's draw (frame lane, like the grass tiles)
	{
		const size_t bytes = cpu.data.size() * sizeof(float);
		rq::push("foliage-cell", [this, cell = cpu.cell, data = std::move(cpu.data)]
		{
			sg_buffer_desc bd = {};
			bd.data = { data.data(), data.size() * sizeof(float) };
			bd.label = "foliage-instances";
			gpu_[cell] = sg_make_buffer(&bd);
		}, rq::Lane::kFrame, bytes);
	}
	stats_.generated++;
	stats_.genMs += cpu.ms;
	stats_.genMaxMs = std::max(stats_.genMaxMs, cpu.ms);
	stats_.unresolved += cpu.unresolved;
}

bool Foliage::take(bool wait)
{
	CellCpu c;
	{
		std::unique_lock<std::mutex> lk(mu_);
		if (wait && results_.empty() && outstanding_ > 0) { resCv_.wait(lk, [this] { return !results_.empty(); }); }
		if (results_.empty()) { return false; }
		c = std::move(results_.front());
		results_.pop_front();
	}
	outstanding_--;
	accept(c);
	return true;
}

// Draw distance of a group: the layer's class distance x foliage range, at most the size limit (a part stays drawn while its smallest instance radius
// / distance >= 1/400); then at least the landmark distance, so a big rock or tree that is still large on screen stays as long as the terrain behind it
// (user 2026-11-10: "The rocks that are fading look like they're closer than the far mountains", kenshi-k16.2.63); at most the load radius (nothing
// exists beyond it) and the drawLimit debug cap. The mesh fades out over the last 1 - kFadeStart.
double Foliage::fadeEnd(const Group& g, double meshRadius) const
{
	const double byClass = std::min(foliageRange * ctx_->viewClass[g.record], meshRadius * g.minScale * kSizeDistance);
	const double d = std::min(std::max(byClass, meshRadius * g.maxScale * kLandmarkDistance), radius * (double)WorldBuildings::kCellSize);
	return wb_->drawLimit > 0 ? std::min(d, wb_->drawLimit) : d;
}

void Foliage::prepare(const double cam[3], const float mvp[16], float, bool blocking, unsigned shadowLevels, FrameData& out)
{
	frame_++;
	stats_.cullMs = stats_.uploadMs = 0;
	stats_.instancesDrawn = stats_.draws = stats_.groupsDrawn = stats_.held = stats_.greyDraws = 0;
	out.draws.clear();
	for (auto& l : out.shadow) { l.reset(); }
	for (int k = 0; k < 3; k++) { out.cam[k] = cam[k]; }
	out.gen = cellGen_;
	if (!ready_) { return; }
	if (!reported_) { reported_ = true; printf("%s\n", report_.c_str()); }
	auto t0 = Clock::now();

	// Cells within the load radius, nearest first.
	const kworld::ZoneCell cc = kworld::zoneCellOf(cam[0], cam[2]);
	std::vector<std::pair<double, int>> wanted;
	auto cellDistance = [&](int x, int z)
	{
		const double ex = std::max({ kworld::zoneMinX(x) - cam[0], cam[0] - kworld::zoneMinX(x + 1), 0.0 });
		const double ez = std::max({ kworld::zoneMinZ(z) - cam[2], cam[2] - kworld::zoneMinZ(z + 1), 0.0 });
		return std::hypot(ex, ez);
	};
	for (int dz = -radius - 1; dz <= radius + 1; dz++)
	{
		for (int dx = -radius - 1; dx <= radius + 1; dx++)
		{
			const int x = cc.x + dx, z = cc.z + dz;
			if (x < 0 || z < 0 || x >= kworld::kZoneCellsPerAxis || z >= kworld::kZoneCellsPerAxis) { continue; }
			const double d = cellDistance(x, z);
			if (d <= radius * WorldBuildings::kCellSize) { wanted.push_back({ d, x + kworld::kZoneCellsPerAxis * z }); }
		}
	}
	std::sort(wanted.begin(), wanted.end());
	for (const auto& [d, idx] : wanted)
	{
		if (!blocking && queued_.size() >= kQueueAhead) { break; }
		if (cells_.count(idx) || queued_.count(idx)) { continue; }
		queued_.insert(idx);
		outstanding_++;
		{
			std::lock_guard<std::mutex> lk(mu_);
			jobs_.push_back(idx);
		}
		jobCv_.notify_one();
	}
	if (blocking) { while (take(true)) {} }
	else { for (int n = 0; n < kUploadsPerFrame && take(false); n++) {} }
	std::vector<int> frees;
	for (auto it = cells_.begin(); it != cells_.end();)	// free the cells that left the radius (one cell of hysteresis)
	{
		if (cellDistance(it->first % 64, it->first / 64) > (radius + 1) * WorldBuildings::kCellSize)
		{
			frees.push_back(it->first);
			it = cells_.erase(it);
			cellGen_++;
		} else { ++it; }
	}
	if (!frees.empty())	// after this frame's creates (same lane, submit order)
	{
		rq::push("foliage-free", [this, frees = std::move(frees)]
		{
			for (const int cell : frees)
			{
				const auto it = gpu_.find(cell);
				if (it == gpu_.end()) { continue; }
				if (it->second.id != SG_INVALID_ID) { sg_destroy_buffer(it->second); }
				gpu_.erase(it);
			}
		});
	}
	out.gen = cellGen_;
	stats_.uploadMs = msSince(t0);
	t0 = Clock::now();

	// Frustum side planes (Gribb-Hartmann), normalised.
	float planes[4][4];
	for (int k = 0; k < 4; k++)
	{
		const int axis = k >> 1;
		const float s = (k & 1) ? -1.0f : 1.0f;
		for (int c = 0; c < 4; c++) { planes[k][c] = mvp[c * 4 + 3] + s * mvp[c * 4 + axis]; }
		const float l = std::sqrt(planes[k][0] * planes[k][0] + planes[k][1] * planes[k][1] + planes[k][2] * planes[k][2]);
		for (int c = 0; c < 4; c++) { planes[k][c] /= l; }
	}
	// Draw distance per record: foliage range x class table x 10 (the exe's single detail level, no impostors); the load radius still caps what exists.
	auto maxDistOf = [&](int record) { return std::min(foliageRange * ctx_->viewClass[record], radius * (double)WorldBuildings::kCellSize); };
	auto sphereOut = [&](const double c[3], double r)
	{
		for (int p = 0; p < 4; p++) { if (planes[p][0] * c[0] + planes[p][1] * c[1] + planes[p][2] * c[2] + planes[p][3] < -r) { return true; } }
		return false;
	};

	// Pass 1: groups that could be visible (mesh size unknown yet) -> parts to load.
	struct Cand { int cell; int group; uint32_t part; float base[3]; double centre[3], half, dist; const Cell* cp; };
	std::vector<Cand> cands;
	stats_.cellsLoaded = cells_.size();
	stats_.cellsPending = queued_.size();
	stats_.instancesLoaded = stats_.groupsLoaded = stats_.children = 0;
	needed_.clear();
	if (stamp_.size() < wb_->data_.parts.size()) { stamp_.resize(wb_->data_.parts.size() + 256, 0); }
	for (const auto& [idx, cell] : cells_)
	{
		stats_.instancesLoaded += cell.instances;
		stats_.children += cell.children;
		stats_.groupsLoaded += cell.groups.size();
		const double base[3] = { kworld::zoneMinX(idx % 64) - cam[0], -cam[1], kworld::zoneMinZ(idx / 64) - cam[2] };
		for (size_t gi = 0; gi < cell.groups.size(); gi++)
		{
			const Group& g = cell.groups[gi];
			double c[3], half = 0;
			for (int k = 0; k < 3; k++)
			{
				c[k] = base[k] + (g.bmin[k] + g.bmax[k]) * 0.5;
				half += (double)(g.bmax[k] - g.bmin[k]) * (g.bmax[k] - g.bmin[k]);
			}
			half = std::sqrt(half) * 0.5;
			const double dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
			if (dist - half - kLooseMargin > maxDistOf(g.record) || sphereOut(c, half + kLooseMargin)) { continue; }
			for (const uint32_t pid : partsOf(g.record))
			{
				if (stamp_.size() <= pid) { stamp_.resize(pid + 256, 0); }
				if (stamp_[pid] != frame_) { stamp_[pid] = frame_; needed_.push_back(pid); }
				cands.push_back({ idx, (int)gi, pid, { (float)base[0], (float)base[1], (float)base[2] }, { c[0], c[1], c[2] }, half, dist, &cell });
			}
		}
	}
	for (;;)
	{
		for (const uint32_t pid : needed_) { wb_->ensurePart(pid); }
		if (!blocking) { wb_->drain(false, 8); break; }
		if (wb_->outstanding_ == 0) { break; }
		wb_->drain(true, -1);
	}

	// Pass 2: with the mesh size, cull by projected size and exact extent, and make the draw list.
	for (const Cand& k : cands)
	{
		const WorldBuildings::PartRt& rt = wb_->partRt_[k.part];
		if (rt.mesh < 0 || !rt.resolved || wb_->meshes_[rt.mesh].state != WorldBuildings::Ready) { continue; }
		if (!diffuseGateOff() && !wb_->partTexturesReady(k.part)) { stats_.held += rt.subTex.size(); continue; }	// mesh ready, a diffuse still streaming: held out, never drawn on the grey fallback (kenshi-k16.2.298.2)
		const WorldBuildings::MeshEntry& m = wb_->meshes_[rt.mesh];
		const Group& g = k.cp->groups[k.group];
		const double centreLen = std::sqrt((double)m.center[0] * m.center[0] + (double)m.center[1] * m.center[1] + (double)m.center[2] * m.center[2]);
		const double reach = (m.radius + centreLen) * g.maxScale;
		const double end = fadeEnd(g, m.radius);
		if (k.dist - k.half - reach > end || sphereOut(k.centre, k.half + reach)) { continue; }
		out.draws.push_back({ k.cell, g.first, g.count, k.part, (int)m.subs.size(), { k.base[0], k.base[1], k.base[2] }, (float)end, k.dist + k.half + reach > end * kFadeStart });
		stats_.draws += m.subs.size();
		for (const int t : rt.subTex) { stats_.greyDraws += t >= 0 && wb_->textures_[t].state != WorldBuildings::Ready && wb_->textures_[t].state != WorldBuildings::Failed; }
		stats_.instancesDrawn += (size_t)g.count;
		stats_.groupsDrawn++;
	}
	if (shadowLevels)
	{
		const uint64_t key = shadowKey();
		for (int l = 0; l < kLevels; l++)
		{
			if (!(shadowLevels >> l & 1)) { continue; }
			ShadowCache& sc = shadowCache_[l];
			double d2 = 0;
			if (sc.valid) { for (int k = 0; k < 3; k++) { d2 += (cam[k] - sc.list->origin[k]) * (cam[k] - sc.list->origin[k]); } }
			if (!sc.valid || sc.key != key || d2 > kShadowListDrift * kShadowListDrift)
			{
				auto list = std::make_shared<ShadowList>();
				buildShadowList(cam, l, *list);
				sc.list = std::move(list);
				sc.key = key;
				sc.valid = true;
			}
			out.shadow[l] = sc.list;
		}
	}
	stats_.cullMs = msSince(t0);
}

void Foliage::render(const FrameData& fd, const float mvp[16], const LightParams& light)
{
	if (fd.draws.empty()) { return; }
	const auto t0 = Clock::now();
	wb_->beginDraws(true);
	int lastCell = -1;
	sg_buffer buf = {};
	for (const Draw& d : fd.draws)
	{
		if (d.cell != lastCell) { lastCell = d.cell; const auto g = gpu_.find(d.cell); buf = g == gpu_.end() ? sg_buffer{} : g->second; }
		if (buf.id == SG_INVALID_ID) { continue; }
		wb_->applyCull(d.part);
		for (int s = 0; s < d.subs; s++) { wb_->draw(d.part, s, buf, d.first * kInstanceBytes, d.count, d.base, mvp, light, d.fadeEnd * kFadeStart, d.fadeEnd, 1); }
	}
	framestats::addFoliage(msSince(t0), 0, 0);
}

// The fade band (kenshi-k16.2.63): the part of each group between kFadeStart and its end, which render() left out. Its nearest surface goes into depth
// first, then the colour is blended at exactly that depth with alpha 1 - fade (mesh.glsl), so a fading tree shows one even layer over what lies behind
// it instead of the screen door's pixel pattern. Two-sided like renderGlass: at these distances no camera is inside a rock.
void Foliage::renderFadeBand(const FrameData& fd, const float mvp[16], const LightParams& light)
{
	if (fd.draws.empty()) { return; }
	const auto t0 = Clock::now();
	for (const bool depthOnly : { true, false })
	{
		bool begun = false;
		int lastCell = -1;
		sg_buffer buf = {};
		for (const Draw& d : fd.draws)
		{
			if (!d.band) { continue; }
			if (!begun) { wb_->beginFadeBand(depthOnly); begun = true; }
			if (d.cell != lastCell) { lastCell = d.cell; const auto g = gpu_.find(d.cell); buf = g == gpu_.end() ? sg_buffer{} : g->second; }
			if (buf.id == SG_INVALID_ID) { continue; }
			for (int s = 0; s < d.subs; s++) { wb_->draw(d.part, s, buf, d.first * kInstanceBytes, d.count, d.base, mvp, light, d.fadeEnd * kFadeStart, d.fadeEnd, 2); }
		}
	}
	framestats::addFoliage(msSince(t0), 0, 0);
}

// What a shadow list depends on besides the camera: the cell set, which parts are drawable, the ranges that set each group's fade end.
uint64_t Foliage::shadowKey() const
{
	uint64_t k = cellGen_ * 0x9e3779b97f4a7c15ull;
	auto mix = [&k](uint64_t v) { k ^= v + 0x9e3779b97f4a7c15ull + (k << 6) + (k >> 2); };
	uint64_t ready = 0;
	for (const auto& parts : recordParts_)
	{
		for (const uint32_t pid : parts)
		{
			float r, c;
			if (wb_->partTexturesReady(pid) && wb_->partReady(pid, &r, &c)) { ready += pid * 2654435761ull + 1; mix(std::bit_cast<uint32_t>(r)); }
		}
	}
	mix(ready);
	mix(std::bit_cast<uint64_t>(foliageRange));
	mix(std::bit_cast<uint64_t>(wb_->drawLimit));
	mix((uint64_t)radius);
	return k;
}

// Every (group, part) that can cast a shadow, with what the per-cascade test needs (computed once instead of once per cascade); kept across frames (prepare).
void Foliage::buildShadowList(const double cam[3], int level, ShadowList& sl)
{
	std::vector<ShadowCand>& list = sl.cands;
	for (int k = 0; k < 3; k++) { sl.origin[k] = cam[k]; }
	for (const auto& [idx, cell] : cells_)
	{
		const double base[3] = { kworld::zoneMinX(idx % 64) - cam[0], -cam[1], kworld::zoneMinZ(idx / 64) - cam[2] };
		const std::vector<Group>& groups = level ? cell.shadowGroups[level - 1] : cell.groups;
		for (size_t gi = 0; gi < groups.size(); gi++)
		{
			const Group& g = groups[gi];
			double ctr[3], half = 0;
			for (int k = 0; k < 3; k++)
			{
				ctr[k] = base[k] + (g.bmin[k] + g.bmax[k]) * 0.5;
				half += (double)(g.bmax[k] - g.bmin[k]) * (g.bmax[k] - g.bmin[k]);
			}
			half = std::sqrt(half) * 0.5;
			const double dist = std::sqrt(ctr[0] * ctr[0] + ctr[1] * ctr[1] + ctr[2] * ctr[2]);
			for (const uint32_t pid : partsOf(g.record))
			{
				float radius, centreLen;
				if ((!diffuseGateOff() && !wb_->partTexturesReady(pid)) || !wb_->partReady(pid, &radius, &centreLen)) { continue; }	// cut-out leaves shadow by their diffuse alpha: wait for it (kenshi-k16.2.298.2)
				const double end = fadeEnd(g, radius), size = (radius + centreLen) * g.maxScale;
				if (dist - half - size > end + kShadowListDrift) { continue; }	// the list outlives the camera: kept for a drift of up to kShadowListDrift
				list.push_back({ idx, g.first, g.count, pid, (int)wb_->meshes_[wb_->partRt_[pid].mesh].subs.size(),
					{ (float)base[0], (float)base[1], (float)base[2] }, { (float)ctr[0], (float)ctr[1], (float)ctr[2] }, (float)(half + size), (float)size, (float)end });
			}
		}
	}
}

void Foliage::renderShadow(const FrameData& fd, const ShadowCascade& c)
{
	const auto t0 = Clock::now();
	const int ci = c.index, level = kShadowLevel[ci];
	wb_->beginShadowDraws();
	const double minSize = c.texel * kShadowMinTexels[ci];
	size_t draws = 0;
	int lastCell = -1;
	sg_buffer buf = {};
	if (!fd.shadow[level]) { framestats::addFoliage(0, msSince(t0), 0); return; }
	const ShadowList& sl = *fd.shadow[level];
	const float sh[3] = { (float)(sl.origin[0] - fd.cam[0]), (float)(sl.origin[1] - fd.cam[1]), (float)(sl.origin[2] - fd.cam[2]) };	// list origin -> this frame's camera
	for (const ShadowCand& k : sl.cands)
	{
		if (k.size < minSize) { continue; }
		const float ctr[3] = { k.ctr[0] + sh[0], k.ctr[1] + sh[1], k.ctr[2] + sh[2] };
		bool out = false;
		for (int p = 0; p < 4 && !out; p++) { out = c.planes[p][0] * ctr[0] + c.planes[p][1] * ctr[1] + c.planes[p][2] * ctr[2] + c.planes[p][3] < -k.reach; }
		if (out) { continue; }
		if (k.cell != lastCell) { lastCell = k.cell; const auto g = gpu_.find(k.cell); buf = g == gpu_.end() ? sg_buffer{} : g->second; }
		if (buf.id == SG_INVALID_ID) { continue; }
		const float base[3] = { k.base[0] + sh[0], k.base[1] + sh[1], k.base[2] + sh[2] };
		for (int s = 0; s < k.subs; s++) { wb_->drawShadow(k.part, s, buf, k.first * kInstanceBytes, k.count, base, c.lvp, c.bias, k.fadeEnd * kFadeStart, k.fadeEnd); }
		draws += k.subs;
	}
	framestats::addFoliage(0, msSince(t0), draws);
}

Foliage::Stats Foliage::stats() const { return stats_; }

GrassSource Foliage::grassSource() const { return ready_ ? GrassSource{ &ctx_->gd, &ctx_->terrain, &ctx_->biomes } : GrassSource{}; }

std::string Foliage::nearReport(const double pos[3], double radius) const
{
	if (!ready()) { return "foliage not ready\n"; }
	const Context& c = *ctx_;
	std::string out;
	char buf[256];
	for (const kworld::FoliageInstance& f : kworld::generateFoliage(kworld::zoneCellOf(pos[0], pos[2]), c.terrain, c.biomes, c.gd, c.features, c.towns))
	{
		if ((f.x - pos[0]) * (f.x - pos[0]) + (f.z - pos[2]) * (f.z - pos[2]) > radius * radius) { continue; }
		const kdata::Record* r = c.gd.find(f.recordID);
		snprintf(buf, sizeof buf, "foliage %s at %.1f %.1f %.1f scale %.3f q %.3f %.3f %.3f %.3f\n", r ? r->name.c_str() : f.recordID.c_str(), f.x, f.y, f.z, f.scale, f.qw, f.qx, f.qy, f.qz);
		out += buf;
		const auto it = c.recordIndex.find(f.recordID);
		if (it == c.recordIndex.end() || recordPartsDone_.size() <= (size_t)it->second || !recordPartsDone_[it->second]) { continue; }
		for (const uint32_t pid : recordParts_.at(it->second))
		{
			const WorldBuildings::PartRt& rt = wb_->partRt_[pid];
			if (rt.mesh < 0 || wb_->meshes_[rt.mesh].state != WorldBuildings::Ready) { continue; }
			const WorldBuildings::MeshEntry& m = wb_->meshes_[rt.mesh];
			double mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
			for (int k = 0; k < 8; k++)	// the mesh box's corners, scaled, rotated by q (w first), moved to the instance
			{
				const double v[3] = { (k & 1 ? m.bmax[0] : m.bmin[0]) * f.scale, (k & 2 ? m.bmax[1] : m.bmin[1]) * f.scale, (k & 4 ? m.bmax[2] : m.bmin[2]) * f.scale };
				const double t[3] = { 2 * (f.qy * v[2] - f.qz * v[1]), 2 * (f.qz * v[0] - f.qx * v[2]), 2 * (f.qx * v[1] - f.qy * v[0]) };
				const double w[3] = { v[0] + f.qw * t[0] + (f.qy * t[2] - f.qz * t[1]) + f.x, v[1] + f.qw * t[1] + (f.qz * t[0] - f.qx * t[2]) + f.y, v[2] + f.qw * t[2] + (f.qx * t[1] - f.qy * t[0]) + f.z };
				for (int a = 0; a < 3; a++) { mn[a] = std::min(mn[a], w[a]); mx[a] = std::max(mx[a], w[a]); }
			}
			snprintf(buf, sizeof buf, "  mesh %s box %.0f %.0f %.0f - %.0f %.0f %.0f\n", m.path.c_str(), mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
			out += buf;
		}
	}
	return out;
}

std::string Foliage::cellReport(const double pos[3]) const
{
	const kworld::ZoneCell zc = kworld::zoneCellOf(pos[0], pos[2]);
	const auto it = cells_.find(kworld::zoneIndexOf(zc));
	char buf[160];
	if (it == cells_.end()) { snprintf(buf, sizeof buf, "cell %d.%d not resident", zc.x, zc.z); }
	else { snprintf(buf, sizeof buf, "cell %d.%d: %zu instances (%zu children), %zu records", zc.x, zc.z, it->second.instances, it->second.children, it->second.groups.size()); }
	std::vector<const Group*> top;
	if (it != cells_.end()) { for (const Group& g : it->second.groups) { top.push_back(&g); } }
	std::sort(top.begin(), top.end(), [](const Group* a, const Group* b) { return a->count > b->count; });
	std::string out = buf;
	for (size_t i = 0; i < top.size() && i < 6; i++) { out += (i ? ", " : "; top: ") + ctx_->gd.find(ctx_->records[top[i]->record].recordID)->name + " x" + std::to_string(top[i]->count); }
	return out;
}
