#pragma once
#include <memory>
#include <vector>

#include "lightbake.hpp"
#include "sokol_gfx.h"

// Interior light on the GPU (render.interior_light, kenshi-jj3.2.1; lighting.inc lighting_vol). The baked sky transfer of the nearest buildings (WorldBuildings::lightVolumes,
// lightbake.hpp) is packed into one 3D atlas, up to kMax volumes side by side along x, and the lit programs that call light_vol (mesh, character) multiply the ambient
// term by it. The atlas is rebuilt only when the set of volumes changes; the uniform block (the camera-relative position -> probe index of each volume) every frame.
// render.sun_bounce (kenshi-jj3.4.1) adds the SUN TRANSFER of the same volumes: an RGB one per probe (3 textures, one per channel), two pages side by side (the latest
// bake, and the one it replaces) so a re-bake cross-fades on the GPU: the per-volume fade mixes the pages.
// Like the AO, it is live only between begin() and end() (the scene pass of the main camera): any other pass (the reflection, portraits, the creator) binds the empty 1x1x1
// volume and a zero count, factor exactly 1.
namespace lightvol
{
constexpr int kMax = 8;
constexpr int kParamFloats = 8 + kMax * 7 * 4;	// light_volumes: head, atlas, 7 vec4 per volume

struct Volume
{
	uint32_t key = 0;	// the placement (the same building drawn again keeps its blend)
	std::shared_ptr<const lightbake::Grid> grid;
	std::shared_ptr<const lightbake::Grid> sunNew, sunOld;	// render.sun_bounce: the latest sun bake and the one it replaces (null: no sun term)
	double origin[3] = {};	// building-local -> world: origin + R(q) * (local x scale)
	float q[4] = { 1, 0, 0, 0 };	// qw, qx, qy, qz
	float scale[3] = { 1, 1, 1 };
	std::shared_ptr<const std::vector<uint8_t>> lamps;	// render.lamp_occlusion: lightbake::kMaxLamps bytes per probe (255 = the probe sees the lamp), null: none traced yet
	int lampCount = 0;
};

struct Binding
{
	sg_view sh = {};
	sg_view use = {};
	sg_view lamp = {};	// RGBA8, twice the atlas wide: lamps 0..3 in the left half, 4..7 in the right (255 = seen)
	sg_view sun[3] = {};	// render.sun_bounce: the sun transfer's R, G, B SH
	sg_sampler smp = {};
	float params[kParamFloats] = {};
};
// vol_use is RG16F: r = usable, g = enclosure x usable (the sun shafts). What the lit programs bind: VIEW_vol_sh, VIEW_vol_use, VIEW_vol_sun0..2, SMP_vol_smp and UB_light_volumes from here.
const Binding& binding();

class Volumes
{
public:
	void init();
	void shutdown();
	// Before the main scene pass: make `vols` (nearest first, at most kMax) the live volumes for the camera at `cam`. strength 0 (render.interior_light 1, the original
	// lighting, the effect off) or no volume: off, unless keep (render.sun_shafts reads the volumes): then they stay live with strength 0, which the lit programs read as factor exactly 1.
	// groundAlbedo: what the bake's ground bounced (the open-field reference). sunStrength: render.sun_bounce (0 = off; the sun term also needs strength > 0, as the switch
	// does nothing while render.interior_light is 1). dt: the frame time, for the blend-in of a new volume and the cross-fade of a re-baked sun.
	void begin(const double cam[3], const std::vector<Volume>& vols, float strength, float sunStrength, float groundAlbedo, float dt, bool keep = false);
	void end();	// after the main scene pass: the empty volume again
	size_t live() const { return n_; }
	// The code a light carries in pl[4 i + 3].w (lighting.inc light_points_vol): 1 + volume * 8 + lamp for lamp `lamp` of the volume keyed `key` (WorldBuildings owner) when that volume has its visibility; else 0.
	int lampCode(uint32_t key, int lamp) const;

private:
	void build(const std::vector<Volume>& vols);
	void destroyAtlas();

	sg_image shImg_ = {}, useImg_ = {}, sunImg_[3] = {};
	sg_view shView_ = {}, useView_ = {}, sunView_[3] = {};
	sg_image noneImg_ = {};
	sg_view noneView_ = {};
	sg_sampler smp_ = {};
	struct Slot { uint32_t key; const lightbake::Grid* grid; const lightbake::Grid* sunNew; const lightbake::Grid* sunOld; int offset; int sunOffset; float blend; float fade; const std::vector<uint8_t>* lamps; int lampCount; };
	sg_image lampImg_ = {};
	sg_view lampView_ = {};
	sg_image noneLampImg_ = {};
	sg_view noneLampView_ = {};
	std::vector<Slot> slots_;
	int atlas_[3] = { 1, 1, 1 };
	int sunAtlas_[3] = { 1, 1, 1 };	// page A width (probes), height, depth; the texture is twice as wide (page A, page B)
	size_t n_ = 0;
};
}
