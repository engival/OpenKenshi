#include "kworld/heightmap.hpp"

#include "kworld/coords.hpp"
#include "kworld/error.hpp"
#include "kutil/platform/mapped_file.hpp"
#include "le.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>


namespace kworld {

namespace {

struct Tag
{
	std::uint16_t tag = 0;
	std::uint16_t type = 0;
	std::uint32_t count = 0;
	std::uint32_t value = 0;
};

const Tag* findTag(const std::vector<Tag>& tags, std::uint16_t wanted)
{
	for (const auto& tag : tags)
		if (tag.tag == wanted)
			return &tag;
	return nullptr;
}

std::uint32_t tagUint32(const std::vector<Tag>& tags, std::uint16_t wanted,
	const char* name)
{
	const Tag* tag = findTag(tags, wanted);
	if (tag == nullptr)
		throw Error(std::string("TIFF: missing tag ") + name);
	if (tag->count != 1)
		throw Error(std::string("TIFF: tag ") + name + " is not a scalar");
	if (tag->type == 4) // LONG
		return tag->value;
	if (tag->type == 3) // SHORT, stored in the low half of the value field
		return tag->value & 0xffffu;
	throw Error(std::string("TIFF: tag ") + name + " has an unexpected type");
}

} // namespace

Heightmap Heightmap::open(const std::string& path)
{
	Heightmap map;
	map.path_ = path;

	std::string openError;
	if (!map.file_.open(path, openError))
		throw Error(openError);
	const auto* bytes = static_cast<const std::byte*>(map.file_.data());

	if (map.file_.size() < 8)
		throw Error("TIFF: file is too short");
	if (std::to_integer<std::uint8_t>(bytes[0]) != 'I'
		|| std::to_integer<std::uint8_t>(bytes[1]) != 'I'
		|| std::to_integer<std::uint8_t>(bytes[2]) != 42
		|| std::to_integer<std::uint8_t>(bytes[3]) != 0)
		throw Error("TIFF: not a classic little-endian TIFF");

	const std::uint32_t ifdOffset = detail::readU32(bytes + 4);
	if (ifdOffset > map.file_.size() - 2)
		throw Error("TIFF: IFD offset past end of file");
	const std::uint16_t entryCount = detail::readU16(bytes + ifdOffset);
	const std::uint64_t entriesEnd = static_cast<std::uint64_t>(ifdOffset) + 2
		+ 12ull * entryCount;
	if (entriesEnd > map.file_.size())
		throw Error("TIFF: IFD past end of file");

	std::vector<Tag> tags;
	tags.reserve(entryCount);
	for (std::uint16_t i = 0; i < entryCount; ++i)
	{
		const std::byte* entry = bytes + ifdOffset + 2 + 12ull * i;
		tags.push_back(Tag{
			detail::readU16(entry),
			detail::readU16(entry + 2),
			detail::readU32(entry + 4),
			detail::readU32(entry + 8),
		});
	}

	const std::uint32_t width = tagUint32(tags, 256, "ImageWidth");
	const std::uint32_t rows = tagUint32(tags, 257, "ImageLength");
	const std::uint32_t bits = tagUint32(tags, 258, "BitsPerSample");
	const std::uint32_t compression = tagUint32(tags, 259, "Compression");
	const std::uint32_t stripOffset = tagUint32(tags, 273, "StripOffsets");
	const std::uint32_t samplesPerPixel = tagUint32(tags, 277, "SamplesPerPixel");
	const std::uint32_t stripByteCount = tagUint32(tags, 279, "StripByteCounts");

	// TERRAIN_FORMAT.md: one uncompressed strip of 16-bit samples.
	if (bits != 16 || compression != 1 || samplesPerPixel != 1)
		throw Error("TIFF: expected 16-bit uncompressed single-sample pixels");
	if (width == 0 || rows == 0)
		throw Error("TIFF: empty image");
	const std::uint64_t expectedBytes = static_cast<std::uint64_t>(width) * rows * 2;
	if (stripByteCount != expectedBytes)
		throw Error("TIFF: strip byte count does not match width*height*2");
	if (static_cast<std::uint64_t>(stripOffset) + stripByteCount > map.file_.size())
		throw Error("TIFF: pixel strip past end of file");

	map.width_ = static_cast<int>(width);
	map.rows_ = static_cast<int>(rows);
	map.pixelOffset_ = stripOffset;
	map.pixels_ = reinterpret_cast<const std::uint8_t*>(map.file_.data());
	return map;
}

Heightmap::Heightmap(Heightmap&& other) noexcept
	: path_(std::move(other.path_))
	, file_(std::move(other.file_))
	, pixels_(other.pixels_)
	, pixelOffset_(other.pixelOffset_)
	, width_(other.width_)
	, rows_(other.rows_)
{
	other.pixels_ = nullptr;
	other.pixelOffset_ = 0;
	other.width_ = 0;
	other.rows_ = 0;
}

Heightmap& Heightmap::operator=(Heightmap&& other) noexcept
{
	if (this != &other)
	{
		path_ = std::move(other.path_);
		file_ = std::move(other.file_);
		pixels_ = other.pixels_;
		pixelOffset_ = other.pixelOffset_;
		width_ = other.width_;
		rows_ = other.rows_;
		other.pixels_ = nullptr;
		other.pixelOffset_ = 0;
		other.width_ = 0;
		other.rows_ = 0;
	}
	return *this;
}

Heightmap::~Heightmap() = default;

std::uint16_t Heightmap::rawSample(int col, int row) const
{
	if (col < 0 || col >= width_ || row < 0 || row >= rows_)
		throw Error("heightmap sample out of range");
	const std::size_t offset = pixelOffset_
		+ 2 * (static_cast<std::size_t>(row) * static_cast<std::size_t>(width_)
			+ static_cast<std::size_t>(col));
	return static_cast<std::uint16_t>(pixels_[offset])
		| static_cast<std::uint16_t>(pixels_[offset + 1] << 8);
}

double Heightmap::sampleHeight(int col, int row) const
{
	return rawSample(col, row) * kHeightScale;
}

double Heightmap::terrainHeight(double x, double z) const
{
	if (!std::isfinite(x) || !std::isfinite(z))
		throw Error("heightmap: non-finite coordinate");

	const double maxX = static_cast<double>(width_ - 1);
	const double maxZ = static_cast<double>(rows_ - 1);
	const double fx = std::clamp((x - kWorldMin) / kHeightmapSpacing, 0.0, maxX);
	const double fz = std::clamp((z - kWorldMin) / kHeightmapSpacing, 0.0, maxZ);
	const int col0 = static_cast<int>(std::floor(fx));
	const int row0 = static_cast<int>(std::floor(fz));
	const int col1 = std::min(col0 + 1, width_ - 1);
	const int row1 = std::min(row0 + 1, rows_ - 1);
	const double tx = fx - col0;
	const double tz = fz - row0;

	double raw;
	if (tx + tz <= 1.0)
	{
		const double h00 = rawSample(col0, row0);
		raw = h00 + (rawSample(col1, row0) - h00) * tx + (rawSample(col0, row1) - h00) * tz;
	} else {
		const double h11 = rawSample(col1, row1);
		raw = h11 + (rawSample(col0, row1) - h11) * (1.0 - tx) + (rawSample(col1, row0) - h11) * (1.0 - tz);
	}
	return raw * kHeightScale;
}

double Heightmap::height(double x, double z) const
{
	if (!std::isfinite(x) || !std::isfinite(z))
		throw Error("heightmap: non-finite coordinate");

	// TERRAIN_FORMAT.md "Image <-> world mapping": sample i sits at
	// -147456 + 18*i. The doc leaves vertex vs. centre unresolved; the
	// COLLISION_FORMAT.md section 4 example (terrain 1,948.9) matches the
	// vertex reading. UNSURE: centre placement would shift every value by 9.
	const double maxX = static_cast<double>(width_ - 1);
	const double maxZ = static_cast<double>(rows_ - 1);
	const double fx = std::clamp((x - kWorldMin) / kHeightmapSpacing, 0.0, maxX);
	const double fz = std::clamp((z - kWorldMin) / kHeightmapSpacing, 0.0, maxZ);

	const int col0 = static_cast<int>(std::floor(fx));
	const int row0 = static_cast<int>(std::floor(fz));
	const int col1 = std::min(col0 + 1, width_ - 1);
	const int row1 = std::min(row0 + 1, rows_ - 1);
	const double tx = fx - col0;
	const double tz = fz - row0;

	const double h00 = rawSample(col0, row0);
	const double h10 = rawSample(col1, row0);
	const double h01 = rawSample(col0, row1);
	const double h11 = rawSample(col1, row1);
	const double raw = (h00 * (1.0 - tx) + h10 * tx) * (1.0 - tz)
		+ (h01 * (1.0 - tx) + h11 * tx) * tz;
	return raw * kHeightScale;
}

} // namespace kworld
