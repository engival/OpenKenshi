#pragma once

// data/newland/land/fullmap.tif reader (docs/formats/TERRAIN_FORMAT.md).
// The 16-bit samples are memory-mapped; only touched pages are read.

#include <cstddef>
#include <cstdint>
#include <string>

namespace kworld {

inline constexpr double kMaxHeight = 9800.0; // raw sample 65535
inline constexpr double kHeightScale = kMaxHeight / 65535.0;
inline constexpr double kHeightmapSpacing = 18.0;

class Heightmap
{
public:
	// Throws Error when the TIFF header/tags do not match the documented
	// single-strip, 16-bit, uncompressed layout.
	static Heightmap open(const std::string& path);

	Heightmap(Heightmap&& other) noexcept;
	Heightmap& operator=(Heightmap&& other) noexcept;
	Heightmap(const Heightmap&) = delete;
	Heightmap& operator=(const Heightmap&) = delete;
	~Heightmap();

	int sampleCountX() const noexcept { return width_; }
	int sampleCountZ() const noexcept { return rows_; }

	std::uint16_t rawSample(int col, int row) const;
	double sampleHeight(int col, int row) const;

	// World units, bilinear between samples. Coordinates are clamped to the
	// sample grid, so the half-open world edge +147456 uses the last sample.
	double height(double x, double z) const;

	// Terrain::getHeight at full detail (docs/reference/TERRAIN_HEIGHT.md):
	// the 18-unit lattice is two triangles per cell, split along the diagonal
	// (i+1, j)-(i, j+1). Differs from height() by up to ~0.5 on rough ground.
	double terrainHeight(double x, double z) const;

	const std::string& path() const noexcept { return path_; }

private:
	Heightmap() = default;

	std::string path_;
	void* mapping_ = nullptr;
	std::size_t mappingSize_ = 0;
	const std::uint8_t* pixels_ = nullptr;
	std::size_t pixelOffset_ = 0;
	int width_ = 0;
	int rows_ = 0;
};

} // namespace kworld
