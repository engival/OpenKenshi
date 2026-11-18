#include "characters/creatorscene.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

#include "creatorbg.glsl.h"
#include "assets/dds.hpp"
#include "render/drawstats.hpp"
#include "render/sky.hpp"

static bool slurp(const std::string& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { return false; }
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

// `param_named NAME float3 x y z` / `param_named NAME float x` from a material script; false when absent.
static bool param(const std::string& text, const char* name, int n, float* out)
{
	const std::string num = R"(\s+(-?[0-9.]+))";
	std::string re = std::string("param_named\\s+") + name + R"(\s+float)" + (n == 1 ? "" : std::to_string(n));
	for (int i = 0; i < n; i++) { re += num; }
	std::smatch m;
	if (!std::regex_search(text, m, std::regex(re))) { return false; }
	for (int i = 0; i < n; i++) { out[i] = (float)atof(m[1 + i].str().c_str()); }
	return true;
}

bool CreatorScene::load(const std::string& dataDir, std::string& err)
{
	std::string comp, mat;
	const std::string cpath = dataDir + "/materials/compositors/charedit.compositor", mpath = dataDir + "/materials/deferred/deferred.material";
	if (!slurp(cpath, comp)) { err = cpath + " is missing"; return false; }
	if (!slurp(mpath, mat)) { err = mpath + " is missing"; return false; }
	// the CharacterEditorLighting material sets the direction and the colour; the Deferred_RTT program's default supplies the exposure
	const size_t at = comp.find("material CharacterEditorLighting");
	const std::string lighting = at == std::string::npos ? std::string() : comp.substr(at);
	if (!param(lighting, "lightDirection", 3, dir_) || !param(lighting, "lightColour", 3, colour_)) { err = cpath + ": CharacterEditorLighting lightDirection / lightColour not found"; return false; }
	const size_t rtt = mat.find("fragment_program Deferred_RTT unified");
	if (rtt == std::string::npos || !param(mat.substr(rtt), "exposure", 1, &exposure_)) { err = mpath + ": Deferred_RTT exposure not found"; return false; }

	DdsImage dds;
	if (!dds_load(dataDir + "/gui/gfx/SandyGround.dds", dds, err)) { return false; }
	img_ = dds_make_image(dds, "creator-backdrop");
	sg_view_desc vd = {};
	vd.texture.image = img_;
	view_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);
	shd_ = sg_make_shader(creatorbg_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = false;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "creator-backdrop";
	pip_ = sg_make_pipeline(&pd);
	return true;
}

void CreatorScene::shutdown()
{
	if (!ready()) { return; }
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
	sg_destroy_sampler(smp_);
	sg_destroy_view(view_);
	sg_destroy_image(img_);
	pip_ = {};
}

void CreatorScene::drawBackdrop()
{
	if (!ready()) { return; }
	drawstats::pipeline(pip_);
	sg_bindings bd = {};
	bd.views[VIEW_bg_tex] = view_;
	bd.samplers[SMP_bg_smp] = smp_;
	drawstats::bindings(&bd);
	drawstats::draw(0, 3, 1);
}

// lighting.inc light_shade / light_spec / light_spec_sun are rtt.hlsl main_fs term for term at these values: sun = pi N.L lightColour x 0.96 (nadir.w = 1,
// sun_col = lightColour), ambient = the static cube's irradiance x 0.96 (amb_mult = 1, zenith.w = 1, the dynamic ambient off; its clamp(5 sunY + 0.2, 0.1, 1)
// is 1 for this light's elevation), the environment and sun specular, all x the pass' exposure (fog_col.w). The pass has no fog (the range and the density
// are pushed out of reach), no weather surface state, no building lights and no hcloud blend.
void CreatorScene::light(LightParams& lp) const
{
	const float len = std::sqrt(dir_[0] * dir_[0] + dir_[1] * dir_[1] + dir_[2] * dir_[2]);
	for (int i = 0; i < 3; i++)
	{
		lp.v[0][i] = dir_[i] / len;
		lp.v[1][i] = colour_[i];
		lp.v[7][i] = 1.0f;
		lp.v[6][i] = lp.v[4][i] = 0.0f;
	}
	lp.v[0][3] = 0;
	lp.v[1][3] = 1.0f;
	lp.v[2][3] = 1.0f;	// zenith.w: envColour.w
	lp.v[3][3] = 1.0f;	// nadir.w: the sun's scale
	lp.v[4][3] = exposure_;
	lp.v[5][0] = 1.0e9f;	// fog range: never reached
	lp.v[5][1] = 2.0e9f;
	lp.v[5][3] = 0;
	lp.v[6][3] = 0;
	lp.v[7][3] = 0;	// amb_mult.w: not the fog-map mode
	lp.sh[0][3] = 0;
	lp.plHead[0] = 0;
}
