#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// The render-side resource job queue (docs/RENDER_THREAD.md §7.2; design review 2.2, kenshi-k16.2.205.12.22).
//
// The logic half never calls sg_make_*, sg_alloc_* or sg_destroy_* (sokol's pools are unsynchronised; the thread-owner assert,
// dev/sgowner.cpp, aborts on it). It owns the CPU data (pixels, vertices, a decal list) and a pending state, and queues a job that
// carries the data by value. The render thread runs the jobs at the top of render_frame, before the first pass, and publishes what it
// made: into its own render-side members (the draw reads them), or into a Handle that logic polls. Destroys and one-shot GPU work
// (a decal atlas repaint) go through the same queue, so a packet stays a pure snapshot (§7.1 rule 2): drawing it twice or skipping it
// cannot lose a create or a free.
//
// Two lanes, each in submit order:
// - kFrame: every job queued for packet N runs before packet N is drawn, all of them, whatever they cost. The packet may name what they
//   make. On one thread (render.thread off) logic_frame queues and render_frame drains in the same frame(), so the order of GPU work is
//   today's and the pictures are unchanged.
// - kStream: under a per-frame byte budget and a time budget (at least one job per frame), for streaming (kenshi-k16.2.258: buildings, characters,
//   foliage). What it makes is published through a Handle; logic names it in a packet only once the Handle is ready().
// Both lanes run a job only on the draw of the packet it was queued for or a later one (its serial), so a redraw of packet N (§7.1 rule 4) never
// runs a destroy that logic queued for N+1, when N+1 stopped naming the resource (kenshi-k16.2.205.12.35).
// A resource's create and its destroy go in the same lane (the lanes are not ordered against each other).
//
// Jobs capture app-lifetime objects (a renderer) by pointer and their data by value. A job may queue another; it runs on a later drain.
namespace rq
{
	enum class Lane { kFrame, kStream };

	// The stream lane's order (kenshi-k16.2.258.3): a job with a Prio runs in the order of its Prio's value, lowest first (a resource's jobs share one Prio,
	// so its slices keep their push order); logic rewrites `want` as the camera moves, a drain reads it once (snap) so one drain sees one value per
	// resource. A job without a Prio runs first, in submit order (characters, frees). Ties keep submit order.
	struct Prio
	{
		std::atomic<float> want{ 1e30f };	// logic side: lower = sooner
		float snap = 0;				// render side: `want` as this drain first read it
		uint64_t stamp = 0;			// render side: the drain that took snap
	};
	using PrioPtr = std::shared_ptr<Prio>;

	// Logic side (any thread but the render thread's draw; on one thread, anywhere). tag: a static string for the stats and the dev log.
	void push(const char* tag, std::function<void()> job, Lane lane = Lane::kFrame, size_t bytes = 0, PrioPtr prio = nullptr);
	// logic_frame's start: the packet being filled from now on. Returns its serial (FramePacket::serial); jobs queued until the next call
	// belong to it. Jobs queued before the first call belong to serial 0.
	uint64_t beginPacket();

	// Render side. The top of render_frame: the frame lane's jobs up to `serial` (the packet about to be drawn), then the stream lane's
	// up to `serial` under the budget. A redraw of the same serial finds no frame job left.
	void drain(uint64_t serial);
	// cleanup(), after the logic thread is joined and before sg_shutdown: every job left, both lanes (the shutdown's destroys).
	void flushAll();
	void setStreamBudget(size_t bytesPerFrame);	// default 8 MiB
	// The stream lane's time per drain, ms (default 4; 0 = bytes only, the old lane): a job whose predicted cost (its bytes at the measured create
	// rate) would take the lane past it waits for the next drain; the first job of a drain always runs (kenshi-k16.2.205.12.74)
	void setStreamTimeBudget(double ms);
	size_t pendingStreamBytes();	// the bytes of the stream jobs queued now

	struct Stats
	{
		uint64_t frameJobs = 0, streamJobs = 0;	// run so far
		uint64_t streamBytes = 0;		// the stream lane's bytes run so far
		uint64_t deferredFrames = 0;		// drains that left stream jobs for the next frame
		size_t pendingFrame = 0, pendingStream = 0;	// queued now
	};
	Stats stats();

	// What a stream job made, published to logic. The job (render side) calls publish(); logic tests ready() and only then names get()
	// in a packet. Copyable: the logic side keeps one, the job captures another.
	template <class T> class Handle
	{
	public:
		Handle() : s_(std::make_shared<State>()) {}
		bool ready() const { return s_->ready.load(std::memory_order_acquire); }
		const T& get() const { return s_->value; }	// render side, or logic after ready()
		void publish(const T& v) const	// render side, inside the job
		{
			s_->value = v;
			s_->ready.store(true, std::memory_order_release);
		}

	private:
		struct State
		{
			T value{};
			std::atomic<bool> ready{ false };
		};
		std::shared_ptr<State> s_;
	};
}
