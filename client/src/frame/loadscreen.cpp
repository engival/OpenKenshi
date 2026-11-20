#define STB_TRUETYPE_IMPLEMENTATION
#include "frame/loadscreen.hpp"
#include "render/drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "app/config.hpp"
#include "assets/dds.hpp"
#include "render/renderqueue.hpp"
#include "sokol_app.h"
#include "sokol_gl.h"
#include "stb_image.h"
#include "ui/skin.hpp"
#include "world/worldmap.hpp"
#include "platform/windowgate.hpp"

namespace
{

constexpr float kUiW = 1920, kUiH = 1080;	// Kenshi_UI.png size
// 9-slice skins in gui/skins/kenshi_skins.xml: texture rect, then the corner sizes (left, right, top, bottom). The layout places the floating panel at
// 0.34375,0.657407 with size 0.3125 x 0.277778 of the screen, and the tip box inside it at 0.00833,0.3167 size 0.9833 x 0.6667.
constexpr LoadScreen::Skin kPanelSkin = { 1074, 367, 246, 172, 76, 88, 43, 44 };	// Kenshi_FloatingPanelSkin
constexpr LoadScreen::Skin kTextSkin = { 341, 446, 186, 102, 33, 31, 33, 27 };	// Kenshi_GenericTextBoxSkin (parchment)

// Our own UI style is the default, so the loading text is smoothed like the rest of the UI; the game's hard-edged glyphs (Antialias false in
// kenshi_fonts.xml) belong to ui.style=original only (k16.2.221.5.1). ui.loading_aa is gone.
bool originalStyle()
{
	for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { return i.value == "original"; } }
	return false;
}
[[maybe_unused]] const bool kRetiredLoadingAa = (cfg::retire("ui.loading_aa", "ui.loading_aa is gone (k16.2.221.5.1): the loading text is smoothed like our UI; ui.style=original keeps the game's hard-edged glyphs"), true);

// A MyGUI TrueType font entry of kenshi_fonts.xml: its source file, its pixel size at 1080 lines (Size * Resolution / 72) and Antialias.
struct FontEntry { std::string source; float px1080 = 0; bool smooth = true; };

FontEntry fontEntry(const std::string& xml, const std::string& name, const char* defSource, float defSize)
{
	FontEntry e{ defSource, defSize * 50.0f / 72.0f, false };
	const size_t at = xml.find("name=\"" + name + "\"");
	if (at == std::string::npos) { fprintf(stderr, "kclient: loading screen: %s not in kenshi_fonts.xml, defaults\n", name.c_str()); return e; }
	const size_t end = xml.find("</Resource>", at);
	auto prop = [&](const char* key) -> std::string
	{
		const size_t k = xml.find(std::string("key=\"") + key + "\"", at);
		if (k == std::string::npos || k > end) { return {}; }
		const size_t v = xml.find("value=\"", k);
		if (v == std::string::npos || v > end) { return {}; }
		return xml.substr(v + 7, xml.find('"', v + 7) - (v + 7));
	};
	const std::string src = prop("Source"), size = prop("Size"), res = prop("Resolution"), aa = prop("Antialias");
	if (!src.empty()) { e.source = src; }
	const float sz = size.empty() ? defSize : (float)atof(size.c_str()), r = res.empty() ? 72.0f : (float)atof(res.c_str());
	e.px1080 = sz * r / 72.0f;
	e.smooth = aa != "false";
	return e;
}

void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
	sgl_v2f_t2f(x0, y0, u0, v0); sgl_v2f_t2f(x1, y0, u1, v0); sgl_v2f_t2f(x1, y1, u1, v1); sgl_v2f_t2f(x0, y1, u0, v1);
}

}

// em: the em size in pixels, as FreeType's char size (MyGUI's font Size); stb bakes by ascent-to-descent height, so that height is derived.
bool LoadScreen::Font::bake(const std::string& path, float em, bool smooth, std::vector<uint8_t>& rgba)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (ttf.empty()) { return false; }
	const uint8_t* data = (const uint8_t*)ttf.data();
	stbtt_fontinfo fi;
	if (!stbtt_InitFont(&fi, data, stbtt_GetFontOffsetForIndex(data, 0))) { return false; }
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
	const float k = stbtt_ScaleForMappingEmToPixels(&fi, em), pixels = (asc - desc) * k;
	// The line step as MyGUI lays it out (ResourceTrueTypeFont::mDefaultHeight): the ascent and the descent in whole pixels, each rounded
	// up. This reproduces the game's own "real height" lines (MyGUI.log, 1280x720: FloaterFont_Large 16 px, its Small 12, its Medium 13,
	// LOADING 71) where stb's unrounded ascent - descent would be 1-2 px short (k16.2.221).
	lineH = std::ceil(asc * k) + std::ceil(-desc * k);
	std::vector<uint8_t> alpha((size_t)kAtlas * kAtlas);
	rgba.assign((size_t)kAtlas * kAtlas * 4, 255);
	if (stbtt_BakeFontBitmap(data, 0, pixels, alpha.data(), kAtlas, kAtlas, 32, 96, glyphs) <= 0) { return false; }
	for (size_t i = 0; i < alpha.size(); i++) { rgba[i * 4 + 3] = smooth ? alpha[i] : alpha[i] >= 128 ? 255 : 0; }	// white with the coverage as alpha
	px = pixels;
	return true;
}

