#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "characters.hpp"
#include "sky.hpp"
#include "sokol_gfx.h"

// Character portraits (docs/CLIENT.md "Portraits"; the original's RTT_Portrait 14084b890): a character's head, rendered once into a 128 x 128 cell of
// one 2048 x 2048 atlas (`portraits_texture`, 140414380) and kept until its appearance changes. The view is the character's own: its portrait_serial
// turns the camera (serialYaw / serialPitch). Each portrait renders into a 128 x 128 HDR scratch (the character pipelines' formats), then a copy
// pass writes the cell.
// Lighting is fixed (kinspect item 9, findings/portrait9_static.md + portrait9_live.md): the portrait scene has no lights, ambient, fog or shadows;
// its Deferred_RTT_Lighting quad (rtt.hlsl) lights with the shipped deferred.material constants and the static IBL, so a portrait is the same at noon
// and at night. A changed appearance re-renders after queuePortrait's 1 s timer (140787550), re-armed by every change. MINE: the oldest-used cell is
// reused when all 256 are taken.
// The hand-off between the threads (render thread hook, kenshi-k16.2.205.12.56): the bookkeeping and the draw are two objects.
//   PortraitTable (logic thread): which character has which cell, what is stale, the queue, the uv the UI shows (get). The UI (HUD, squads,
//     dialogue) reads only this table, never the atlas object.
//   Portraits (render thread): the atlas, the scratch targets and the draw (update). It draws the jobs the packet carries
//     (FramePacket::portraitJobs, PortraitTable::send at the packet's fill) and posts what became of each to a mailbox (one mutex) that logic
//     takes at its next frame start (PortraitTable::apply(Portraits::take())). One thread: logic frame N queues, render frame N draws, logic
//     frame N+1 sees it ready: the same frame as when get() and update() shared one object.
// No sokol handle crosses per frame: the UI's PortraitRef carries the uv rect and App::portraitsTex, the drawlist id bound to the atlas once at
// Portraits::init (before the logic thread starts) and unbound at shutdown (after it is joined); the atlas view never changes in between, and
// the pixels are only sampled by the render thread's UI replay.
// Draw `key` (Characters index `index`) into `cell`. PortraitTable::send fills the first four; Portraits::pose (logic, after send) the rest: the
// pose and its frame data are built in the logic half and carried in the packet (kenshi-k16.2.205.12.67), so the render side reads no character.
struct PortraitJob
{
	uint64_t key = 0;
	int index = -1;
	uint32_t serial = 0;
	int cell = -1;
	bool run = false;	// within the frame's portrait buffer sets (Characters::kMaxPortraits posed jobs); false: kNotRun
	bool posed = false;	// Characters::portraitFrame placed it; false: kNotPlaced
	double target[3] = {}, eye[3] = {};	// the camera (world)
	Characters::FrameData frame;	// the posed copy's frame data, camera-relative to eye
};
struct PortraitDone
{
	enum Result : uint8_t { kDrawn, kNotPlaced, kNotRun };	// drawn; portraitPose had no pose yet (ask again); the frame's budget ran out (queue again)
	uint64_t key = 0;
	int cell = -1;
	Result result = kDrawn;
};

// Logic thread only (owner: logic_frame and the UI it builds).
class PortraitTable
{
public:
	// The atlas cell of `key` (a sim character id) as u0, v0, u1, v1 (v0 the top row). A missing portrait is queued from Characters index `index`
	// at once, a stale one (version or serial changed) 1 s after its last change; the old image shows meanwhile. False until first rendered.
	bool get(uint64_t key, int index, uint32_t version, uint32_t serial, float uv[4]);
	// At the packet's fill (kWorld with a live sim): up to `budget` queued jobs into `out` (the original: one per frame), each with its cell.
	void send(std::vector<PortraitJob>& out, int budget);
	// At logic's frame start: what the render thread did with the jobs sent before (Portraits::take).
	void apply(const std::vector<PortraitDone>& done);
	void setOrigin(bool topLeft) { topLeft_ = topLeft; }	// sokol's features.origin_top_left, read once at Portraits::init (the uv's row order)
	void clear();

private:
	struct Cell { int cell = -1; uint32_t version = 0, serial = 0; bool ready = false, dirty = true; uint64_t used = 0; double due = 0; };	// due: when a dirty one may queue
	struct Job { uint64_t key; int index; };
	std::unordered_map<uint64_t, Cell> cells_;
	std::deque<Job> queue_;
	uint64_t frame_ = 0;	// the sends so far (the LRU clock)
	int next_ = 0;	// cells handed out so far (up to kCells)
	bool topLeft_ = false;

	int takeCell();
};

// Render thread only (owner: render_frame's worldSetup), but for take(), which logic calls under the mailbox's mutex.
class Portraits
{
public:
	static constexpr int kCell = 128, kAtlas = 2048, kPerRow = kAtlas / kCell, kCells = kPerRow * kPerRow;
	static constexpr float kFovY = 25, kNear = 0.1f, kFar = 400;	// FUN_140845f30: 25 deg, near 0.1, far 400 (aspect 1)

	void init();
	void shutdown();

	// Logic half, after PortraitTable::send and Characters::update: each job's pose and frame data (Characters::portraitFrame), the first
	// Characters::kMaxPortraits posed ones marked to run (the buffer sets one frame has). No sokol call.
	static void pose(Characters& chars, std::vector<PortraitJob>& jobs);
	// Render side: renders the packet's posed jobs in order (outside any pass, after Characters::upload) from their frame data, while
	// chars.portraitReady() allows; each one's result goes to the mailbox. Reads no character state.
	void update(Characters& chars, const std::vector<PortraitJob>& jobs);
	std::vector<PortraitDone> take();	// logic thread: the results posted since the last take
	sg_view atlas() const { return atlasTex_; }
	sg_sampler sampler() const { return smp_; }

	// 1406223f0: the serial is rand x 4e9 at creation; the camera turns by these (radians)
	static float serialYaw(uint32_t s) { return ((float)(s % 500) / 1000.0f - 0.25f) * 3.14159265f; }
	static float serialPitch(uint32_t s) { return ((float)(s % 252) / 1000.0f - 0.083f) * 3.14159265f; }

private:
	LightParams light_ = {};	// the fixed portrait light (init)
	bool atlasCleared_ = false;	// the first copy clears the whole atlas
	sg_image atlas_ = {}, scratch_ = {}, depth_ = {};
	sg_view atlasTex_ = {}, atlasAtt_ = {}, scratchTex_ = {}, scratchAtt_ = {}, depthAtt_ = {};
	sg_sampler smp_ = {};
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	std::mutex doneMtx_;	// the mailbox: render appends in update, logic swaps it out in take
	std::vector<PortraitDone> done_;
};
