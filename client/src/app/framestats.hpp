#pragma once

// The frame's stats return slot (render hook 5, kenshi-k16.2.205.12.12; docs HOOKS.md 5). The render side (the pass recorder) fills its own
// slot during the frame and publishes a copy under a mutex when the frame's last pass has ended; logic takes that copy once at its frame start
// (as view::take does) and reads only the copy (kenshi-k16.2.205.12.24, review kenshi-k16.2.205.12.15 section 5). The copy is stamped with the
// render frame number and the packet serial it drew, so a reader can pair it with GPULOG and the scene JSON; it is one frame old, which every
// logic reader allows (the cursor hit already was a frame or two old). The HUD, KCLIENT_GPULOG, the scene JSON writer, the stage report and
// `dump gpu` / `dump peek` never read GpuTimer / drawstats:: / the id buffer directly. One thread today: logic's frame N+1 starts right after
// render frame N ended, so the copy holds exactly what the slot held when the logic readers read it before.
//   render side (render_frame and what it calls):
//     fillGpu(): at the frame start, right after GPU_BEGIN_FRAME (the collected ms are fixed from there) - also the applied size.
//     addGrass(): Grass::render's counters.
//     publishCursor(): from the id pass's readback (a frame or two late, as before).
//     fillCpu(): the world frame's CPU ms (logic's half from the packet plus the draw's), before publish (kenshi-k16.2.205.12.27).
//     fogDrawn: the fog volumes the world frame drew (FogVolumes::render).
//     lightVols: worldSetup's light volume counts (the volumes, those with a sun bounce), at lightVolumes.begin.
//     fillDraw(): when the frame's last pass has ended (before sg_commit; the per-pass counters are complete).
//     publish(packet): right after fillDraw: the logic copy's source.
//     fillRenderMs(): after render_frame returns (lockstep): the draw half's smoothed ms, into the slot and the published copy.
//     endFrame(): after sg_commit on every path: the count of frames drawn (App::frames before kenshi-k16.2.205.12.27) into the slot and the
//                 published copy (logic mirrors it into app.frames at its frame start).
//     drawing(): the render side's own readers of this frame's slot (the HUD, GPULOG).
//   logic side: take() at logic_frame's start, then stats().
#include "render/drawstats.hpp"
#include "render/gputimer.hpp"
#include "render/idbuffer.hpp"

#include <cstdint>
#include <mutex>

struct FrameStats
{
	bool gpuOk = false;	// the timer works (GLCORE, KCLIENT_PROFILE)
	double gpuMs[GpuTimer::kSections] = {};	// per-section GPU ms, as GpuTimer::ms
	double gpuTotal = 0;
	drawstats::Pass passes[GpuTimer::kSections] = {};	// per-pass submit counters (sgl batches counted at the UI's sgl_end sites)
	IdBuffer::Hit cursorHit;	// the id pass's last result (a frame or two old)
	struct { size_t bladesDrawn = 0, draws = 0, tilesDrawn = 0; } grass;	// Grass::render's draw counters (render hook 7a1; summed over the frame's grass renders, reset at fillGpu)
	struct { double renderMs = 0, shadowMs = 0; size_t shadowDraws = 0; } foliage;	// Foliage::render + renderFadeBand draw submission ms; renderShadow ms and submesh draws over all cascades (render hook 7b; reset at fillGpu)
	// The world frame's CPU (review kenshi-k16.2.205.12.15 3.1, kenshi-k16.2.205.12.27): lastMs = the packet's logic ms + the draw's ms up to the
	// overlay's end; ms smoothed (0.9 / 0.1, frames of 250 ms or more are blocking loads and skipped). Not reset at fillGpu: it carries over.
	struct { double ms = 0, lastMs = 0; } cpu;
	double renderMs = 0;	// lockstep: the draw half's CPU ms (render_frame, world frames), smoothed 0.9 / 0.1; was App::renderMs, written by the render thread and read by the HUD (kenshi-k16.2.205.12.94)
	double shadowMs = 0;	// the shadow pass's CPU ms this frame (the HUD's; set by worldSetup, sitting 9)
	int fogDrawn = 0;	// fog volumes drawn by the last world frame
	// The light volumes worldSetup built for the scene pass (kenshi-k16.2.205.12.56): the render thread owns the list (RenderState::lightVols); the
	// dev ops (`dump gpu`) read these counts, never the list. Set at every lightVolumes.begin.
	struct { size_t volumes = 0, sunVolumes = 0; } lightVols;
	int frames = 0;	// every frame the render side ended (all paths), as App::frames counted them; endFrame
	int appliedW = 0, appliedH = 0;	// the size the frame renders at (windowgate), which can differ from the window while a change is held
	uint64_t renderFrame = 0;	// the stamp (publish): the count of world frames the render side drew, 1 for the first
	uint64_t packet = 0;	// the stamp (publish): the serial of the packet that frame drew (FramePacket::serial)
	// sokol_gl after the frame's draws (render hook 8, kenshi-k16.2.205.12.28): main.cpp sgl_check's flags per context (1 vertices full,
	// 2 commands full, 4 uniforms full, 8 matrix stack, 16 no context) and the 2D draw list's last replay (drawlist_gpu.hpp Stats).
	struct
	{
		uint32_t flags[2] = {};	// the default context, the trails' context
		uint32_t uiDraws = 0, uiVertices = 0, uiMissing = 0, uiTextures = 0;
	} sgl;
};

