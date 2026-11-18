#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sokol_gfx.h"

// Blood on the ground (docs/reference/BLOOD_DECALS.md, the game's TerrainDecalsManager): decals painted with the `materials/blood/<type>` images into one render
// target per terrain patch near the camera; the terrain shader mixes that texture into its albedo before lighting (the game's TerrainBlood pass). The 8 patch targets
// are the 8 tiles of one atlas here. Decals come from scene keys until the sim sends hits and bleeding.
class BloodDecals
{
public:
	enum Type { kLine, kSplash, kPool, kSmear, kTypes };
	static constexpr int kSlots = 8;	// the game's 8 Kenshi_Decal_Texture_N
	static constexpr int kTile = 1024;	// settings.cfg `Decal Resolution`
	static constexpr double kPatch = 288.0;	// a terrain patch at quadtree level 10: 294912 / 1024 (UNSURE, BLOOD_DECALS.md section 2)
	static constexpr double kRange = 1000.0;	// settings.cfg `Decal Range`

	// dataDir = the game's data/ directory
	void init(const std::string& dataDir);
	void shutdown();

	// dce50: size is the emitter's size (0.05..0.5 for a hit splash); dirRad = the direction's angle in the x-z plane (atan2(z, x)), the image's u (width) axis follows it.
	// image < 0: picked from a hash of the position (the game: rand over the type's images). colour: RACE blood colour, alpha = the decal's alpha.
	void add(Type type, double x, double z, float size, float dirRad, const float colour[4], int image = -1);
	// dcfa0, the growing pool under a KO'd or down character: resizes key's latest pool decal in place (size g, 0..2; width 72 g with the 512 px pool images), or
	// starts a new one (true) when there is none within sqrt(10) of x, z. key = the character id; the pool stops growing when the calls stop.
	bool pool(uint64_t key, double x, double z, float size, const float colour[4]);
	void clear();
	static int typeByName(const std::string& name);	// -1 if unknown

	// 1408dd860, once per 100 ms pass: every decal's life -= the frame's dt * game speed (0 when paused); dead decals are deleted only off resident patches.
	// Painted tiles are not faded live: a repaint draws with alpha min(a, life), so only the last 1.0 of life shows.
	void age(double dt, double speed);

	// Logic side: picks the patches (up to 8 within kRange of the camera, the most decals first) and, when the choice or the decals changed, queues
	// the atlas repaint (renderqueue.hpp, frame lane: its own pass, run at the top of render_frame before the scene pass) with a copy of the decals.
	void update(const double cam[3]);

	// Render side. Terrain fragment uniforms: per slot (patch origin x, z, 1 / patch size, 1 if used), as the last repaint job published them
	const float* patches() const { return gpuPatch_[0]; }
	sg_view atlas() const { return atlasView_.id ? atlasView_ : blank_; }
	sg_sampler sampler() const { return smp_; }
	size_t size() const { return decals_.size(); }

private:
	struct Decal
	{
		int type, image;
		double x, z;
		float halfW, halfH;	// world units: size * image px * patch / 4096
		float rot;	// the image's u axis angle
		float colour[4];
		uint64_t key = 0;	// pool(): the growing character's id, 0 for the rest
		float life = 99.0f;	// dce50: always 99; ~0.16 / s at 60 fps, 1x (age)
	};
	struct Image { sg_image img = {}; sg_view view = {}; int w = 0, h = 0; };
	struct Size { int w = 0, h = 0; };
	// one repaint: the slots' patches and the decals that touch them, by value (the render job owns it)
	struct Paint
	{
		long long slotPatch[kSlots];
		float patch[kSlots][4];
		std::vector<Decal> decals;
	};
	void paint(const Paint& p);	// render side: the atlas (made on first use), cleared and every slot repainted

	// logic side
	std::vector<Size> sizes_[kTypes];	// the images' pixel sizes (init), what add() and pool() read
	std::vector<Decal> decals_;
	bool dirty_ = false;
	double passClock_ = 0;	// real seconds toward the next 100 ms aging pass
	long long slotPatch_[kSlots];	// patch key per slot, -1 free
	float patch_[kSlots][4] = {};
	// render side (init and shutdown run on the one thread there is then)
	std::vector<Image> images_[kTypes];
	float gpuPatch_[kSlots][4] = {};
	sg_image atlas_ = {}, blankImg_ = {};
	sg_view atlasView_ = {}, atlasAtt_ = {}, blank_ = {};
	sg_sampler smp_ = {}, imgSmp_ = {};
	sg_pipeline pip_ = {};
	sg_buffer quad_ = {};
};
