#include "dds.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

struct Fmt { sg_pixel_format format; int blockBytes; };

// DXGI_FORMAT: 70-72 BC1 (typeless/unorm/srgb), 73-75 BC2, 76-78 BC3. sokol has no BC2 sRGB or BC1 sRGB format, so sRGB BC1/BC2 are
// uploaded as the linear format (the shader then sees the stored values; the difference is small for diffuse maps).
bool dxgiFormat(uint32_t dxgi, Fmt& f)
{
	if (dxgi >= 70 && dxgi <= 72) { f = { SG_PIXELFORMAT_BC1_RGBA, 8 }; }
	else if (dxgi >= 73 && dxgi <= 75) { f = { SG_PIXELFORMAT_BC2_RGBA, 16 }; }
	else if (dxgi == 76 || dxgi == 77) { f = { SG_PIXELFORMAT_BC3_RGBA, 16 }; }
	else if (dxgi == 78) { f = { SG_PIXELFORMAT_BC3_SRGBA, 16 }; }
	else { return false; }
	return true;
}

size_t levelBytes(int w, int h, int blockBytes) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * blockBytes; }

}

bool dds_load(const std::string& path, DdsImage& out, std::string& err)
{
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) { err = "cannot open " + path; return false; }
	std::vector<uint8_t> file((size_t)in.tellg());
	in.seekg(0);
	in.read((char*)file.data(), (std::streamsize)file.size());
	// "DDS " + 124-byte header: height@12 width@16 mips@28, pixel format flags@80 fourcc@84; "DX10" adds a 20-byte header (dxgi format first).
	if (file.size() < 128 || memcmp(file.data(), "DDS ", 4) != 0) { err = path + ": not a DDS file"; return false; }
	const uint8_t* h = file.data();
	size_t offset = 128;
	Fmt f;
	if (memcmp(h + 84, "DXT1", 4) == 0) { f = { SG_PIXELFORMAT_BC1_RGBA, 8 }; }
	else if (memcmp(h + 84, "DXT3", 4) == 0) { f = { SG_PIXELFORMAT_BC2_RGBA, 16 }; }
	else if (memcmp(h + 84, "DXT5", 4) == 0) { f = { SG_PIXELFORMAT_BC3_RGBA, 16 }; }
	else if (memcmp(h + 84, "DX10", 4) == 0 && file.size() >= 148 && dxgiFormat(rd32(h + 128), f)) { offset = 148; }
	else { err = path + ": unsupported DDS pixel format (only BC1-BC3)"; return false; }
	out.height = (int)rd32(h + 12);
	out.width = (int)rd32(h + 16);
	out.mips = (int)std::max<uint32_t>(1, rd32(h + 28));
	out.format = f.format;
	if (out.width <= 0 || out.height <= 0 || out.width > 16384 || out.height > 16384) { err = path + ": bad dimensions"; return false; }
	size_t total = 0;
	int mips = 0;
	for (int w = out.width, hh = out.height; mips < out.mips; mips++)
	{
		const size_t lb = levelBytes(w, hh, f.blockBytes);
		if (offset + total + lb > file.size()) { break; }	// truncated chain: keep the levels that are there
		total += lb;
		w = std::max(1, w / 2);
		hh = std::max(1, hh / 2);
	}
	if (mips == 0) { err = path + ": no pixel data"; return false; }
	out.mips = mips;
	out.data.assign(file.begin() + (std::ptrdiff_t)offset, file.begin() + (std::ptrdiff_t)(offset + total));
	return true;
}