void LoadScreen::Font::make(const std::vector<uint8_t>& rgba)
{
	sg_image_desc id = {};
	id.width = id.height = kAtlas;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = { rgba.data(), rgba.size() };
	id.label = "loading-font";
	img = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = img;
	view = sg_make_view(&vd);
}

void LoadScreen::Font::free()
{
	if (img.id == SG_INVALID_ID) { return; }
	sg_destroy_view(view);
	sg_destroy_image(img);
	img = {};
}

float LoadScreen::Font::width(const std::string& t) const
{
	float w = 0;
	for (unsigned char c : t) { if (c >= 32 && c < 128) { w += glyphs[c - 32].xadvance; } }
	return w;
}

// Text with its baseline at y, left edge x: nearest sampling, since the atlas is baked at the size it is drawn (k16.2.221).
void LoadScreen::drawText(const Font& f, float x, float y, const std::string& t, float r, float g, float b, float a)
{
	sgl_enable_texture();
	sgl_texture(f.view, gpu_.smpText);
	sgl_begin_quads();
	sgl_c4f(r, g, b, a);
	for (unsigned char c : t)
	{
		if (c < 32 || c >= 128) { continue; }
		stbtt_aligned_quad q;
		stbtt_GetBakedQuad(f.glyphs, Font::kAtlas, Font::kAtlas, c - 32, &x, &y, &q, 0);
		quad(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
	}
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
}

bool LoadScreen::init(const std::string& dataDir, sg_view shared)
{
	if (ok_) { return true; }	// one screen serves a whole load: the startup load's picture and tip stay through the session load after it (k16.2.221)
	std::vector<std::string> tips;
	std::ifstream in(dataDir + "/tips.txt");
	for (std::string l; std::getline(in, l);)
	{
		while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) { l.pop_back(); }
		if (!l.empty()) { tips.push_back(l); }
	}
	if (tips.empty()) { fprintf(stderr, "kclient: loading screen: no tips in %s/tips.txt\n", dataDir.c_str()); return false; }
	std::mt19937 rng{ std::random_device{}() };
	// Kenshi_UI.png's pixels, unless the UI layer's copy is shared (a handle the render side made; only its id travels)
	std::vector<uint8_t> ui;
	int uiW = 0, uiH = 0;
	if (shared.id == SG_INVALID_ID)
	{
		int comp;
		stbi_uc* px = stbi_load((dataDir + "/gui/gfx/Kenshi_UI.png").c_str(), &uiW, &uiH, &comp, 4);
		if (!px)
		{
			fprintf(stderr, "kclient: loading screen: Kenshi_UI.png missing under %s/gui/gfx\n", dataDir.c_str());
			return false;
		}
		ui.assign(px, px + (size_t)uiW * uiH * 4);
		stbi_image_free(px);
	}
	// One painting and one tip for the whole load, as the original (its LoadingWindow picks a random background image and shows one tip:
	// kenshi-inspect findings/loading_tip.md): both are picked here, once, and kept until shutdown(). The pick is logged so a run can check
	// the picture never changes mid-load.
	std::vector<std::string> files;
	for (int i = 0; i < 16; i++)
	{
		const std::string p = dataDir + "/gui/gfx/loading_screen" + std::to_string(i) + ".dds";
		if (std::filesystem::exists(p)) { files.push_back(p); }
	}
	int pinArt = -1, pinTip = -1;
#ifdef KCLIENT_DEV_TOOLS
	// A comparison shot needs the reference's own picture and tip (k16.2.221): KCLIENT_LOADING_PICK="ART,TIP" pins loading_screen<ART>.dds
	// and the tip's 0-based line of tips.txt. Dev builds only; unset or malformed keeps the random pick.
	if (const char* pin = getenv("KCLIENT_LOADING_PICK")) { if (sscanf(pin, "%d,%d", &pinArt, &pinTip) != 2) { pinArt = pinTip = -1; } }
#endif
	std::shuffle(files.begin(), files.end(), rng);
	if (pinArt >= 0)
	{
		const std::string want = "loading_screen" + std::to_string(pinArt) + ".dds";
		auto it = std::find_if(files.begin(), files.end(), [&](const std::string& p) { return p.size() >= want.size() && p.compare(p.size() - want.size(), want.size(), want) == 0; });
		if (it != files.end()) { std::iter_swap(files.begin(), it); }
	}
	// the painting: the first that decodes (the render side makes its image; a file sokol then refuses draws nothing, where it used to try the next)
	std::string art;
	DdsImage dds;
	for (const std::string& p : files)
	{
		std::string err;
		if (dds_load(p, dds, err)) { art = p; break; }
		fprintf(stderr, "kclient: loading screen art %s: %s\n", p.c_str(), err.c_str());
	}
	if (art.empty())
	{
		fprintf(stderr, "kclient: loading screen: no art under %s/gui/gfx\n", dataDir.c_str());
		return false;
	}
	tip_ = pinTip >= 0 && pinTip < (int)tips.size() ? tips[pinTip] : tips[rng() % tips.size()];
	printf("kclient: loading screen: picture %s, tip \"%s\"\n", std::filesystem::path(art).filename().string().c_str(), tip_.c_str());
	const float H = view::heightf();	// sizes follow the window's height (the game's layout is 1080p); a resize re-bakes them (relayout)
	// The fonts and the tip's colour from the active gui files (a theme's over the install's) when the UI layer is up.
	const bool uiLayer = shared.id != SG_INVALID_ID;
	const std::string fontsXml = uiLayer ? ui::res::file("fonts/kenshi_fonts.xml") : dataDir + "/gui/fonts/kenshi_fonts.xml";
	std::ifstream fx(fontsXml);
	const std::string xml((std::istreambuf_iterator<char>(fx)), std::istreambuf_iterator<char>());
	const FontEntry tf = fontEntry(xml, "Kenshi_UI_Messages", "MailartRubberstamp-Regular.ttf", 110), bf = fontEntry(xml, "Kenshi_FloaterFont_Large", "Exo2-SemiBold.ttf", 26);
	auto ttf = [&](const std::string& f) { return uiLayer ? ui::res::file("fonts/" + f) : dataDir + "/gui/fonts/" + f; };
	if (uiLayer)
	{
		ui::Rect a;
		ui::Color c;
		if (const ui::Skin* k = ui::res::skin("Kenshi_GenericTextBoxSkin"); k && k->text({ 0, 0, 100, 100 }, 1, ui::State::kNormal, false, a, c))
		{
			tipR_ = c.r;
			tipG_ = c.g;
			tipB_ = c.b;
		}
	}
	titlePath_ = ttf(tf.source);
	titleEm_ = tf.px1080;
	titleSmooth_ = tf.smooth || !originalStyle();
	bodyPath_ = ttf(bf.source);
	bodyEm_ = bf.px1080;
	bodySmooth_ = bf.smooth || !originalStyle();
	Baked fonts;
	if (!bakeFonts(H, fonts))
	{
		fprintf(stderr, "kclient: loading screen: fonts unusable under %s/gui/fonts\n", dataDir.c_str());
		return false;
	}
	layoutH_ = H;
	// the render side makes the screen's resources at the top of the next draw (frame lane: the frame that draws it)
	rq::push("loadscreen-init", [this, shared, ui = std::move(ui), uiW, uiH, dds = std::move(dds), fonts = std::move(fonts), tip = tip_,
		tr = tipR_, tg = tipG_, tb = tipB_]() mutable {
		freeGpu();
		if (shared.id != SG_INVALID_ID) { gpu_.uiView = shared; }
		else
		{
			sg_image_desc id = {};
			id.width = uiW;
			id.height = uiH;
			id.pixel_format = SG_PIXELFORMAT_RGBA8;
			id.data = { ui.data(), ui.size() };
			gpu_.uiImg = sg_make_image(&id);
			sg_view_desc vd = {};
			vd.texture.image = gpu_.uiImg;
			gpu_.uiView = sg_make_view(&vd);
		}
		sg_sampler_desc sd = {};
		sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
		sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
		gpu_.smp = sg_make_sampler(&sd);
		sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
		gpu_.smpText = sg_make_sampler(&sd);
		gpu_.art.img = dds_make_image(dds, "loading-art");
		gpu_.art.w = dds.width;
		gpu_.art.h = dds.height;
		sg_view_desc vd = {};
		vd.texture.image = gpu_.art.img;
		gpu_.art.view = sg_make_view(&vd);
		gpu_.title = fonts.title;
		gpu_.title.make(fonts.titleRgba);
		gpu_.body = fonts.body;
		gpu_.body.make(fonts.bodyRgba);
		gpu_.tip = std::move(tip);
		gpu_.tipR = tr;
		gpu_.tipG = tg;
		gpu_.tipB = tb;
		gpu_.ok = true;
	}, rq::Lane::kFrame, ui.size() + fonts.titleRgba.size() + fonts.bodyRgba.size());
	ok_ = true;
	return true;
}

