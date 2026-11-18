#include "render/speccube.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{

constexpr int kFaces = 6;
SpecCubeBinding g_binding, g_water, g_irr;
sg_image g_image = {}, g_waterImage = {}, g_irrImage = {};

size_t level_bytes(int w) { return (size_t)((std::max(w, 1) + 3) / 4) * ((std::max(w, 1) + 3) / 4) * 16; }

// One DXT5 cube from data/materials; a black stand-in when the file is missing or malformed (the return value says which).
bool load_cube(const std::string& path, const char* label, sg_image& image, SpecCubeBinding& binding)
{
	std::vector<uint8_t> file;
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (in)
	{
		file.resize((size_t)in.tellg());
		in.seekg(0);
		in.read((char*)file.data(), (std::streamsize)file.size());
	}
	// DDS cube: header, then per face the full mip chain (face-major); sokol wants per mip all six faces (mip-major).
	uint32_t height = 0, width = 0, mips = 0, caps2 = 0;
	bool ok = file.size() >= 128 && memcmp(file.data(), "DDS ", 4) == 0 && memcmp(file.data() + 84, "DXT5", 4) == 0;
	if (ok)
	{
		memcpy(&height, file.data() + 12, 4);
		memcpy(&width, file.data() + 16, 4);
		memcpy(&mips, file.data() + 28, 4);
		memcpy(&caps2, file.data() + 112, 4);
		mips = std::max(mips, 1u);	// 0 = no chain (mp_specuwatery.dds)
		ok = width == height && width >= 4 && width <= 4096 && mips >= 1 && mips <= 13 && (caps2 & 0x200) != 0;
	}
	std::vector<uint8_t> data;
	if (ok)
	{
		size_t faceBytes = 0;
		for (uint32_t m = 0; m < mips; m++) { faceBytes += level_bytes((int)width >> m); }
		ok = file.size() >= 128 + faceBytes * kFaces;
		size_t before = 0;
		for (uint32_t m = 0; ok && m < mips; m++)
		{
			const size_t lb = level_bytes((int)width >> m);
			for (int f = 0; f < kFaces; f++)
			{
				const uint8_t* src = file.data() + 128 + faceBytes * f + before;
				data.insert(data.end(), src, src + lb);
			}
			before += lb;
		}
	}
	static const uint8_t black[kFaces * 16] = {};	// one BC3 block of zeros per face = rgb 0, alpha 0: a missing cube reflects nothing
	sg_image_desc id = {};
	id.type = SG_IMAGETYPE_CUBE;
	id.pixel_format = SG_PIXELFORMAT_BC3_RGBA;
	id.label = label;
	id.width = id.height = ok ? (int)width : 4;
	id.num_mipmaps = ok ? (int)mips : 1;
	id.data = ok ? sg_range{ data.data(), data.size() } : sg_range{ black, sizeof black };
	image = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = image;
	binding.view = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = sd.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
	sd.label = label;
	binding.smp = sg_make_sampler(&sd);
	return ok;
}

}

const SpecCubeBinding& spec_cube_binding() { return g_binding; }
const SpecCubeBinding& water_cube_binding() { return g_water; }
const SpecCubeBinding& irr_cube_binding() { return g_irr; }

bool spec_cube_init(const std::string& dataDir)
{
	const bool ok = load_cube(dataDir + "/materials/mp_specularity.dds", "specularity-cube", g_image, g_binding);
	if (!load_cube(dataDir + "/materials/mp_specuwatery.dds", "water-specularity-cube", g_waterImage, g_water))
	{
		fprintf(stderr, "kclient: mp_specuwatery.dds not usable under %s/materials: no environment specular on water\n", dataDir.c_str());
	}
	if (!load_cube(dataDir + "/materials/mp_irradiance.dds", "irradiance-cube", g_irrImage, g_irr))
	{
		fprintf(stderr, "kclient: mp_irradiance.dds not usable under %s/materials: the ambient light is black\n", dataDir.c_str());
	}
	return ok;
}

void spec_cube_shutdown()
{
	sg_destroy_sampler(g_irr.smp);
	sg_destroy_view(g_irr.view);
	sg_destroy_image(g_irrImage);
	g_irr = {};
	sg_destroy_sampler(g_water.smp);
	sg_destroy_view(g_water.view);
	sg_destroy_image(g_waterImage);
	g_water = {};
	sg_destroy_sampler(g_binding.smp);
	sg_destroy_view(g_binding.view);
	sg_destroy_image(g_image);
	g_binding = {};
}