namespace
{
// The levels sokol gets: the top ones over the GPU limit dropped (skip), each with its offset into dds.data.
struct Level { int w, h; size_t offset, bytes; };
int dds_levels(const DdsImage& dds, std::vector<Level>& out)
{
	int skip = 0;
	while (skip + 1 < dds.mips && (std::max(dds.width, dds.height) >> skip) > sg_query_limits().max_image_size_2d) { skip++; }
	const int blockBytes = dds.format == SG_PIXELFORMAT_BC1_RGBA ? 8 : 16;
	size_t offset = 0;
	for (int i = 0, w = dds.width, h = dds.height; i < dds.mips; i++)
	{
		const size_t lb = levelBytes(w, h, blockBytes);
		if (i >= skip) { out.push_back({ w, h, offset, lb }); }
		offset += lb;
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return skip;
}

sg_image_desc dds_desc(const DdsImage& dds, const std::vector<Level>& lv, int skip, const char* label)
{
	sg_image_desc id = {};
	id.width = std::max(1, dds.width >> skip);
	id.height = std::max(1, dds.height >> skip);
	id.num_mipmaps = (int)lv.size();
	id.pixel_format = dds.format;
	id.label = label;
	return id;
}
}

sg_image dds_make_image(const DdsImage& dds, const char* label)
{
	std::vector<Level> lv;
	const int skip = dds_levels(dds, lv);
	sg_image_desc id = dds_desc(dds, lv, skip, label);
	id.data = { dds.data.data() + lv[0].offset, dds.data.size() - lv[0].offset };
	return sg_make_image(&id);
}

namespace { size_t gSlice = 4u << 20; }
void dds_set_slice(size_t bytes) { gSlice = bytes; }
size_t dds_slice() { return gSlice; }

void dds_queue_image(std::shared_ptr<const DdsImage> dds, const char* label, size_t sliceBytes, const DdsPush& push, std::function<void(sg_image)> done)
{
	std::vector<Level> lv;
	const int skip = dds_levels(*dds, lv);
	size_t total = 0;
	for (const Level& l : lv) { total += l.bytes; }
	if (sliceBytes == 0 || total <= 2 * sliceBytes)	// small: one create, as dds_make_image
	{
		push(total, [dds, label, done = std::move(done)] { done(dds_make_image(*dds, label)); });
		return;
	}
	// The pieces, each at most sliceBytes (a strip of one level's block rows, or several whole small levels), in level order.
	struct Write { int level, y, rows; size_t offset, bytes; };
	std::vector<std::vector<Write>> pieces(1);
	size_t pieceBytes = 0;
	const int blockBytes = dds->format == SG_PIXELFORMAT_BC1_RGBA ? 8 : 16;
	for (int i = 0; i < (int)lv.size(); i++)
	{
		const Level& l = lv[i];
		const size_t rowPitch = (size_t)((l.w + 3) / 4) * blockBytes;	// one row of 4x4 blocks
		const int blockRows = (l.h + 3) / 4;
		const int perStrip = std::max<int>(1, (int)(sliceBytes / rowPitch));
		for (int br = 0; br < blockRows;)
		{
			int n = std::min(blockRows - br, perStrip);
			if (pieceBytes > 0 && pieceBytes + n * rowPitch > sliceBytes)
			{
				pieces.emplace_back();
				pieceBytes = 0;
				n = std::min(blockRows - br, perStrip);
			}
			const int y = br * 4, rows = std::min(l.h - y, n * 4);
			pieces.back().push_back({ i, y, rows, l.offset + br * rowPitch, n * rowPitch });
			pieceBytes += n * rowPitch;
			br += n;
		}
	}
	auto img = std::make_shared<sg_image>();
	const sg_image_desc desc = dds_desc(*dds, lv, skip, label);
	for (size_t p = 0; p < pieces.size(); p++)
	{
		size_t bytes = 0;
		for (const Write& w : pieces[p]) { bytes += w.bytes; }
		const bool first = p == 0, last = p + 1 == pieces.size();
		std::function<void(sg_image)> fin = last ? std::move(done) : nullptr;
		push(bytes, [dds, img, desc, first, ws = std::move(pieces[p]), lv, fin = std::move(fin)]
		{
			if (first)	// the storage, no data: an unsealed immutable image (sokol "ON POPULATING IMMUTABLE RESOURCES")
			{
				sg_image_desc d = desc;
				d.usage.write_unsealed = true;
				*img = sg_make_image(&d);
			}
			const bool open = sg_query_image_state(*img) == SG_RESOURCESTATE_UNSEALED;
			for (const Write& w : ws)
			{
				if (!open) { break; }
				sg_write_image_desc wd = {};
				wd.src.data = { dds->data.data() + w.offset, w.bytes };
				wd.src.bytes_per_row = (int)(w.bytes / (size_t)((w.rows + 3) / 4));
				wd.src.bytes_per_slice = (int)w.bytes;
				wd.dst.image = *img;
				wd.dst.mip_level = w.level;
				wd.dst.y = w.y;
				wd.size.width = lv[w.level].w;
				wd.size.height = w.rows;
				wd.size.num_slices = 1;
				sg_write_image_unsealed(&wd);
			}
			if (fin)
			{
				if (open) { sg_seal_image(*img); }
				fin(*img);
			}
		});
	}
}

// dds.hpp
std::string resolve_ci(const std::string& root, std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	std::string cur = root;
	size_t pos = 0;
	while (pos < path.size())
	{
		size_t e = path.find('/', pos);
		if (e == std::string::npos) { e = path.size(); }
		const std::string part = path.substr(pos, e - pos);
		pos = e + 1;
		if (part.empty() || part == ".") { continue; }
		std::string lp = part;
		for (char& c : lp) { c = (char)tolower((unsigned char)c); }
		std::error_code ec;
		std::string found;
		for (const std::filesystem::directory_entry& d : std::filesystem::directory_iterator(cur, ec))
		{
			std::string n = d.path().filename().string();
			for (char& c : n) { c = (char)tolower((unsigned char)c); }
			if (n == lp) { found = d.path().filename().string(); break; }
		}
		if (found.empty()) { return ""; }
		cur += "/" + found;
	}
	return cur;
}

namespace
{

// BC1 colour block (the 8 bytes at b) -> 16 RGBA texels; four = BC2/BC3's colour block, always 4-colour mode
void decode_colour(const uint8_t* b, bool four, uint8_t out[16][4])
{
	const unsigned c[2] = { (unsigned)(b[0] | b[1] << 8), (unsigned)(b[2] | b[3] << 8) };
	int pal[4][4];
	for (int k = 0; k < 2; k++)
	{
		pal[k][0] = (int)((c[k] >> 11 & 31) * 255 + 15) / 31;
		pal[k][1] = (int)((c[k] >> 5 & 63) * 255 + 31) / 63;
		pal[k][2] = (int)((c[k] & 31) * 255 + 15) / 31;
		pal[k][3] = 255;
	}
	for (int ch = 0; ch < 4; ch++)
	{
		if (four || c[0] > c[1])
		{
			pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
			pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
		} else {
			pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
			pal[3][ch] = 0;
		}
	}
	const uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
	for (int t = 0; t < 16; t++)
	{
		for (int ch = 0; ch < 4; ch++) { out[t][ch] = (uint8_t)pal[idx >> (2 * t) & 3][ch]; }
	}
}

}

bool dds_decode(const DdsImage& d, int level, std::vector<uint8_t>& rgba, int& w, int& h)
{
	const bool bc1 = d.format == SG_PIXELFORMAT_BC1_RGBA;
	const bool bc2 = d.format == SG_PIXELFORMAT_BC2_RGBA;
	if ((!bc1 && !bc2 && d.format != SG_PIXELFORMAT_BC3_RGBA) || level >= d.mips) { return false; }
	const size_t bb = bc1 ? 8 : 16;
	size_t off = 0;
	for (int l = 0; l < level; l++) { off += (size_t)std::max(1, ((d.width >> l) + 3) / 4) * std::max(1, ((d.height >> l) + 3) / 4) * bb; }
	w = std::max(1, d.width >> level);
	h = std::max(1, d.height >> level);
	const int bw = (w + 3) / 4, bh = (h + 3) / 4;
	if (off + (size_t)bw * bh * bb > d.data.size()) { return false; }
	rgba.assign((size_t)w * h * 4, 0);
	for (int by = 0; by < bh; by++)
	{
		for (int bx = 0; bx < bw; bx++)
		{
			const uint8_t* b = &d.data[off + ((size_t)by * bw + bx) * bb];
			uint8_t t[16][4];
			decode_colour(bc1 ? b : b + 8, !bc1, t);
			if (bc2)
			{
				for (int i = 0; i < 16; i++) { t[i][3] = (uint8_t)((b[i / 2] >> (4 * (i & 1)) & 15) * 17); }
			}
			else if (!bc1)
			{
				int a[8] = { b[0], b[1] };
				for (int i = 2; i < 8; i++) { a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7 : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255; }
				uint64_t bits = 0;
				for (int i = 0; i < 6; i++) { bits |= (uint64_t)b[2 + i] << (8 * i); }
				for (int i = 0; i < 16; i++) { t[i][3] = (uint8_t)a[bits >> (3 * i) & 7]; }
			}
			for (int i = 0; i < 16; i++)
			{
				const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
				if (x < w && y < h) { memcpy(&rgba[((size_t)y * w + x) * 4], t[i], 4); }
			}
		}
	}
	return true;
}
