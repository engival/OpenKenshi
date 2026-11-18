#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sokol_gfx.h"
#include "worldbuildings.hpp"

namespace kdata { class GameData; }
namespace kworld { class Heightmap; class BiomeMap; }

// The generator context that Grass shares (merged game data, heightmap, biome map): pointers stay valid until Foliage::shutdown().
struct GrassSource
{
	const kdata::GameData* gd = nullptr;
	const kworld::Heightmap* terrain = nullptr;
	const kworld::BiomeMap* biomes = nullptr;
};

// Procedural foliage (M2e): kworld::generateFoliage per zone cell on worker threads, instance data uploaded once per cell into one static vertex
// buffer, drawn through WorldBuildings' mesh/texture caches and instanced pipeline. Cells stream with the camera (radius cells, freed one cell
// beyond). Per frame (render hook 7b, kenshi-k16.2.205.12.18): prepare() on the logic side (after WorldBuildings::update: streaming, the workers' results,
// eviction, culling, the draw and shadow lists) fills a FrameData in the frame packet slot; the cells' instance buffers are resource jobs (rq, frame lane:
// created when a worker result is taken, destroyed when the cell leaves the radius); render() / renderFadeBand() / renderShadow() on the render side read
// only the FrameData and the render-side buffer table (gpu_). Neither reads the logic-side cell table. Counters drawn on the render side go to FrameStats.
class Foliage
{
public:
	Foliage();
	~Foliage();
	void init(WorldBuildings* wb, const std::string& dataDir);
	void shutdown();
	bool ready() const { return ready_.load(); }	// generator context (game data, terrain, biome map, features, towns) loaded
	bool failed() const { return failed_.load(); }
	GrassSource grassSource() const;	// empty (gd == nullptr) until ready()

	static constexpr int kLevels = 3;	// shadow node levels (see foliage.cpp)
	struct Draw		// one (group, part): every submesh of the part is drawn with the group's instances
	{
		int cell;	// into the render side's buffer table
		int first, count;	// the group's instances in the cell buffer
		uint32_t part;
		int subs;
		float base[3];
		float fadeEnd;	// camera distance where the fade reaches zero
		bool band;	// some instance may lie in the fade band (drawn again by renderFadeBand)
	};
	struct ShadowCand	// one (group, part) that may cast a shadow this frame
	{
		int cell;
		int first, count;
		uint32_t part;
		int subs;
		float base[3], ctr[3];	// cell origin and group centre, relative to the camera
		float reach;		// group bounding radius + mesh reach
		float size;		// mesh reach alone (radius x scale), for the per-cascade size test
		float fadeEnd;
	};
	struct ShadowList
	{
		double origin[3] = {};	// the camera position the candidates' base / ctr are relative to
		std::vector<ShadowCand> cands;
	};
	// One frame's foliage work, in the frame packet slot (one per packet). prepare() fills it; render*() read only it.
	struct FrameData
	{
		std::vector<Draw> draws;
		// Per node level: null unless prepare() was asked for the level. Immutable and shared between packets: the list is rebuilt only when its key changes
		// (cell generation, which parts are drawable, the ranges, the camera drifting kShadowListDrift from the list's origin), not every frame (kenshi-k16.2.205.12.37).
		std::shared_ptr<const ShadowList> shadow[kLevels];
		double cam[3] = {};	// camera position the lists are relative to
		uint64_t gen = 0;	// the resident cell set's generation (the shadow cache's key, kenshi-k16.2.205.12.2)
	};
	// Logic side. cam: world position; mvp: projection * rotation-only view. blocking (scripted runs): wait for every cell in radius and its meshes.
	// shadowLevels: bit per node level whose shadow caster list the frame needs (0: no shadows).
	void prepare(const double cam[3], const float mvp[16], float fogDist, bool blocking, unsigned shadowLevels, FrameData& out);
	// Render side.
	void render(const FrameData& d, const float mvp[16], const LightParams& light);	// the opaque pass: everything short of each group's fade band
	void renderFadeBand(const FrameData& d, const float mvp[16], const LightParams& light);	// the fade band, blended: after everything opaque (kenshi-k16.2.63)
	// Draws the resident foliage into one shadow cascade (inside Shadow::beginPass/endPass). Culled by the cascade's box, not by the camera frustum, and
	// by size: parts smaller than a couple of shadow texels are skipped.
	void renderShadow(const FrameData& d, const ShadowCascade& c);