// Both fonts at window height h, on the CPU. The live pair is swapped only when both baked (queueFonts): a resize never leaves half the text missing.
bool LoadScreen::bakeFonts(float h, Baked& out)
{
	return out.title.bake(titlePath_, h / 1080.0f * titleEm_, titleSmooth_, out.titleRgba) && out.body.bake(bodyPath_, h / 1080.0f * bodyEm_, bodySmooth_, out.bodyRgba);
}

void LoadScreen::queueFonts(Baked&& b)
{
	const size_t bytes = b.titleRgba.size() + b.bodyRgba.size();
	rq::push("loadscreen-fonts", [this, b = std::move(b)]() mutable {
		if (!gpu_.ok) { return; }	// the screen went first
		gpu_.title.free();
		gpu_.title = b.title;
		gpu_.title.make(b.titleRgba);
		gpu_.body.free();
		gpu_.body = b.body;
		gpu_.body.make(b.bodyRgba);
	}, rq::Lane::kFrame, bytes);
}

// A resize or maximize mid-load: bake the text at the new height, so the tip, LOADING and the layout are what a load started at that size
// draws (k16.2.221). The window-state gate (main.cpp frame()) invokes it when it applies a change (k16.2.222): a storm of changes within the
// gate's cooldown re-bakes once, at the applied height, not once per event. view::heightf() is the applied height on that frame.
void LoadScreen::relayout()
{
	if (!ok_) { return; }
	const float H = view::heightf();
	if (std::fabs(H - layoutH_) < 0.5f) { return; }
	Baked b;
	if (!bakeFonts(H, b))
	{
		fprintf(stderr, "kclient: loading screen: cannot re-bake the text for %dx%d, it keeps the last size\n", view::width(), view::height());
		layoutH_ = H;	// logged once: no retry every frame
		return;
	}
	layoutH_ = H;
	printf("kclient: loading screen: relaid out for %dx%d (tip %.0f px, LOADING %.0f px)\n", view::width(), view::height(), b.body.px, b.title.px);
	queueFonts(std::move(b));
}

