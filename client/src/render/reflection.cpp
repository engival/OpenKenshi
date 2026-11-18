#include "render/reflection.hpp"

#include <cmath>

void Reflection::init()
{
	sg_image_desc id = {};
	id.usage.color_attachment = true;
	id.width = id.height = kSize;
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.sample_count = 1;
	id.label = "water-reflection";
	colour_ = sg_make_image(&id);
	id = {};
	id.usage.depth_stencil_attachment = true;
	id.width = id.height = kSize;
	id.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
	id.sample_count = 1;
	id.label = "water-reflection-depth";
	depth_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = colour_;
	tex_ = sg_make_view(&vd);
	vd = {};
	vd.color_attachment.image = colour_;
	att_ = sg_make_view(&vd);
	vd = {};
	vd.depth_stencil_attachment.image = depth_;
	depthAtt_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.label = "water-reflection";
	smp_ = sg_make_sampler(&sd);
}

void Reflection::shutdown()
{
	sg_destroy_sampler(smp_);
	sg_destroy_view(depthAtt_);
	sg_destroy_view(att_);
	sg_destroy_view(tex_);
	sg_destroy_image(depth_);
	sg_destroy_image(colour_);
}

bool Reflection::setup(const Camera& cam)
{
	if (cam.pos[1] <= kPlane + 0.01) { return false; }
	cam_ = cam;
	cam_.width = cam_.height = kSize;
	cam_.pos[1] = 2.0 * kPlane - cam.pos[1];
	cam_.f[1] = -cam.f[1];
	cam_.u[1] = -cam.u[1];
	// right = forward x up of the mirrored pair = minus the mirrored right: a rotation, not a reflection (the image comes out flipped left to right)
	cam_.r[0] = -cam.r[0];
	cam_.r[2] = -cam.r[2];

	const float t = 1.0f / cam_.tanY;
	float proj[16] = {};
	proj[0] = t / cam_.aspect;
	proj[5] = t;
	proj[11] = -1;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	proj[10] = cam_.farZ / (cam_.nearZ - cam_.farZ);
	proj[14] = cam_.farZ * cam_.nearZ / (cam_.nearZ - cam_.farZ);
#else
	proj[10] = (cam_.farZ + cam_.nearZ) / (cam_.nearZ - cam_.farZ);
	proj[14] = 2 * cam_.farZ * cam_.nearZ / (cam_.nearZ - cam_.farZ);
#endif
	// the near plane is the water plane (Ogre enableCustomNearClipPlane; Lengyel's oblique near plane): world y >= kPlane, in view space (rows r, u, -f).
	// The mirrored camera is below the plane, so it lies on the plane's negative side as the method needs.
	const double c[4] = { cam_.r[1], cam_.u[1], -cam_.f[1], cam_.pos[1] - kPlane };
	// the far corner of the frustum on the plane's side: the inverse projection of (sign x, sign y, 1, 1)
	const double q[4] = { ((c[0] > 0) - (c[0] < 0)) / (double)proj[0], ((c[1] > 0) - (c[1] < 0)) / (double)proj[5], -1.0, (1.0 + proj[10]) / proj[14] };
	const double cq = c[0] * q[0] + c[1] * q[1] + c[2] * q[2] + c[3] * q[3];
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	const double s = 1.0 / cq;	// z 0..1: the third row is the plane itself
	for (int k = 0; k < 4; k++) { proj[k * 4 + 2] = (float)(c[k] * s); }
#else
	const double s = 2.0 / cq;	// z -1..1: the third row is 2 x plane / (plane . q) minus the fourth row (0, 0, -1, 0)
	for (int k = 0; k < 4; k++) { proj[k * 4 + 2] = (float)(c[k] * s + (k == 2 ? 1.0 : 0.0)); }
#endif
	float view[16] = {};
	for (int i = 0; i < 3; i++)
	{
		view[i * 4 + 0] = (float)cam_.r[i];
		view[i * 4 + 1] = (float)cam_.u[i];
		view[i * 4 + 2] = (float)-cam_.f[i];
	}
	view[15] = 1;
	// the renderers place geometry relative to the main camera; relative to the mirrored one it sits higher by (main y - mirrored y)
	view[13] = (float)(cam_.u[1] * (cam.pos[1] - cam_.pos[1]));
	view[12] = (float)(cam_.r[1] * (cam.pos[1] - cam_.pos[1]));
	view[14] = (float)(-cam_.f[1] * (cam.pos[1] - cam_.pos[1]));
	mat_mul(mvp_, proj, view);
	return true;
}

sg_pass Reflection::pass() const
{
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	p.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 1.0f };	// the compositor's default clear (ColourValue::Black): no reflection where nothing is drawn
	p.action.depth.load_action = SG_LOADACTION_CLEAR;
	p.action.depth.clear_value = 1.0f;
	p.attachments.colors[0] = att_;
	p.attachments.depth_stencil = depthAtt_;
	p.label = "water-reflection";
	return p;
}
