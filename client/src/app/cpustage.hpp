#pragma once
// Per-frame main-thread CPU stage timers (kenshi-k16.2.205.1), dev-only (KCLIENT_DEV_TOOLS). CPUSTAGE_BEGIN() at the top of frame(), CPUSTAGE("name")
// after each stage: the stage's time is the wall time since the previous mark. CPUSTAGE_END() after sg_commit prints one line per frame to stderr
// when KCLIENT_STAGES=<ms> is set (frames whose total or gap is at least <ms>; 0 = every frame):
//   stages: frame N gap G total T (cpu C) | name ms | name ms ...
// gap: wall time from the previous CPUSTAGE_END to this frame's CPUSTAGE_BEGIN: the swap/present, the vsync or compositor wait and sokol's event
// handling, outside frame(). total: BEGIN to END. cpu: the HUD's cpu field (frame() up to the last pass end).
#ifdef KCLIENT_DEV_TOOLS
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cpustage
{
	// One half's marks. thread_local: under render.thread=lockstep the logic half marks on the logic thread and the draw half on the sapp thread, so
	// neither writes the other's vectors (kenshi-k16.2.205.12.94, overlap review 1.3 / A5). Off: both halves run on one thread and share one Half.
	struct Half
	{
		std::chrono::steady_clock::time_point last;
		std::vector<std::pair<const char*, double>> marks;
		std::vector<std::pair<const char*, double>> accs;	// accumulated sub-stage times (many short calls in one frame): name -> total ms, count in `counts`
		std::vector<long> counts;
		std::string extra;	// free-form tail for the line (kenshi-k16.2.205.7: the GPU sections and submit counters, KCLIENT_GPULOG)
		void clear() { marks.clear(); accs.clear(); counts.clear(); extra.clear(); }
	};
	inline Half& half() { static thread_local Half h; return h; }
	// What the two threads share: the logic half's finished marks (handed over at halfEnd, under the mutex), the frame number and the previous end.
	struct Shared
	{
		std::mutex mtx;
		Half pending;	// the logic half, published by halfEnd, taken by end
		std::chrono::steady_clock::time_point begin;	// with `pending`
		double gapMs = 0;	// with `pending`
		bool have = false;
		std::atomic<long long> prevEndNs{ 0 };	// steady_clock ticks of the last end(); 0 = none yet
		std::atomic<long> frame{ 0 };
		std::atomic<double> threshold{ -2 };	// -2: not read yet; -1: off
	};
	inline Shared& shared() { static Shared s; return s; }
	inline bool on()
	{
		Shared& s = shared();
		double t = s.threshold.load(std::memory_order_relaxed);
		if (t == -2) { const char* e = getenv("KCLIENT_STAGES"); t = e ? atof(e) : -1; s.threshold.store(t, std::memory_order_relaxed); }
		return t >= 0;
	}
	// Top of the logic half (frame() / logic_frame): clears this thread's marks and starts the frame's clock.
	inline void begin()
	{
		if (!on()) { return; }
		Shared& s = shared();
		Half& h = half();
		const auto now = std::chrono::steady_clock::now();
		h.clear();
		h.last = now;
		const long long pe = s.prevEndNs.load(std::memory_order_relaxed);
		const double gap = pe ? std::chrono::duration<double, std::milli>(now.time_since_epoch() - std::chrono::steady_clock::duration(pe)).count() : 0;
		std::lock_guard<std::mutex> lock(s.mtx);
		s.begin = now;
		s.gapMs = gap;
	}
	// The logic half is over (logic_frame's end, world path): its marks move to the shared hand-off for end() to merge.
	inline void halfEnd()
	{
		if (!on()) { return; }
		Shared& s = shared();
		Half& h = half();
		std::lock_guard<std::mutex> lock(s.mtx);
		std::swap(s.pending, h);
		s.have = true;
		h.clear();
	}
	// Top of the draw half on the thread that draws (before the packet's split probe): a fresh Half for it. Nothing of the logic half is in it.
	inline void renderBegin()
	{
		if (!on()) { return; }
		Half& h = half();
		h.clear();
		h.last = std::chrono::steady_clock::now();
	}
	inline void addAcc(Half& h, const char* name, double ms, long n)
	{
		for (size_t i = 0; i < h.accs.size(); i++) { if (h.accs[i].first == name) { h.accs[i].second += ms; h.counts[i] += n; return; } }
		h.accs.push_back({ name, ms });
		h.counts.push_back(n);
	}
	// A scope that adds its wall time to the named accumulator (names are string literals compared by pointer): for a call made once per character.
	struct Acc
	{
		const char* name;
		std::chrono::steady_clock::time_point t0;
		explicit Acc(const char* n) : name(n), t0(on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point()) {}
		~Acc()
		{
			if (!on()) { return; }
			addAcc(half(), name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), 1);
		}
	};
	// A parallel pass's workers (WorkPool, kenshi-k16.2.205.12.102): each worker hands its thread's accumulators to the shared drop at its run's
	// end (mergeToCaller, the pool's epilogue); the caller then adds them to its own half (mergedFromWorkers), so +pose counts every thread's time.
	inline std::mutex& workerMtx() { static std::mutex m; return m; }
	inline Half& workerDrop() { static Half h; return h; }
	inline void mergeToCaller()
	{
		if (!on()) { return; }
		Half& h = half();
		std::lock_guard<std::mutex> lock(workerMtx());
		for (size_t i = 0; i < h.accs.size(); i++) { addAcc(workerDrop(), h.accs[i].first, h.accs[i].second, h.counts[i]); }
		h.clear();
	}
	inline void mergedFromWorkers()
	{
		if (!on()) { return; }
		Half& h = half();
		std::lock_guard<std::mutex> lock(workerMtx());
		Half& d = workerDrop();
		for (size_t i = 0; i < d.accs.size(); i++) { addAcc(h, d.accs[i].first, d.accs[i].second, d.counts[i]); }
		d.clear();
	}
	// Tail text for this frame's line (the GPU sections and submit counters, kenshi-k16.2.205.7); only with KCLIENT_STAGES on.
	inline void extra(const std::string& s)
	{
		if (!on()) { return; }
		half().extra += s;
	}
	inline void mark(const char* name)
	{
		if (!on()) { return; }
		Half& h = half();
		const auto now = std::chrono::steady_clock::now();
		h.marks.push_back({ name, std::chrono::duration<double, std::milli>(now - h.last).count() });
		h.last = now;
	}
	// cpuMs: the frame's own CPU figure (the HUD's). Early-returning frames (loading, title) never reach the end: their marks are dropped at the next begin.
	// Runs on the drawing thread: the logic half's marks (halfEnd's hand-off) come first on the line, then this thread's.
	inline void end(double cpuMs)
	{
		if (!on()) { return; }
		Shared& s = shared();
		Half& h = half();
		const auto now = std::chrono::steady_clock::now();
		Half logic;
		std::chrono::steady_clock::time_point begin = now;
		double gapMs = 0;
		{
			std::lock_guard<std::mutex> lock(s.mtx);
			if (s.have) { std::swap(logic, s.pending); begin = s.begin; gapMs = s.gapMs; s.have = false; }
		}
		const double total = std::chrono::duration<double, std::milli>(now - begin).count();
		s.prevEndNs.store(now.time_since_epoch().count(), std::memory_order_relaxed);
		const long frame = s.frame.fetch_add(1) + 1;
		const double threshold = s.threshold.load(std::memory_order_relaxed);
		if (total < threshold && gapMs < threshold) { h.clear(); return; }
		for (size_t i = 0; i < h.accs.size(); i++) { addAcc(logic, h.accs[i].first, h.accs[i].second, h.counts[i]); }
		std::string line;
		char buf[96];
		for (const auto& [n, ms] : logic.marks) { snprintf(buf, sizeof buf, " | %s %.2f", n, ms); line += buf; }
		for (const auto& [n, ms] : h.marks) { snprintf(buf, sizeof buf, " | %s %.2f", n, ms); line += buf; }
		for (size_t i = 0; i < logic.accs.size(); i++) { snprintf(buf, sizeof buf, " | +%s %.2f x%ld", logic.accs[i].first, logic.accs[i].second, logic.counts[i]); line += buf; }
		fprintf(stderr, "stages: frame %ld gap %.2f total %.2f (cpu %.2f)%s%s%s\n", frame, gapMs, total, cpuMs, line.c_str(), logic.extra.c_str(), h.extra.c_str());
		h.clear();
	}
}
#define CPUSTAGE_BEGIN() cpustage::begin()
#define CPUSTAGE(name) cpustage::mark(name)
#define CPUSTAGE_END(cpu) cpustage::end(cpu)
#define CPUSTAGE_HALF_END() cpustage::halfEnd()
#define CPUSTAGE_RENDER_BEGIN() cpustage::renderBegin()
#define CPUSTAGE_MERGE_TO_CALLER() cpustage::mergeToCaller()
#define CPUSTAGE_MERGED_FROM_WORKERS() cpustage::mergedFromWorkers()
#define CPUSTAGE_CAT2_(a, b) a##b
#define CPUSTAGE_CAT_(a, b) CPUSTAGE_CAT2_(a, b)
#define CPUSTAGE_ACC(name) cpustage::Acc CPUSTAGE_CAT_(cpustageAcc_, __LINE__)(name)	/* unique per line: two in one scope */
#define CPUSTAGE_DECL(opt) std::optional<cpustage::Acc> opt
#define CPUSTAGE_OPT(opt, name) (opt.reset(), opt.emplace(name))	/* a std::optional<cpustage::Acc>: ends the previous sub-stage, starts the next */
/* an event line with the number the frame's stages line will carry ("note: frame N ..."), only with KCLIENT_STAGES on (kenshi-k16.2.205.12.77) */
#define CPUSTAGE_NOTE(...) (cpustage::on() ? (void)(fprintf(stderr, "note: frame %ld ", cpustage::shared().frame.load() + 1), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)) : (void)0)
#else
#define CPUSTAGE_BEGIN() ((void)0)
#define CPUSTAGE(name) ((void)0)
#define CPUSTAGE_END(cpu) ((void)0)
#define CPUSTAGE_HALF_END() ((void)0)
#define CPUSTAGE_RENDER_BEGIN() ((void)0)
#define CPUSTAGE_MERGE_TO_CALLER() ((void)0)
#define CPUSTAGE_MERGED_FROM_WORKERS() ((void)0)
#define CPUSTAGE_ACC(name) ((void)0)
#define CPUSTAGE_DECL(opt) ((void)0)
#define CPUSTAGE_OPT(opt, name) ((void)0)
#define CPUSTAGE_NOTE(...) ((void)0)
#endif
