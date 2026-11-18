#include "gputimer.hpp"
#include "drawstats.hpp"

#if KCLIENT_PROFILE && defined(SOKOL_GLCORE)
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#if KCLIENT_PROFILE
const char* GpuTimer::name(int s)
{
	static const char* n[kSections] = { "shadow", "sky", "terrain", "buildings", "foliage", "grass", "water", "overlay", "exposure", "fog", "mask", "particles", "ao", "post", "reflection",
		"characters", "character_shadows", "items", "ui", "shafts" };
	return n[s];
}

double GpuTimer::totalMs() const
{
	double t = 0;
	for (double v : ms_) { t += v; }
	return t;
}
#endif

#if KCLIENT_PROFILE && defined(SOKOL_GLCORE)
void GpuTimer::init()
{
	for (auto& set : q_) { glGenQueries(kSections, set); }
	ok_ = glGetError() == GL_NO_ERROR;
}

void GpuTimer::beginFrame()
{
	if (!ok_) { return; }
	frame_ = (frame_ + 1) % kRing;
	for (int s = 0; s < kSections; s++)
	{
		if (!used_[frame_][s]) { continue; }
		GLuint avail = 0;
		glGetQueryObjectuiv(q_[frame_][s], GL_QUERY_RESULT_AVAILABLE, &avail);
		if (!avail) { continue; }
		GLuint64 ns = 0;
		glGetQueryObjectui64v(q_[frame_][s], GL_QUERY_RESULT, &ns);
		ms_[s] = (double)ns * 1e-6;
		used_[frame_][s] = false;
	}
	for (bool& u : used_[frame_]) { u = false; }
}

void GpuTimer::begin(Section s)
{
	if (!ok_ || active_ >= 0) { return; }
	glBeginQuery(GL_TIME_ELAPSED, q_[frame_][s]);
	active_ = s;
	drawstats::section(s);
}

void GpuTimer::end()
{
	if (!ok_ || active_ < 0) { return; }
	glEndQuery(GL_TIME_ELAPSED);
	used_[frame_][active_] = true;
	active_ = -1;
	drawstats::section(-1);
}

void GpuTimer::shutdown()
{
	if (!ok_) { return; }
	for (auto& set : q_) { glDeleteQueries(kSections, set); }
	ok_ = false;
}
#elif KCLIENT_PROFILE
// profiling on, but not GLCORE: the sections still bucket the submit counters, with no GPU timer
void GpuTimer::init() {}
void GpuTimer::beginFrame() {}
void GpuTimer::begin(Section s) { drawstats::section(s); }
void GpuTimer::end() { drawstats::section(-1); }
void GpuTimer::shutdown() {}
#endif
