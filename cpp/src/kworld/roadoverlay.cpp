#include "kworld/roadoverlay.hpp"

#include "kworld/error.hpp"
#include "io.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// GCC false positive in stb's tRNS path (tc[3]: img_n is 1 or 3 there, checked just above it).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include <stb_image.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace kworld {

const std::vector<std::uint8_t>& RoadOverlay::tile(int tx, int tz) const
{
	const std::size_t slot = static_cast<std::size_t>(tz * kTilesPerAxis + tx);
	std::call_once(loaded_[slot], [&] {
		const auto path = std::filesystem::path(dataDir_) / "newland" / "land" / "overlaymaps"
			/ ("new_overlay." + std::to_string(tx) + "." + std::to_string(tz) + ".png");
		if (!std::filesystem::exists(path))
			return; // no tile: no road
		const auto bytes = detail::readFile(path.string());
		int width = 0;
		int height = 0;
		int channels = 0;
		std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
			stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
				static_cast<int>(bytes.size()), &width, &height, &channels, 4),
			stbi_image_free);
		if (!pixels || width != kTilePixels || height != kTilePixels)
			throw Error("overlay: cannot decode " + path.string());
		std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
		std::vector<std::uint8_t> red(out.size());
		for (std::size_t i = 0; i < out.size(); ++i)
		{
			out[i] = pixels.get()[4 * i + 3];
			red[i] = pixels.get()[4 * i];
		}
		alpha_[slot] = std::move(out);
		red_[slot] = std::move(red);
	});
	return alpha_[slot];
}

long RoadOverlay::region(double world) const
{
	if (!(world >= kWorldMin && world < kWorldMin + kRegionSize * kTilesPerAxis))
		return -1;
	const long index = static_cast<long>((world - kWorldMin) / kRegionSize);
	return std::clamp(index, 0L, static_cast<long>(kTilesPerAxis) - 1);
}

long RoadOverlay::texelIn(long region, double world) const
{
	// trunc((w - (regionMin - 18)) / (36900 / 1024)), the game's placement.
	if (region < 0)
		return -1; // off the world: alpha() answers 0 (out-of-range clear)
	const double origin = kWorldMin + kRegionSize * static_cast<double>(region) + kOriginMargin;
	const long inside = static_cast<long>((world - origin) / kPixelSize);
	return region * kTilePixels + std::clamp(inside, 0L, static_cast<long>(kTilePixels) - 1);
}

long RoadOverlay::texel(double world) const
{
	return texelIn(region(world), world);
}

std::uint8_t RoadOverlay::alpha(long texelX, long texelZ) const
{
	constexpr long total = static_cast<long>(kTilePixels) * kTilesPerAxis;
	if (texelX < 0 || texelZ < 0 || texelX >= total || texelZ >= total)
		return 0;
	const auto& pixels = tile(static_cast<int>(texelX / kTilePixels),
		static_cast<int>(texelZ / kTilePixels));
	if (pixels.empty())
		return 0;
	return pixels[static_cast<std::size_t>(texelZ % kTilePixels) * kTilePixels
		+ static_cast<std::size_t>(texelX % kTilePixels)];
}

std::uint8_t RoadOverlay::grassDensity(long texelX, long texelZ) const
{
	constexpr long total = static_cast<long>(kTilePixels) * kTilesPerAxis;
	if (texelX < 0 || texelZ < 0 || texelX >= total || texelZ >= total)
		return 0;
	tile(static_cast<int>(texelX / kTilePixels), static_cast<int>(texelZ / kTilePixels)); // loads both planes
	const auto& red = red_[static_cast<std::size_t>(texelZ / kTilePixels * kTilesPerAxis + texelX / kTilePixels)];
	if (red.empty())
		return 0;
	return red[static_cast<std::size_t>(texelZ % kTilePixels) * kTilePixels + static_cast<std::size_t>(texelX % kTilePixels)];
}

double RoadOverlay::texelCentre(long texel)
{
	const long region = texel / kTilePixels;
	return kWorldMin + kRegionSize * static_cast<double>(region) + kOriginMargin
		+ (static_cast<double>(texel % kTilePixels) + 0.5) * kPixelSize;
}

bool RoadOverlay::roadRejected(double x, double z, double radius) const
{
	// Both corners on an axis are read in the region that holds the point:
	// the game clamps them into its one region image.
	const long regionX = region(x);
	const long regionZ = region(z);
	const long x0 = texelIn(regionX, x - radius);
	const long x1 = texelIn(regionX, x + radius);
	const long z0 = texelIn(regionZ, z - radius);
	const long z1 = texelIn(regionZ, z + radius);
	for (long i = x0; i <= x1; ++i)
		if (alpha(i, z0) != 0)
			return true;
	if (x0 < x1 || z0 < z1)
		if (alpha((x0 + x1) / 2, (z0 + z1) / 2) != 0)
			return true;
	if (z0 < z1)
		for (long i = x0; i <= x1; ++i)
			if (alpha(i, z1) != 0)
				return true;
	if (z0 + 1 < z1)
		for (long j = z0 + 1; j < z1; ++j)
			if (alpha(x0, j) != 0 || alpha(x1, j) != 0)
				return true;
	return false;
}

} // namespace kworld
