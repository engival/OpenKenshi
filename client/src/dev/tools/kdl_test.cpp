// The 2D draw list's unit check (drawlist.hpp, render hook 8, kenshi-k16.2.205.12.28): the recorder only, no sokol, no window.
//   kdl_test   exit 0 and "kdl_test: ok" when every check holds
#include <cstdio>
#include <thread>
#include <vector>

#include "drawlist.hpp"

namespace
{
int gFails = 0;

void check(bool ok, const char* what)
{
	if (!ok) { fprintf(stderr, "kdl_test: FAIL %s\n", what); gFails++; }
}

// sokol_gl.h's _sgl_pack_rgbaf / _sgl_pack_rgbab, written out from the header: dl::pack must store the byte sgl_c4f would.
uint32_t sglPack(float r, float g, float b, float a)
{
	const auto cl = [](float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; };
	const uint8_t r8 = (uint8_t)(cl(r) * 255.0f), g8 = (uint8_t)(cl(g) * 255.0f), b8 = (uint8_t)(cl(b) * 255.0f), a8 = (uint8_t)(cl(a) * 255.0f);
	return (uint32_t)(((uint32_t)a8 << 24) | ((uint32_t)b8 << 16) | ((uint32_t)g8 << 8) | r8);
}

bool same(const dl::Rect& a, const dl::Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
}

int main()
{
	// Colour packing: sgl's truncation, clamping and byte order.
	bool packOk = true;
	for (float v : { -1.0f, 0.0f, 0.001f, 0.25f, 0.5f, 0.6f, 0.999f, 1.0f, 2.0f })
	{
		packOk = packOk && dl::pack(v, 1.0f - v, v * 0.5f, 0.7f) == sglPack(v, 1.0f - v, v * 0.5f, 0.7f);
	}
	check(packOk, "pack matches sgl's packing");
	check(dl::pack(1, 0, 0, 1) == 0xFF0000FFu, "pack: r in the low byte, a in the high byte");

	// Groups carry the state set before their begin; the screen is the first clip.
	dl::List l;
	l.begin(800, 600);
	check(l.empty() && l.width() == 800 && l.height() == 600, "begin: empty, the framebuffer size");
	check(same(l.clip(), { 0, 0, 800, 600 }), "begin: the clip is the screen");
	l.beginPrim(dl::Prim::kQuads);
	for (int i = 0; i < 4; i++) { l.vertex((float)i, 0, 0, 0, dl::pack(1, 1, 1, 1)); }
	l.endPrim();
	const dl::TexId page = dl::newTex();
	l.clip({ 10, 20, 30, 40 });
	l.texture(page);
	l.beginPrim(dl::Prim::kTriangles);
	for (int i = 0; i < 3; i++) { l.vertex(0, (float)i, 0.5f, 0.5f, 0); }
	l.endPrim();
	int hostRan = 0;
	l.host({ 1, 2, 3, 4 }, [&hostRan](float, float, float w, float) { hostRan += (int)w; });
	l.host({ 1, 2, 3, 4 }, {});	// no function: nothing recorded
	check(l.cmds().size() == 3, "two groups and one host entry");
	if (l.cmds().size() == 3)
	{
		const dl::Cmd& a = l.cmds()[0];
		const dl::Cmd& b = l.cmds()[1];
		const dl::Cmd& h = l.cmds()[2];
		check(a.kind == dl::Cmd::Kind::kDraw && a.prim == dl::Prim::kQuads && a.tex == 0 && a.first == 0 && a.count == 4, "group 1: untextured quads, vertices 0-3");
		check(same(a.clip, { 0, 0, 800, 600 }), "group 1: the screen clip");
		check(b.prim == dl::Prim::kTriangles && b.tex == page && b.first == 4 && b.count == 3, "group 2: textured triangles, vertices 4-6");
		check(same(b.clip, { 10, 20, 30, 40 }), "group 2: the clip set before it");
		check(a.layer == dl::kLayerInherit && b.layer == dl::kLayerInherit, "layer: inherited unless set");
		check(h.kind == dl::Cmd::Kind::kHost && h.host == 0 && same(h.clip, { 10, 20, 30, 40 }), "host: its index and the clip in force");
		l.hosts()[h.host].draw(0, 0, 3, 0);
		check(hostRan == 3, "host: the function recorded");
	}
	check(l.vertices().size() == 7, "seven vertices");

	// Uploads: in order, kept across begin, taken once.
	const dl::TexId icon = dl::newTex();
	check(icon != page && icon != 0 && page != 0, "newTex: fresh non-zero ids");
	l.create(page, 2, 2, std::vector<uint8_t>(16, 255), 1, dl::Filter::kLinear, true, "ui-font");
	l.update(page, 1, 1, 1, 1, std::vector<uint8_t>(4, 0));
	l.create(icon, 1, 1, std::vector<uint8_t>(4, 9), 1, dl::Filter::kTrilinear);
	l.destroy(icon);
	l.begin(800, 600);
	check(l.empty() && l.vertices().empty() && l.hosts().empty(), "begin: the draws are gone");
	check(l.uploads().size() == 4, "begin: the uploads stay until taken");
	std::vector<dl::Upload> ups = l.takeUploads();
	check(ups.size() == 4 && l.uploads().empty(), "takeUploads: all of them, once");
	if (ups.size() == 4)
	{
		check(ups[0].op == dl::Upload::Op::kCreate && ups[0].id == page && ups[0].w == 2 && ups[0].updatable && ups[0].rgba.size() == 16, "upload 1: the page's create");
		check(ups[1].op == dl::Upload::Op::kUpdate && ups[1].x == 1 && ups[1].y == 1 && ups[1].rgba.size() == 4, "upload 2: the dirty rect");
		check(ups[2].op == dl::Upload::Op::kCreate && ups[2].filter == dl::Filter::kTrilinear && !ups[2].updatable, "upload 3: the icon, trilinear");
		check(ups[3].op == dl::Upload::Op::kDestroy && ups[3].id == icon, "upload 4: the destroy");
	}

	// newTex from two threads: no id twice.
	std::vector<dl::TexId> ta, tb;
	std::thread t1([&] { for (int i = 0; i < 1000; i++) { ta.push_back(dl::newTex()); } });
	std::thread t2([&] { for (int i = 0; i < 1000; i++) { tb.push_back(dl::newTex()); } });
	t1.join();
	t2.join();
	bool unique = true;
	std::vector<bool> seen(4000, false);
	for (const auto* v : { &ta, &tb })
	{
		for (dl::TexId id : *v)
		{
			if (id >= seen.size() || seen[id]) { unique = false; continue; }
			seen[id] = true;
		}
	}
	check(unique, "newTex: unique across threads");

	if (gFails) { fprintf(stderr, "kdl_test: %d failed\n", gFails); return 1; }
	printf("kdl_test: ok\n");
	return 0;
}
