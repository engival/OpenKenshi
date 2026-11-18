#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sokol_gfx.h"

// DDS loader for block-compressed textures: BC1/BC2/BC3 via DXT1/DXT3/DXT5 fourccs or a DX10 header (DXGI 70-78, BC1-BC3
// incl. sRGB), full mip chain, any dimensions (blocks round up). Uncompressed files are rejected. Pixel data is kept in
// DDS order (top row first), which is also sokol's upload order, so Ogre's UVs (v = 0 at the top) sample it unflipped.
struct DdsImage
{
	int width = 0, height = 0, mips = 1;
	sg_pixel_format format = SG_PIXELFORMAT_NONE;
	std::vector<uint8_t> data;	// all mip levels, largest first, tightly packed
};

// False with err filled in when the file is missing, malformed or not a supported compressed format.
bool dds_load(const std::string& path, DdsImage& out, std::string& err);

// Creates a GPU image from the DDS (BC formats are uploaded as they are). On failure the returned image is invalid.
// Top mips are dropped when the image is larger than the GPU limit.
sg_image dds_make_image(const DdsImage& dds, const char* label);

// The same create as resource jobs (renderqueue.hpp's stream lane; kenshi-k16.2.205.12.74), split when large. push(bytes, job) queues one job (the caller
// picks the lane and the tag). An image of at most 2 * sliceBytes is one job (dds_make_image). A larger one is a run of jobs of at most sliceBytes
// each, in order: the first makes the storage (an unsealed immutable image), each writes its strip of block rows or its small whole levels
// (sg_write_image_unsealed), the last seals it. Measured on the Intel test card: a create costs about its bytes (a 4096^2 BC3, 22 MB, 3-10 ms in one
// job), so one job never holds a frame for more than a slice's worth. done(image) runs inside the last job, render side; the image is not sealed (not
// drawable) before, so nothing names it half written. A failed storage: done gets the invalid or failed image, as from dds_make_image.
// The streamed creates' slice (render.stream_slice_mb, default 4 MiB; 0 = one create per image). Logic side: set once a frame by logic_frame, read by
// the renderers' queueing calls (worldbuildings, characters) on the same thread.
void dds_set_slice(size_t bytes);
size_t dds_slice();
using DdsPush = std::function<void(size_t bytes, std::function<void()> job)>;
void dds_queue_image(std::shared_ptr<const DdsImage> dds, const char* label, size_t sliceBytes, const DdsPush& push, std::function<void(sg_image)> done);

// Decodes mip level `level` of a BC1/BC2/BC3 image to RGBA8 (top row first, tightly packed; w, h = that level's size). False for another format or a
// level the file lacks.
bool dds_decode(const DdsImage& d, int level, std::vector<uint8_t>& rgba, int& w, int& h);

// Resolves a record path (".\data\newland\land\textures\Foo_DIF.dds") below root (the install directory, the parent of data/), matching every
// component case-insensitively. "" when a component is missing.
std::string resolve_ci(const std::string& root, std::string path);
