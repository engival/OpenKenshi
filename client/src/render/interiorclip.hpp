// The interior mask view the terrain and grass fragment shaders clip by (shaders/interiorclip.inc; kenshi-h2v.11.4.2). main.cpp binds the scene's
// mask target (Hdr::maskTex) while it draws the main scene pass (use) and the 1x1 zero view otherwise (the water's reflection pass, every other
// view; release), so a renderer always binds a valid view and only the main scene clips.
#pragma once
#include "sokol_gfx.h"

namespace interiorclip
{
struct State { sg_image zeroImg = {}; sg_view zero = {}, cur = {}; sg_sampler smp = {}; };
inline State& state() { static State s; return s; }
inline void init()
{
	State& s = state();
	const uint16_t zero[2] = { 0, 0 };
	sg_image_desc d = {};
	d.width = d.height = 1;
	d.pixel_format = SG_PIXELFORMAT_RG16F;
	d.data = { zero, sizeof zero };
	d.label = "interior-clip-zero";
	s.zeroImg = sg_make_image(&d);
	sg_view_desc v = {};
	v.texture.image = s.zeroImg;
	s.zero = sg_make_view(&v);
	s.cur = s.zero;
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	s.smp = sg_make_sampler(&sd);
}
inline void shutdown() { State& s = state(); sg_destroy_sampler(s.smp); sg_destroy_view(s.zero); sg_destroy_image(s.zeroImg); s = {}; }
inline void use(sg_view mask) { state().cur = mask; }
inline void release() { state().cur = state().zero; }
inline sg_view view() { return state().cur; }
inline sg_sampler sampler() { return state().smp; }
}
