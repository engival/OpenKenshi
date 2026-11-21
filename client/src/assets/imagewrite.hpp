#pragma once
#include <string>

// The one image writer (kenshi-k16.2.470.1): RGBA8 rows bottom-first (as glReadPixels returns them) to a file, the format by the path's
// extension: ".webp" = lossless WebP (Linux builds with libwebp only: our own dev tooling, kenshi-k16.2.470), anything else = PNG.
namespace imagewrite
{
// true on success; errno is the file system's reason when a file could not be opened.
bool write(const std::string& path, int w, int h, const unsigned char* rgba);
// ".webp" when this build can write it and screenshot.format allows it, else ".png": the default extension of a shot nobody named.
const char* defaultExt();
void setWebp(bool on);	// screenshot.format: webp (default) / png
bool hasWebp();
// base without a trailing .png / .webp
std::string stripExt(const std::string& path);
}
