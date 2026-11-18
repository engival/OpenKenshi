#include "kworld/foliage.hpp"

#include "kworld/error.hpp"
#include "io.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// GCC false positive in stb's tRNS path (tc[3]: img_n is 1 or 3 there, checked just above it).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <stb_image.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace kworld {

BiomeMap BiomeMap::load(const std::string& pngPath)
{
	const auto bytes = detail::readFile(pngPath);
	int width = 0;
	int height = 0;
	int channels = 0;
	std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
		stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
			static_cast<int>(bytes.size()), &width, &height, &channels, 3),
		stbi_image_free);
	if (!pixels || width <= 0 || height <= 0)
		throw Error("biomemap: cannot decode " + pngPath);

	std::vector<std::uint32_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
	for (std::size_t i = 0; i < rgb.size(); ++i)
	{
		const stbi_uc* p = pixels.get() + 3 * i;
		rgb[i] = (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
	}
	return fromPixels(width, height, std::move(rgb));
}

BiomeMap BiomeMap::fromPixels(int width, int height, std::vector<std::uint32_t> rgb)
{
	if (width <= 0 || height <= 0
		|| rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
		throw Error("biomemap: pixel count does not match the size");
	BiomeMap map;
	map.width_ = width;
	map.height_ = height;
	map.rgb_ = std::move(rgb);
	return map;
}

std::uint32_t BiomeMap::pixel(int col, int row) const
{
	if (col < 0 || row < 0 || col >= width_ || row >= height_)
		throw Error("biomemap: pixel outside the image");
	return rgb_[static_cast<std::size_t>(row) * static_cast<std::size_t>(width_)
		+ static_cast<std::size_t>(col)];
}

std::uint32_t BiomeMap::biomeAt(double x, double z) const
{
	const double unit = unitsPerPixel();
	const auto axis = [unit](double v, int count) {
		const double cell = std::floor((v - kWorldMin) / unit);
		if (!(cell > 0.0))
			return 0; // also NaN
		return cell >= count ? count - 1 : static_cast<int>(cell);
	};
	return pixel(axis(x, width_), axis(z, height_));
}

} // namespace kworld