void LoadScreen::shutdown()
{
	if (!ok_) { return; }
	layoutH_ = 0;	// the next load re-bakes at its own window size
	ok_ = false;
	rq::push("loadscreen-free", [this] { freeGpu(); });	// the render side destroys what it made (frame lane, after any draw of the frame before)
}

void LoadScreen::freeGpu()
{
	if (gpu_.art.view.id != SG_INVALID_ID) { sg_destroy_view(gpu_.art.view); }
	if (gpu_.art.img.id != SG_INVALID_ID) { sg_destroy_image(gpu_.art.img); }
	gpu_.title.free();
	gpu_.body.free();
	if (gpu_.smp.id != SG_INVALID_ID) { sg_destroy_sampler(gpu_.smp); }
	if (gpu_.smpText.id != SG_INVALID_ID) { sg_destroy_sampler(gpu_.smpText); }
	if (gpu_.uiImg.id != SG_INVALID_ID)	// else the view is the UI layer's
	{
		sg_destroy_view(gpu_.uiView);
		sg_destroy_image(gpu_.uiImg);
	}
	gpu_ = Gpu();
}

void LoadScreen::drawArt(const Art& a, float alpha)
{
	const float W = view::widthf(), H = view::heightf(), aspect = (float)a.w / (float)a.h;
	float u0 = 0, v0 = 0;	// cover the window, cropping the axis that overflows
	if (W / H > aspect) { v0 = (1 - aspect * H / W) * 0.5f; }
	else { u0 = (1 - W / H / aspect) * 0.5f; }
	sgl_enable_texture();
	sgl_texture(a.view, gpu_.smp);
	sgl_begin_quads();
	sgl_c4f(1.0f, 1.0f, 1.0f, alpha);
	quad(0, 0, W, H, u0, v0, 1 - u0, 1 - v0);
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
}

