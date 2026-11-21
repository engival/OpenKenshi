// imagewrite's round trip (kenshi-k16.2.470.1): a bottom-first RGBA buffer written as .webp (lossless, exact) and as .png decodes back to the
// same pixels, top row first, alpha 0 pixels keeping their RGB. No sokol, no window.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "assets/imagewrite.hpp"
#include "stb_image.h"
#if defined(KCLIENT_WEBP)
#include <webp/decode.h>
#endif

int main()
{
	const int w = 97, h = 53;	// odd sizes: no block alignment hides a row error
	std::vector<unsigned char> px((size_t)w * h * 4);
	unsigned s = 12345;
	for (size_t i = 0; i < px.size(); i++) { s = s * 1664525u + 1013904223u; px[i] = (unsigned char)(i % 7 < 3 ? (s >> 24) : (i / 4) % 251); }
	for (size_t i = 0; i < (size_t)w * h; i += 5) { px[i * 4 + 3] = 0; }	// alpha 0 with RGB set: exact keeps it
	for (size_t i = 1; i < (size_t)w * h; i += 5) { px[i * 4 + 3] = 255; }
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kimagewrite_test";
	std::filesystem::create_directories(dir);
	int bad = 0;
	const auto top = [&](int y) { return &px[(size_t)(h - 1 - y) * w * 4]; };	// the written image's row y is the buffer's row h-1-y
	{
		const std::string path = (dir / "t.png").string();
		if (!imagewrite::write(path, w, h, px.data())) { fprintf(stderr, "png write failed\n"); return 1; }
		int iw, ih, n;
		unsigned char* d = stbi_load(path.c_str(), &iw, &ih, &n, 4);
		if (!d || iw != w || ih != h) { fprintf(stderr, "png decode failed\n"); return 1; }
		for (int y = 0; y < h; y++) { for (int x = 0; x < w * 4; x++) { if (d[(size_t)y * w * 4 + x] != top(y)[x] && !(x % 4 != 3 && d[(size_t)y * w * 4 + x - x % 4 + 3] == 0)) { bad++; } } }
		stbi_image_free(d);
		printf("png: %s\n", bad ? "MISMATCH" : "round trip equal");
	}
#if defined(KCLIENT_WEBP)
	{
		const std::string path = (dir / "t.webp").string();
		if (!imagewrite::write(path, w, h, px.data())) { fprintf(stderr, "webp write failed\n"); return 1; }
		FILE* f = fopen(path.c_str(), "rb");
		std::vector<unsigned char> file(1 << 20);
		const size_t len = f ? fread(file.data(), 1, file.size(), f) : 0;
		if (f) { fclose(f); }
		int iw = 0, ih = 0;
		unsigned char* d = WebPDecodeRGBA(file.data(), len, &iw, &ih);
		if (!d || iw != w || ih != h) { fprintf(stderr, "webp decode failed\n"); return 1; }
		int wbad = 0;
		for (int y = 0; y < h; y++) { for (int x = 0; x < w * 4; x++) { if (d[(size_t)y * w * 4 + x] != top(y)[x]) { wbad++; } } }	// every byte, alpha 0 pixels' RGB too
		WebPFree(d);
		printf("webp: %s (%zu bytes)\n", wbad ? "MISMATCH" : "round trip equal, alpha 0 RGB kept", len);
		bad += wbad;
	}
#else
	printf("webp: not built (no libwebp)\n");
#endif
	std::filesystem::remove_all(dir);
	return bad ? 1 : 0;
}
