#pragma once

// GPU time per pass (GL_TIME_ELAPSED queries, GLCORE only; a no-op on other backends). Queries are triple-buffered and read two frames late, so
// nothing stalls. Sections must not nest or overlap (one TIME_ELAPSED query can be active at a time).
// KCLIENT_PROFILE (CMake, default ON; kenshi-k16.2.101.1): OFF compiles every query, every counter and every call site out (GPU_BEGIN / GPU_END
// become an empty statement; the timer has no methods at all, so an ungated call does not compile).
#ifndef KCLIENT_PROFILE
#define KCLIENT_PROFILE 1
#endif

class GpuTimer
{
public:
	enum Section { Shadow, Sky, Terrain, Buildings, Foliage, Grass, Water, Overlay, Exposure, Fog, Mask, Particles, Ao, Post, Reflection,
		Characters, CharacterShadows, Items, Ui, Shafts, kSections };	// the last four: kenshi-k16.2.101.1 (the existing ones keep their names)
#if KCLIENT_PROFILE
	static const char* name(int s);
	void init();
	void beginFrame();	// rotate the query set and collect the oldest set's results
	void begin(Section s);	// also makes s the bucket of the per-pass submit counters (drawstats)
	void end();
	void shutdown();
	bool ok() const { return ok_; }
	double ms(int s) const { return ms_[s]; }	// latest collected value
	double totalMs() const;

private:
	static constexpr int kRing = 3;
	bool ok_ = false;
	int frame_ = 0, active_ = -1;
	unsigned q_[kRing][kSections] = {};
	bool used_[kRing][kSections] = {};
	double ms_[kSections] = {};
#endif
};

#if KCLIENT_PROFILE
#define GPU_INIT(timer) (timer).init()
#define GPU_SHUTDOWN(timer) (timer).shutdown()
#define GPU_BEGIN_FRAME(timer) (timer).beginFrame()
#define GPU_BEGIN(timer, section) (timer).begin(section)
#define GPU_END(timer) (timer).end()
#define GPU_OK(timer) (timer).ok()
#define GPU_MS(timer, section) (timer).ms(section)
#define GPU_TOTAL(timer) (timer).totalMs()
#else
#define GPU_INIT(timer) ((void)0)
#define GPU_SHUTDOWN(timer) ((void)0)
#define GPU_BEGIN_FRAME(timer) ((void)0)
#define GPU_BEGIN(timer, section) ((void)0)
#define GPU_END(timer) ((void)0)
#define GPU_OK(timer) false
#define GPU_MS(timer, section) 0.0
#define GPU_TOTAL(timer) 0.0
#endif