namespace framestats
{
namespace detail
{
	inline FrameStats& slot() { static FrameStats s; return s; }	// the render side's own, written during the frame
	inline FrameStats& published() { static FrameStats s; return s; }	// under mtx(): the last finished frame's copy
	inline FrameStats& logicCopy() { static FrameStats s; return s; }	// logic's, taken at its frame start
	inline std::mutex& mtx() { static std::mutex m; return m; }
	inline uint64_t& renderFrames() { static uint64_t n = 0; return n; }
}
inline FrameStats& slot() { return detail::slot(); }	// the render side's writable view
inline const FrameStats& drawing() { return detail::slot(); }	// the render side's readers: this frame's slot as filled so far
inline const FrameStats& stats() { return detail::logicCopy(); }	// the logic side's readers: the copy take() made

// Render side, when the frame's last pass has ended (after fillDraw): the copy logic takes next.
inline void publish(uint64_t packet)
{
	FrameStats& s = detail::slot();
	s.renderFrame = ++detail::renderFrames();
	s.packet = packet;
	std::lock_guard<std::mutex> lock(detail::mtx());
	detail::published() = s;
}
// Logic side, once at logic_frame's start: the last published frame becomes stats().
inline void take()
{
	std::lock_guard<std::mutex> lock(detail::mtx());
	detail::logicCopy() = detail::published();
}

inline void fillGpu(const GpuTimer& t, int appliedW, int appliedH)
{
	FrameStats& s = slot();
	s.gpuOk = GPU_OK(t);
	s.gpuTotal = GPU_TOTAL(t);
	for (int i = 0; i < GpuTimer::kSections; i++) { s.gpuMs[i] = GPU_MS(t, i); }
	s.appliedW = appliedW;
	s.appliedH = appliedH;
	s.grass = {};
	s.foliage = {};
}
inline void addFoliage(double renderMs, double shadowMs, size_t shadowDraws)
{
	FrameStats& s = slot();
	s.foliage.renderMs += renderMs; s.foliage.shadowMs += shadowMs; s.foliage.shadowDraws += shadowDraws;
}
inline void addGrass(size_t blades, size_t draws, size_t tiles)
{
	FrameStats& s = slot();
	s.grass.bladesDrawn += blades; s.grass.draws += draws; s.grass.tilesDrawn += tiles;
}
inline void fillCpu(double ms)
{
	auto& c = slot().cpu;
	if (ms < 250) { c.ms = c.ms == 0 ? ms : c.ms * 0.9 + ms * 0.1; }	// a blocking load is not a frame
	c.lastMs = ms;
}
// Render side, after sg_commit on every path: one more frame drawn. The published copy gets the count at once (the world path published its
// stats before the commit; the other paths publish nothing else).
inline void endFrame()
{
	const int n = ++slot().frames;
	std::lock_guard<std::mutex> lock(detail::mtx());
	detail::published().frames = n;
}
// Render side, after render_frame returns (the stats were published inside it): the draw half's ms, into the slot and the published copy.
inline void fillRenderMs(double ms)
{
	FrameStats& s = slot();
	if (ms < 250) { s.renderMs = s.renderMs == 0 ? ms : s.renderMs * 0.9 + ms * 0.1; }
	std::lock_guard<std::mutex> lock(detail::mtx());
	detail::published().renderMs = s.renderMs;
}
inline void fillDraw()
{
#if KCLIENT_PROFILE
	FrameStats& s = slot();
	for (int i = 0; i < GpuTimer::kSections; i++) { s.passes[i] = drawstats::passes[i]; }
#endif
}
inline void fillSgl(uint32_t defaultFlags, uint32_t trailsFlags, uint32_t uiDraws, uint32_t uiVertices, uint32_t uiMissing, uint32_t uiTextures)
{
	auto& g = slot().sgl;
	g.flags[0] = defaultFlags;
	g.flags[1] = trailsFlags;
	g.uiDraws = uiDraws;
	g.uiVertices = uiVertices;
	g.uiMissing = uiMissing;
	g.uiTextures = uiTextures;
}
inline void publishCursor(const IdBuffer::Hit& h) { slot().cursorHit = h; }
}	// namespace framestats
