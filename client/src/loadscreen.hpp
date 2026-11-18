#pragma once
#include <string>
#include <vector>

#include "dds.hpp"
#include "sokol_gfx.h"
#include "stb_truetype.h"

// Startup loading screen in the style of the game's Kenshi_LoadingScreen.layout: a full-screen painting from data/gui/gfx/loading_screen*.dds
// (one picked at random per load and kept, as the original: kenshi-inspect findings/loading_tip.md), a LOADING panel cut from gui/gfx/Kenshi_UI.png
// (the Kenshi_FloatingPanelSkin rect) with one random tip from data/tips.txt (the same, once per load), and a progress bar in the top-right corner.
// Everything is read from the install at runtime. Text is the game's own TTFs (gui/fonts: MailartRubberstamp for LOADING, Exo2-SemiBold for the rest)
// baked with stb_truetype at the window's height, and baked again when the window is resized mid-load (relayout). Drawn between begin_overlay() and end_overlay() (worldmap.hpp).
// As the original (kenshi-inspect findings/loading_tip.md; the user's Dark UI shot 2026-11-05): the tip in Kenshi_FloaterFont_Large, LOADING in
// Kenshi_UI_Messages, sized as MyGUI does (Size * Resolution / 72 at 1080 lines) from gui/fonts/kenshi_fonts.xml, smoothed like our UI, and hard-edged (the font's
// Antialias false) only under ui.style=original; the tip's colour is Kenshi_GenericTextBoxSkin's text colour from the active skins (a theme's,
// e.g. Dark UI's grey on its dark box) when the UI layer is up, else the vanilla ink.
// Threads (docs/RENDER_THREAD.md §7.2): init, relayout and shutdown are the logic half's. They read the files and bake the glyphs on the CPU and
// queue the GPU work (renderqueue.hpp, frame lane): the images, views and samplers are made, swapped and destroyed by the render side, which
// holds everything draw() reads.
class LoadScreen
{
public:
	struct Skin { float x, y, w, h, cl, cr, ct, cb; };	// texture rect and corner sizes of a 9-slice
	// shared: the UI layer's copy of Kenshi_UI.png (ui::gfx::atlas(); not ours to destroy), used instead of loading our own.
	bool init(const std::string& dataDir, sg_view shared = {});	// false (with a message on stderr) if an asset is missing; draw() then does nothing
	// The whole screen is one load: a second init() before shutdown() keeps the picture, the tip and the fonts (the startup steps and the
	// session load that follows them draw the same screen). Returns true in that case.
	void draw(float progress, const char* step);
	// Re-bakes the text for the window's height now (k16.2.221). draw() calls it when the height changed, since the load swallows the resize
	// event; it stays one call so a window-state gate (k16.2.222) can invoke it when it applies a change.
	void relayout();
	void shutdown();

private:
	struct Art
	{
		sg_image img = {};
		sg_view view = {};
		int w = 0, h = 0;
	};
	// A TrueType font baked (stb_truetype) into an RGBA atlas at one pixel size: ASCII 32..127.
	struct Font
	{
		static constexpr int kAtlas = 1024;
		stbtt_bakedchar glyphs[96] = {};
		sg_image img = {};
		sg_view view = {};
		float px = 0;
		float lineH = 0;	// ascent - descent + line gap, pixels
		// logic side: the glyphs and the atlas pixels (rgba, kAtlas^2), no GPU call. em pixels (MyGUI's Size); px is the baked ascent-to-descent height
		bool bake(const std::string& path, float em, bool smooth, std::vector<uint8_t>& rgba);
		void make(const std::vector<uint8_t>& rgba);	// render side: the atlas image and view
		void free();	// render side
		float width(const std::string& t) const;
	};
	// A baked pair on its way to the render side (by value in the queued job).
	struct Baked
	{
		Font title, body;
		std::vector<uint8_t> titleRgba, bodyRgba;
	};
	// Logic side: bakes both fonts at window height h on the CPU (false keeps the old pair: a failed bake never leaves half the text missing).
	bool bakeFonts(float h, Baked& out);
	// Logic side: queues the swap of the baked pair in for the live one.
	void queueFonts(Baked&& b);

	// Render side: what draw() reads, made, swapped and destroyed only by the queued jobs.
	struct Gpu
	{
		bool ok = false;
		Font title, body;
		Art art;		// the one painting this load picked, kept until it ends
		sg_image uiImg = {};	// our own Kenshi_UI.png, when not shared
		sg_view uiView = {};
		sg_sampler smp = {};		// linear: the painting and the panels are scaled
		sg_sampler smpText = {};	// nearest: the glyph atlas is baked at the drawn size, so filtering only blurs it (the game's fonts are hard-edged)
		std::string tip;
		float tipR = 0, tipG = 0, tipB = 0;
	};
	Gpu gpu_;
	void freeGpu();	// render side: every resource of gpu_, and gpu_ reset
	void drawArt(const Art& a, float alpha);
	void drawPanel(const Skin& k, float x, float y, float w, float h);
	void drawText(const Font& f, float x, float y, const std::string& t, float r, float g, float b, float a);

	// Logic side
	float tipR_ = 0.0784f, tipG_ = 0.0314f, tipB_ = 0.0235f;	// Kenshi_GenericTextBoxSkin's text (vanilla's ink)
	float layoutH_ = 0;		// the window height the fonts are baked for; relayout() re-bakes at any other
	std::string titlePath_, bodyPath_;	// the two TTFs, and their em size at 1080 lines, for the re-bake
	float titleEm_ = 0, bodyEm_ = 0;
	bool titleSmooth_ = false, bodySmooth_ = false;
	std::string tip_;
	bool ok_ = false;
};
