#include "assets/imagewrite.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "stb_image_write.h"
#if defined(KCLIENT_WEBP)
#include <webp/encode.h>
#endif

namespace imagewrite
{
namespace
{
std::atomic<bool> gWebp{ true };
bool endsWith(const std::string& s, const char* e) { const size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
}

bool hasWebp()
{
#if defined(KCLIENT_WEBP)
	return true;
#else
	return false;
#endif
}

void setWebp(bool on) { gWebp = on; }
const char* defaultExt() { return hasWebp() && gWebp ? ".webp" : ".png"; }

std::string stripExt(const std::string& path)
{
	if (endsWith(path, ".png")) { return path.substr(0, path.size() - 4); }
	if (endsWith(path, ".webp")) { return path.substr(0, path.size() - 5); }
	return path;
}

#if defined(KCLIENT_WEBP)
// cwebp -z level of a lossless shot. 1 = ~0.5 s at 2560x1440, ~4% larger than -z 9 (12 s there: the F12 freeze, kenshi-k1c6).
static constexpr int kWebpLevel = 1;

static bool writeWebp(const std::string& path, int w, int h, const unsigned char* rgba)
{
	std::vector<unsigned char> top((size_t)w * h * 4);	// top row first
	for (int y = 0; y < h; y++) { std::memcpy(&top[(size_t)y * w * 4], rgba + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4); }
	WebPConfig cfg;
	if (!WebPConfigInit(&cfg)) { return false; }
	if (!WebPConfigLosslessPreset(&cfg, kWebpLevel)) { return false; }	// lossless, effort kWebpLevel (cwebp -z N): a shot never stalls a frame
	cfg.exact = 1;		// the RGB of alpha 0 pixels stays
	WebPPicture pic;
	if (!WebPPictureInit(&pic)) { return false; }
	pic.use_argb = 1;
	pic.width = w;
	pic.height = h;
	WebPMemoryWriter mw;
	WebPMemoryWriterInit(&mw);
	pic.writer = WebPMemoryWrite;
	pic.custom_ptr = &mw;
	bool ok = WebPPictureImportRGBA(&pic, top.data(), w * 4) && WebPEncode(&cfg, &pic);
	WebPPictureFree(&pic);
	if (ok)
	{
		errno = 0;
		FILE* f = fopen(path.c_str(), "wb");
		ok = f && fwrite(mw.mem, 1, mw.size, f) == mw.size;
		if (f && fclose(f) != 0) { ok = false; }
	}
	WebPMemoryWriterClear(&mw);
	return ok;
}
#endif

bool write(const std::string& path, int w, int h, const unsigned char* rgba)
{
#if defined(KCLIENT_WEBP)
	if (endsWith(path, ".webp")) { return writeWebp(path, w, h, rgba); }
#endif
	static std::mutex m;	// stbi_flip_vertically_on_write is a process global: the screenshot writer, the anim lab and lastframe write from different threads
	std::lock_guard<std::mutex> lock(m);
	stbi_flip_vertically_on_write(1);
	return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}
}
