// The last composed frame, kept render side (lastframe.hpp; kenshi-k16.2.205.12.25).
#include "frame/lastframe.hpp"
#include "assets/imagewrite.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "render/readback.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"

#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"
#endif

namespace lastframe
{
namespace
{
#if defined(SOKOL_GLCORE)
GLuint gFbo = 0, gRb = 0;
int gW = 0, gH = 0;
bool gKept = false;
#endif
int gShots = 0;
}

void keep()
{
#if defined(SOKOL_GLCORE)
	const int w = sapp_width(), h = sapp_height();
	if (w < 1 || h < 1) { return; }
	if (!gFbo) { glGenFramebuffers(1, &gFbo); glGenRenderbuffers(1, &gRb); }
	if (w != gW || h != gH)
	{
		glBindRenderbuffer(GL_RENDERBUFFER, gRb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gFbo);
		glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gRb);
		gW = w;
		gH = h;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gFbo);
	glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gKept = true;
	sg_reset_state_cache();	// sokol's GL state cache knows nothing of the binds above
#endif
}

bool show()
{
#if defined(SOKOL_GLCORE)
	static const bool off = std::getenv("KCLIENT_LOCKSTEP_NOKEEP") != nullptr;
	if (!gKept || off) { return false; }
	const int w = sapp_width(), h = sapp_height();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gFbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glBlitFramebuffer(0, 0, gW, gH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, (w == gW && h == gH) ? GL_NEAREST : GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	sg_reset_state_cache();
	return true;
#else
	return false;
#endif
}

void shutdown()
{
#if defined(SOKOL_GLCORE)
	if (gFbo) { glDeleteFramebuffers(1, &gFbo); glDeleteRenderbuffers(1, &gRb); gFbo = gRb = 0; }
	gW = gH = 0;
	gKept = false;
	sg_reset_state_cache();
#endif
}

void stallShot()
{
#if defined(SOKOL_GLCORE)
	static const char* dir = std::getenv("KCLIENT_LOCKSTEP_STALL_SHOTS");
	if (!dir || !*dir || gShots >= 8) { return; }
	const int w = sapp_width(), h = sapp_height();
	std::vector<unsigned char> px((size_t)w * h * 4);
	glFinish();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	for (size_t i = 3; i < px.size(); i += 4) { px[i] = 255; }
	char path[512];
	if (gShots == 0 && gKept)	// the last drawn frame (the kept copy) beside them, to compare against
	{
		std::vector<unsigned char> kp((size_t)gW * gH * 4);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gFbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, gW, gH, GL_RGBA, GL_UNSIGNED_BYTE, kp.data());
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		for (size_t i = 3; i < kp.size(); i += 4) { kp[i] = 255; }
		snprintf(path, sizeof path, "%s/kept%s", dir, imagewrite::defaultExt());
		readback::write_png(path, gW, gH, kp.data());
	}
	snprintf(path, sizeof path, "%s/nopass-%02d%s", dir, gShots++, imagewrite::defaultExt());
	if (readback::write_png(path, w, h, px.data())) { fprintf(stderr, "kclient: lockstep: wrote %s (the back buffer a frame without a pass swaps)\n", path); }
	sg_reset_state_cache();
#endif
}
}
