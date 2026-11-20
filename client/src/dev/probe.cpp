// The `probe depth` script op (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7; kenshi-h2v.14.2.1): at the shot's
// view, read the scene depth the particle FS samples back on the CPU at the given pixels (top-left origin, as the PNG) and compare its zv with
// (1) the heightfield march along the same pixel ray, the ground the pixel really hits, and (2) the v_w of the particle instance under that pixel.
// Proves or disproves the depth the FS discards on. Moved off main.cpp; the camera, the drawn terrain, the HDR target and the particles
// instance arrive through dev::Context. Built only with KCLIENT_DEV_TOOLS, and only a GL build has a readable scene depth.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "render/camera.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "render/hdr.hpp"
#include "kworld/heightmap.hpp"
#include "render/particles.hpp"
#include "render/readback.hpp"

namespace dev
{

void probeOp(const Context& cx, const std::vector<std::string>& a)
{
	if (!a.empty() && a[0] == "height")	// probe height X Z (dev/heightprobe.cpp, kenshi-k16.2.384.1)
	{
		if (!heightProbeOp(cx, a)) { fprintf(stderr, "kclient: script: probe height X Z | id=N\n"); }
		return;
	}
#if defined(SOKOL_GLCORE)
	if (a.size() < 3 || a[0] != "depth" || ((a.size() - 1) % 2) != 0) { fprintf(stderr, "kclient: script: probe depth X Y [X Y ...]\n"); return; }
	const double ground = cx.hm->height(cx.pos[0], cx.pos[2]);
	const float nearZ = (float)std::fmax(0.5, std::fmin(300.0, (cx.pos[1] - ground) * 0.05));
	Camera cam;
	cx.currentCamera(nearZ, 700000.0f, cam);
	std::vector<std::pair<int, int>> reads;	// the pixels in range, GL origin bottom-left
	for (size_t i = 1; i + 1 < a.size(); i += 2)
	{
		const int px = atoi(a[i].c_str()), py = atoi(a[i + 1].c_str());
		if (px >= 0 && py >= 0 && px < cam.width && py < cam.height) { reads.push_back({ px, cam.height - 1 - py }); }
	}
	// a request (kenshi-k16.2.205.12.23): the draw half reads the scene depth as the frame drew it, after its last pass; the image is looked up then
	readback::probe([&cx](bool& colour) { return cx.hdr.sceneDepthImage(&colour); }, std::move(reads), [a, cam, &cx](bool ok, const std::vector<float>& depths, const readback::ProbeInfo& info) {
	if (!ok) { printf("probe: no scene depth\n"); return; }
	const bool colour = info.colour;
	printf("probe: scene depth (%s) tex %u, %dx%d, near %.2f far %.0f, read FBO %s\n", colour ? "the R32F opaque copy" : "the depth attachment", info.tex, cam.width, cam.height,
		cam.nearZ, cam.farZ, info.complete ? "complete" : "INCOMPLETE");
	size_t next = 0;
	for (size_t i = 1; i + 1 < a.size(); i += 2)
	{
		const int px = atoi(a[i].c_str()), py = atoi(a[i + 1].c_str());
		if (px < 0 || py < 0 || px >= cam.width || py >= cam.height) { printf("probe px %d %d: off the %dx%d target\n", px, py, cam.width, cam.height); continue; }
		const int gy = cam.height - 1 - py;	// GL's origin is bottom-left
		const float d = depths[next++];
		// particles.glsl: the same zv formula and the same 0.99999 far case
		const double n = cam.nearZ, f = cam.farZ;
		const double zv = d >= 0.99999f ? f : 2.0 * n * f / ((f + n) - (2.0 * d - 1.0) * (f - n));
		// the pixel's ray (Camera::mvp: x = ndcx tanY aspect, y = ndcy tanY, forward 1 in the camera's basis)
		const double ndcx = 2.0 * ((double)px + 0.5) / cam.width - 1.0;
		const double ndcy = 2.0 * ((double)gy + 0.5) / cam.height - 1.0;
		double dir[3] = {}, len = 0;
		for (int k = 0; k < 3; k++) { dir[k] = cam.f[k] + cam.r[k] * ndcx * cam.tanY * cam.aspect + cam.u[k] * ndcy * cam.tanY; len += dir[k] * dir[k]; }
		len = std::sqrt(len);
		for (int k = 0; k < 3; k++) { dir[k] /= len; }
		const double fwdDot = dir[0] * cam.f[0] + dir[1] * cam.f[1] + dir[2] * cam.f[2];
		// the CPU heightfield march: the first point under the terrain, then a bisect to the crossing
		double tHit = -1.0;
		double t = 1.0, step = 2.0;
		for (; t < 700000.0; t += step, step *= 1.02)
		{
			if (cam.pos[1] + dir[1] * t < cx.hm->height(cam.pos[0] + dir[0] * t, cam.pos[2] + dir[2] * t))
			{
				double lo = t - step, hi = t;
				for (int it = 0; it < 40; it++)
				{
					const double m = 0.5 * (lo + hi);
					if (cam.pos[1] + dir[1] * m < cx.hm->height(cam.pos[0] + dir[0] * m, cam.pos[2] + dir[2] * m)) { hi = m; }
					else { lo = m; }
				}
				tHit = 0.5 * (lo + hi);
				break;
			}
		}
		const Particles::PixelHit hit = cx.particles.probePixel(cam, px, py);
		printf("probe px %d %d: depth %.6f zv %.1f", px, py, d, zv);
		if (tHit > 0)
		{
			const double gx = cam.pos[0] + dir[0] * tHit, gyy = cam.pos[1] + dir[1] * tHit, gz = cam.pos[2] + dir[2] * tHit;
			printf(" | cpu terrain %.0f %.0f %.0f along %.1f fwd %.1f (zv - terrain %+.1f)", gx, gyy, gz, tHit, tHit * fwdDot, zv - tHit * fwdDot);
		}
		else { printf(" | cpu terrain: no ground hit along the ray"); }
		if (hit.any)
		{
			printf(" | particle %s at %.0f %.0f %.0f v_w %.1f (screen %.1f %.1f, half %.1f x %.1f; v_w - zv %+.1f)", hit.covers ? "covering" : "nearest (not covering)",
				hit.world[0], hit.world[1], hit.world[2], hit.w, hit.sx, hit.sy, hit.halfW, hit.halfH, hit.w - zv);
		}
		else { printf(" | particle: none in front of the camera"); }
		printf("\n");
	}
	fflush(stdout);
	});
#else
	(void)cx;
	(void)a;
	fprintf(stderr, "kclient: script: probe depth needs a GL dev build (KCLIENT_DEV_TOOLS + SOKOL_GLCORE)\n");
#endif
}

}	// namespace dev
