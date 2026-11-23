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
#include "world/worldbuildings.hpp"

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
	struct Draw		// one (group, part): every submesh of the part is drawn with the group's instances, resolved by prepare() (FrameData::list)
	{
		int buf;	// FrameData::bufs slot of the cell's instance buffer
		int first, subs;	// FrameData::list.opaque[first, first + subs): the opaque pass's draws; with `band`, the next `subs` are the fade band's
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
		float fadep[3];	// the per-instance fade end's parameters (fadeParams)
	};
	struct ShadowList
	{
		double origin[3] = {};	// the camera position the candidates' base / ctr are relative to
		std::vector<ShadowCand> cands;
		size_t flat = 0;	// candidates before adjacent nodes were merged (debug counter)
	};
	// One frame's foliage work, in the frame packet slot (one per packet). prepare() fills it; render*() read only it.
	struct FrameData
	{
		std::vector<Draw> draws;
		// The resolved draws (kenshi-k16.2.205.12.116, overlap review T20): the opaque and fade band draws of `draws`, and the shadow draws of the level lists'
		// candidates, built by prepare() through WorldBuildings::resolveDraw / resolveShadow; the render side replays them and reads no building table.
		// A cell's instance buffer is a resource job's: named by `bufs` slot (the replay puts it into the draw).
		WorldBuildings::DrawList list;
		std::vector<rq::Handle<sg_buffer>> bufs;
		struct ShadowRef { int first = -1; int buf = -1; };	// the candidate's draws in list.shadow [first, first + cand.subs); first -1: none this frame
		std::vector<ShadowRef> shadowRef[kLevels];	// per level, parallel to that level's list.cands
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
		size_t drawsFlat = 0;	// last frame: submesh draws one per node would make (draws is after adjacent nodes merged)
		size_t shadowCands[kLevels] = {}, shadowFlat[kLevels] = {};	// the level's shadow candidates (nodes merged) / before merging
		size_t instancesDrawn = 0, draws = 0, groupsDrawn = 0, groupsLoaded = 0;	// last frame; instances counted per drawn part (leaves count again)
		double cullMs = 0, uploadMs = 0;	// last frame CPU: cull + draw list, taking the cell results (the draw submission ms are FrameStats::foliage)
		size_t generated = 0;	// cells generated since start
		double genMs = 0, genMaxMs = 0;	// summed / slowest generateFoliage + bucketing, worker thread time
		size_t unresolved = 0;	// generated instances whose FOLIAGE_MESH has no drawable mesh
		size_t children = 0;	// resident instances that are children of a parent mesh (FoliageInstance::isChild)
		double contextMs = 0;
	};
	Stats stats() const;
	// kenshi-l5dx / kenshi-2wuu: the click / hover pick's foliage half. A foliage instance is a pick target only when it is interactable (user 2026-11-22: a tree's
	// cursor "should pass through"; a rock too): a deposit, decided once per record at load from the data (Context::pickTarget: `building type` -> a BUILDING that `is
	// foliage` with a producing functionality). Trees and rocks are never tested. The nearest DRAWN triangle of a resident target instance with t in [tMin, tMax]
	// along the ray (alpha cut-out honoured); one past its fade end is not drawn and does not pick. box: the hit instance's world box (the deposit match reads it).
	// Logic thread. False: nothing.
	bool pickRay(const double origin[3], const double dir[3], double tMin, double tMax, double& t, float box[6]) const;
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
		rq::Handle<sg_buffer> buf;	// the instance buffer, published by its resource job (valid: hasBuf)
		bool hasBuf = false;
		std::vector<float> cpu;	// the instance floats, as uploaded (groups index into it): the pick's drawn-triangle test reads them (pickRay)
	};
	void buildShadowList(const double cam[3], int level, ShadowList& list);
	struct ShadowCache { std::shared_ptr<const ShadowList> list; uint64_t key = 0; bool valid = false; };
	ShadowCache shadowCache_[kLevels];	// logic side: the level's last list and the key it was built under
	// A shadow list holds many candidates of one part (a candidate is one record in one cell): what resolveShadow fixes by part -- the shader mode, the
	// alpha cut, the list slots, the vertex uniforms with a zero base -- is resolved once per part per frame, and each candidate only fills in its
	// origin, fade and instances (kenshi-k16.2.205.12.119: foliage.prep-shadow was ~1.8 ms of the logic frame). The slots name one packet's DrawList,
	// so a template lives one prepare frame (the stamp).
	struct ShadowPart { uint64_t stamp = 0; bool ok = false; std::vector<WorldBuildings::ResolvedShadow> subs; };	// by part id
	std::vector<ShadowPart> shadowPart_;
	// The same idea for the draw list: a part's submeshes resolve once per prepare frame (both fade modes), and each group only fills in what resolveDraw
	// takes from the group -- its instances, origin and fade (ResolvedDraw::base/fade, kenshi-k16.2.205.12.119). subs[mode - 1][sub].
	struct DrawPart { uint64_t stamp = 0; std::vector<WorldBuildings::ResolvedDraw> subs[2]; };
	std::vector<DrawPart> drawPart_;
	// The last draw made for a part in prepare()'s draw list, to extend it with the next node that continues its instance range (see prepare).
	struct OpenRun { uint64_t stamp = 0; int cell = -1, draw = 0, next = 0; float fadeKey = 0; bool band = false; };
	std::vector<OpenRun> openPart_;	// by part id
	uint64_t shadowKey() const;
	void fadeParams(const Group& g, double meshRadius, float out[3]) const;	// class distance, radius x landmark factor, cap: an instance's end = min(max(x, y x scale), z) (kenshi-k16.2.205.12.124)
	double fadeEnd(const Group& g, double meshRadius, double scale) const;	// the end of an instance of that scale	// draw distance of a group: class distance x range, size limit, load radius, drawLimit
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
	std::unordered_map<int, int> bufSlot_;	// prepare(): cell -> FrameData::bufs slot
	uint64_t cellGen_ = 0;	// a cell was accepted or freed (FrameData::gen)
	std::unordered_map<int, sg_buffer> gpu_;	// render side: cell index -> its instance buffer (made and destroyed by resource jobs)
	Stats stats_;
	std::string report_;
	bool reported_ = false;
};