	struct Stats
	{
		size_t cellsLoaded = 0, cellsPending = 0, instancesLoaded = 0;	// resident
		size_t held = 0, greyDraws = 0;	// last frame: submesh draws held out while their diffuse streams (each would have drawn the grey fallback); submesh draws in the list on a not-ready diffuse (0 by the gate)
		size_t instancesDrawn = 0, draws = 0, groupsDrawn = 0, groupsLoaded = 0;	// last frame; instances counted per drawn part (leaves count again)
		double cullMs = 0, uploadMs = 0;	// last frame CPU: cull + draw list, taking the cell results (the draw submission ms are FrameStats::foliage)
		size_t generated = 0;	// cells generated since start
		double genMs = 0, genMaxMs = 0;	// summed / slowest generateFoliage + bucketing, worker thread time
		size_t unresolved = 0;	// generated instances whose FOLIAGE_MESH has no drawable mesh
		size_t children = 0;	// resident instances that are children of a parent mesh (FoliageInstance::isChild)
		double contextMs = 0;
	};
	Stats stats() const;
	std::string cellReport(const double pos[3]) const;	// instance counts of the cell under pos
	std::string nearReport(const double pos[3], double radius) const;	// the instances within radius (XZ) of pos, regenerated: record, position, scale, each loaded mesh's world box
	int radius = 4;
	double foliageRange = 6.0;	// multiplies every layer's draw distance. The game ships 1; we deliberately exceed it (smooth fade, no pop-in), see docs/CLIENT.md
	std::string report() const { return report_; }

private:
	struct Context;
	struct Group		// the instances of one FOLIAGE_MESH record in one cell
	{
		int record;
		int first, count;	// instance indices in the cell buffer
		float bmin[3], bmax[3];	// cell-local instance position bounds
		float minScale, maxScale;
	};
	struct CellCpu
	{
		int cell = 0;
		std::vector<float> data;	// 12 floats per instance, grouped by record
		std::vector<Group> groups;
		std::vector<Group> shadowGroups[2];	// coarser nodes of the same instances for the big shadow cascades
		size_t unresolved = 0, children = 0;
		double ms = 0;
	};
	struct Cell		// logic side: what the culling needs (the buffer itself is gpu_'s)
	{
		std::vector<Group> groups;
		std::vector<Group> shadowGroups[2];
		size_t instances = 0, children = 0;
		uint64_t lastUsed = 0;
	};
	void buildShadowList(const double cam[3], int level, ShadowList& list);
	struct ShadowCache { std::shared_ptr<const ShadowList> list; uint64_t key = 0; bool valid = false; };
	ShadowCache shadowCache_[kLevels];	// logic side: the level's last list and the key it was built under
	uint64_t shadowKey() const;
	double fadeEnd(const Group& g, double meshRadius) const;	// draw distance of a group: class distance x range, size limit, load radius, drawLimit
	void worker();
	const std::vector<uint32_t>& partsOf(int record);
	bool take(bool wait);
	void accept(CellCpu& cpu);

	WorldBuildings* wb_ = nullptr;
	std::string dataDir_;
	std::unique_ptr<Context> ctx_;
	std::thread loader_;
	std::atomic<bool> ready_{ false }, failed_{ false };
	std::vector<std::thread> threads_;
	std::mutex mu_;
	std::condition_variable jobCv_, resCv_;
	std::deque<int> jobs_;
	std::deque<CellCpu> results_;
	std::atomic<int> outstanding_{ 0 };
	bool quit_ = false;

	std::unordered_map<int, Cell> cells_;
	std::unordered_set<int> queued_;		// cell indices requested and not yet uploaded
	std::vector<uint64_t> stamp_;		// per part: frame it was last put in needed_
	std::vector<uint32_t> needed_;
	std::vector<std::vector<uint32_t>> recordParts_;	// per record: part ids in WorldBuildings, registered on first use
	std::vector<char> recordPartsDone_;
	uint64_t frame_ = 0;
	uint64_t cellGen_ = 0;	// a cell was accepted or freed (FrameData::gen)
	std::unordered_map<int, sg_buffer> gpu_;	// render side: cell index -> its instance buffer (made and destroyed by resource jobs)
	Stats stats_;
	std::string report_;
	bool reported_ = false;
};
