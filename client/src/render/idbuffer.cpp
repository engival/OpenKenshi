#include "render/idbuffer.hpp"

#include <cmath>
#include <cstdio>

#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"
#endif

void IdBuffer::shutdown()
{
#if defined(SOKOL_GLCORE)
	if (fbo_) { glDeleteFramebuffers(1, &fbo_); }
	if (pbo_[0]) { glDeleteBuffers(kRing, pbo_); }
	fbo_ = 0;
	pbo_[0] = pbo_[1] = 0;
#endif
	if (att_.id) { sg_destroy_view(att_); }
	if (image_.id) { sg_destroy_image(image_); }
	att_ = {};
	image_ = {};
}

sg_pass IdBuffer::pass(sg_view sceneDepth, int w, int h)
{
	if (w != w_ || h != h_)
	{
		if (att_.id) { sg_destroy_view(att_); }
		if (image_.id) { sg_destroy_image(image_); }
		w_ = w;
		h_ = h;
		sg_image_desc id = {};
		id.usage.color_attachment = true;
		id.width = w;
		id.height = h;
		id.pixel_format = SG_PIXELFORMAT_RGBA32F;
		id.sample_count = 1;
		id.label = "cursor-id";
		image_ = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.color_attachment.image = image_;
		att_ = sg_make_view(&vd);
		for (bool& q : queued_) { q = false; }
		last_ = {};
	}
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 0.0f };
	pass.action.depth.load_action = SG_LOADACTION_LOAD;
	pass.action.stencil.load_action = SG_LOADACTION_LOAD;
	pass.attachments.colors[0] = att_;
	pass.attachments.depth_stencil = sceneDepth;
	pass.label = "cursor-id";
	return pass;
}

IdBuffer::Hit IdBuffer::read(int x, int y, const double cam[3])
{
#if defined(SOKOL_GLCORE)
	if (!fbo_)
	{
		glGenFramebuffers(1, &fbo_);
		glGenBuffers(kRing, pbo_);
		for (unsigned b : pbo_)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
			glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(float), nullptr, GL_STREAM_READ);
		}
	}
	// this slot's read was queued kRing frames ago: by now the GPU has finished it
	const int slot = next_;
	if (queued_[slot])
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
		if (const float* px = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT))
		{
			last_.valid = true;
			last_.building = px[0] >= 0.5f ? (int)std::lround(px[0]) - 1 : -1;
			for (int k = 0; k < 3; k++) { last_.at[k] = cam_[slot][k] + px[1 + k]; }
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		queued_[slot] = false;
	}
	if (x >= 0 && y >= 0 && x < w_ && y < h_)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sg_gl_query_image_info(image_).tex[0], 0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
		glReadPixels(x, h_ - 1 - y, 1, 1, GL_RGBA, GL_FLOAT, nullptr);	// GL's origin is the bottom left
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		for (int k = 0; k < 3; k++) { cam_[slot][k] = cam[k]; }
		queued_[slot] = true;
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	sg_reset_state_cache();
	next_ = (next_ + 1) % kRing;
#else
	(void)x;
	(void)y;
	(void)cam;
	static bool warned = false;
	if (!warned) { fprintf(stderr, "kclient: the cursor ID buffer reads back with GL only: the building hover (camera.peek) is off on this backend\n"); warned = true; }
#endif
	return last_;
}
