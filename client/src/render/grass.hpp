#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "assets/dds.hpp"
#include "render/foliage.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "sokol_gfx.h"

// Grass layers (docs/CLIENT.md "Grass"): the game's PagedGeometry GrassLoader, one layer per GRASS record that a biome's FOLIAGE_LAYER lists in `grass`.
// Blades are placed per 256-unit tile around the camera on worker threads (count = density x 0.005 x area, accepted by the overlay map's grass channel,
// the GRASS altitude/slope limits and the biome under the point), uploaded as one static instance buffer per tile and drawn as instanced alpha-tested
// sprite quads. Per frame (render hook 7a, kenshi-k16.2.205.12.14): prepare() on the logic side (streaming, culling) fills a FrameData in the frame
// packet slot; upload() on the render side makes and frees the tile buffers it lists; render() inside the pass (after the terrain) draws its tile list.
// Neither upload() nor render() reads the logic-side tile table.
class Grass
{
public:
	Grass();
	~Grass();
	// dataDir: <install>/data. The game data, heightmap and biome map come from `foliage` once it is ready().
	void init(const ResourceIndex* res, const Foliage* foliage, const std::string& dataDir);
	void shutdown();
	bool ready() const { return ready_.load(); }	// layer table built, sprites decoded
	bool failed() const { return failed_.load(); }
	struct Inst		// 24 bytes, matches the vertex layout in grass.cpp
	{
		float x, y, z;		// tile-relative x/z, absolute y
		uint8_t r, g, b, a;	// colour map colour (a unused)
		uint16_t w, h;		// width, height in 1/1024 units
		uint16_t yaw, phase;	// 0..65535 of a turn
	};
	struct Range { int def, first, count; };
	struct TileCpu
	{
		int key = 0;
		std::vector<Inst> data;
		std::vector<Range> ranges;
		float minY = 0, maxY = 0;
		double ms = 0;
	};
	// One frame's grass draw, in the frame packet slot (double-buffered: one per packet): a pure snapshot. prepare() fills it, render() reads only it.
	// The one-shot GPU work (the pipeline, each tile's buffer create and free) goes through the resource queue (renderqueue.hpp, frame lane), never the packet.
	struct DrawRange { int def, first, count; double reach; };	// reach = the def's class range (fade to 0 at it)
	struct DrawTile
	{
		int key = 0;
		float base[3] = {};		// camera-relative tile origin
		double dist = 0;		// nearest distance from the camera
		int firstRange = 0, rangeCount = 0;	// into ranges
	};
	struct FrameData
	{
		bool ready = false;		// the layer table was ready (the first such prepare queues the pipeline and sprites)
		double cam[3] = {};
		std::vector<DrawTile> tiles;	// this frame's drawn tiles, nearest-first order of the tile table
		std::vector<DrawRange> ranges;
	};
	// Logic side. cam: world position. blocking (scripted runs): wait for every tile in range. Queues the new tiles' buffers and the freed ones.
	void prepare(const double cam[3], const float mvp[16], bool blocking, FrameData& out);
	// Render side.
	void render(const FrameData& d, const float mvp[16], const LightParams& light, double time);

	struct Stats
	{
		size_t tilesLoaded = 0, tilesPending = 0, bladesLoaded = 0;
		size_t bladesDrawn = 0, draws = 0, tilesDrawn = 0;	// last frame (blades counted per instance, not per quad)
		size_t layers = 0, biomes = 0;				// distinct GRASS records used, biomes that have grass
		size_t generated = 0;
		double genMs = 0, genMaxMs = 0;				// worker time per tile
	};
	Stats stats() const;	// the draw counters come from the FrameStats slot (the render side writes them)
	std::string report() const { return report_; }

	const Shadow* shadow = nullptr;
	// Draw distance = rangeScale x 10 x {50, 100, 800}[class] world units (the exe's table, 1406d3e30; the game ships rangeScale 1 = its `grass range` setting).
	// The class is the `visibility range` of the FOLIAGE_LAYER that lists the GRASS record (every shipped grass layer says 1 = 1000 units at scale 1).
	// The dithered fade covers each class's last 20 %. Tiles stay resident out to the largest class range, capped at kMaxReach.
	double rangeScale = 2.0;
	static constexpr double kMaxReach = 6000.0;
	bool paintMap = false;		// --grass-map paint: density from the exe's noise paint (its save-fixup rebuild) instead of the overlay R byte as loaded (docs/CLIENT.md "Grass")
	float density = 1.0f;		// the game's `grass density` setting (settings.cfg, default 1): multiplies every layer's blades per area
	static constexpr double kTile = 256.0;

private:
	struct Def;
	struct Context;
	struct Tile	// logic side: what the culling needs (the buffer itself is gpu_'s)
	{
		bool hasBuf = false;	// blades > 0: upload() makes a buffer for it
		std::vector<Range> ranges;
		size_t blades = 0;
		float minY = 0, maxY = 0;
		uint64_t lastUsed = 0;
	};

	void loader();
	void worker();
	bool take(bool wait);
	void accept(TileCpu& cpu);
	void makePipeline();
	double classRange(const Def& d) const;

	const ResourceIndex* res_ = nullptr;
	const Foliage* foliage_ = nullptr;
	std::string dataDir_;
	std::unique_ptr<Context> ctx_;
	std::thread loader_;
	std::vector<std::thread> threads_;
	std::atomic<bool> ready_{ false }, failed_{ false };
	std::mutex mu_;
	std::condition_variable jobCv_, resCv_;
	std::deque<int> jobs_;
	std::deque<TileCpu> results_;
	std::atomic<int> outstanding_{ 0 };
	bool quit_ = false;

	std::unordered_map<int, Tile> tiles_;
	std::unordered_set<int> queued_;
	uint64_t frame_ = 0;
	bool pipelineQueued_ = false;
	// render side
	std::unordered_map<int, sg_buffer> gpu_;	// tile key -> its instance buffer (the queued jobs make and free them)
	bool texturesMade_ = false;
	std::vector<sg_view> views_;	// per def: sprite texture
	std::vector<sg_image> images_;
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_buffer corners_ = {}, indices_ = {};
	sg_sampler smp_ = {};
	Stats stats_;
	std::string report_;
	bool reported_ = false;
};