void LoadScreen::drawPanel(const Skin& k, float x, float y, float w, float h)
{
	const float xs[4] = { x, x + k.cl, x + w - k.cr, x + w };
	const float ys[4] = { y, y + k.ct, y + h - k.cb, y + h };
	const float us[4] = { k.x, k.x + k.cl, k.x + k.w - k.cr, k.x + k.w };
	const float vs[4] = { k.y, k.y + k.ct, k.y + k.h - k.cb, k.y + k.h };
	sgl_enable_texture();
	sgl_texture(gpu_.uiView, gpu_.smp);
	sgl_begin_quads();
	sgl_c4f(1.0f, 1.0f, 1.0f, 1.0f);
	for (int j = 0; j < 3; j++)
	{
		for (int i = 0; i < 3; i++) { quad(xs[i], ys[j], xs[i + 1], ys[j + 1], us[i] / kUiW, vs[j] / kUiH, us[i + 1] / kUiW, vs[j + 1] / kUiH); }
	}
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
}

void LoadScreen::draw(float progress, const char* step)
{
	if (!gpu_.ok) { return; }	// render side: what the queued jobs made
	// A resize or maximize mid-load re-bakes the text at the window's height; the window-state gate calls relayout() when it applies one
	// (k16.2.222), so a held change draws the applied size until its apply, not one bake per event.
	drawArt(gpu_.art, 1.0f);

	const float W = view::widthf(), H = view::heightf(), s = (float)text_scale();
	const float px = W * 0.34375f, py = H * 0.657407f, pw = W * 0.3125f, ph = H * 0.277778f;
	drawPanel(kPanelSkin, px, py, pw, ph);
	drawPanel(kTextSkin, px + pw * 0.00833f, py + ph * 0.3167f, pw * 0.9833f, ph * 0.6667f);

	// progress bar, top right
	const float bw = std::min(W * 0.25f, 360.0f * s), bh = 8.0f * s, bx = W - bw - 24.0f * s, by = 24.0f * s;
	sgl_disable_texture();
	sgl_begin_quads();
	sgl_c4f(0.0f, 0.0f, 0.0f, 0.7f);
	quad(bx - 3, by - 3, bx + bw + 3, by + bh + 3, 0, 0, 0, 0);
	sgl_c4f(0.78f, 0.36f, 0.12f, 1.0f);
	quad(bx, by, bx + bw * std::clamp(progress, 0.0f, 1.0f), by + bh, 0, 0, 0, 0);
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();

	// LOADING centred in the header bar of the panel (the top 30% of it, as in the layout), cream
	const float mid = px + pw * 0.5f;
	drawText(gpu_.title, mid - gpu_.title.width("LOADING") * 0.5f, py + ph * 0.15f + gpu_.title.px * 0.32f, "LOADING", 0.871f, 0.845f, 0.810f, 1.0f);
	drawText(gpu_.body, bx + 1.0f * s, by + bh + 8.0f * s + gpu_.body.px + 1.0f * s, step, 0.0f, 0.0f, 0.0f, 1.0f);	// drop shadow
	drawText(gpu_.body, bx, by + bh + 8.0f * s + gpu_.body.px, step, 1.0f, 1.0f, 1.0f, 1.0f);

	// the tip, word-wrapped and centred in the parchment box; the wrap width is the box less ~10 px a side, as the original's EditBox
	// wraps its own rect (the vanilla capture's line spans 365 px of a 385 px box, k16.2.221)
	const float boxY = py + ph * 0.3167f, boxH = ph * 0.6667f, maxW = pw * 0.9833f - 20.0f * s;
	std::vector<std::string> lines;
	std::string line, word;
	for (size_t i = 0; i <= gpu_.tip.size(); i++)
	{
		if (i < gpu_.tip.size() && gpu_.tip[i] != ' ') { word += gpu_.tip[i]; continue; }
		if (!line.empty() && gpu_.body.width(line + " " + word) > maxW) { lines.push_back(line); line.clear(); }
		line += (line.empty() ? "" : " ") + word;
		word.clear();
	}
	lines.push_back(line);
	const float lh = gpu_.body.lineH, top = boxY + (boxH - lh * (float)lines.size()) * 0.5f + gpu_.body.px * 0.9f;
	for (size_t i = 0; i < lines.size(); i++) { drawText(gpu_.body, mid - gpu_.body.width(lines[i]) * 0.5f, top + lh * (float)i, lines[i], gpu_.tipR, gpu_.tipG, gpu_.tipB, 1.0f); }
}
