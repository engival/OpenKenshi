#include "ui/overview.hpp"

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "sokol_app.h"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// The window at 1080 lines (the layout's 0.451 x 0.735 of 1920 x 1080), its content 14 units in; its top 58 units down (the user's
		// Dark UI shot 20261105141740_1.jpg), centred across.
		const float kW = 866, kH = 794, kPad = 14, kTop = 58;
		const float kBar = 14;	// the map's scroll bars, units
		const char* const kCaptions[(int)OverviewPage::kCount] = { "MAP", "FACTION", "RESEARCH", "CRAFTING", "SQUADS", "DIALOGUE", "AI" };
		const char* const kKinds[] = { "Town", "Outpost", "Village", "Camp", "Nest", "Workcamp", "SmallPlace", "Ruin" };
		const char* const kRelations[] = { "MarkerPlayer", "MarkerAlly", "MarkerNeutral", "MarkerEnemy" };
		// map.md / map24_static: zoom steps (pixels a texel; the levels and the max x the UI scale), marker sizes (units).
		const double kWheel = 0.075, kLevel1 = 0.40, kLevel2 = 0.75, kMaxZoom = 1.5;
		const float kDot = 32, kCamera = 20, kMoveW = 24, kMoveH = 36, kLabelGap = 2;	// the camera: the user's shot, a 20 x 13 px arrow at 1440
		const double kMoveSeconds = 3;
		const cfg::Bool kStepped{ cfg::Group{ "HUD and labels" }, "ui.map_icons_stepped", false, "World map: town icons jump in size at the zoom levels, as the original (off: they grow smoothly with the zoom)" };
	}

	// The map's place: the host's image for the view kept here, the host's markers over it (Kenshi_MapMarkers). LMB / MMB drag pans 1:1, the
	// wheel zooms about the pointer, a click does nothing; RMB released on the map is a move order (Shift: queued) and puts the move marker
	// there for 3 s. Markers take no input; a visited town's tip is the map's tooltip under the pointer.
	class Overview::MapView : public Widget
	{
	public:
		const MapActions* map = nullptr;
		const MapState* state = nullptr;
		double cx = 0, cz = 0, zoom = 0;	// zoom: pixels a texel; 0 = fit
		ScrollBar* bars[2] = {};	// vertical (right), horizontal (bottom): placed here, outside the view, while the image is larger than it
		static constexpr const char* kLabelFont = "Kenshi_StandardFont_Medium";	// the original's Kenshi_TextboxStandardText

		MapView() { solid = true; }

		double fit() const { return screen_.w > 0 && state ? std::min(screen_.w, screen_.h) / state->texels : 0; }
		double ppu() const { return state ? state->texels * zoom / state->extent : 0; }
		int level() const { return zoom < kLevel1 * scale_ ? 0 : zoom < kLevel2 * scale_ ? 1 : 2; }

		void clampView()
		{
			if (!state || screen_.w <= 0) { return; }
			zoom = std::clamp(zoom, fit(), std::max(fit(), kMaxZoom * scale_));
			cx = keepIn(cx, screen_.w);
			cz = keepIn(cz, screen_.h);
		}

		// A view centre on one axis (the view's side, pixels) as the original's ScrollView: the image never scrolls past its edges, and an
		// image narrower than the view sits centred.
		double keepIn(double c, float side) const
		{
			const double half = state->extent / 2, span = ppu() > 0 ? side / ppu() : state->extent;
			return span >= state->extent ? 0 : std::clamp(c, -half + span / 2, half - span / 2);
		}

		void toScreen(double x, double z, float& sx, float& sy) const
		{
			sx = (float)(screen_.x + screen_.w / 2 + (x - cx) * ppu());
			sy = (float)(screen_.y + screen_.h / 2 + (z - cz) * ppu());
		}

		void toWorld(float sx, float sy, double& x, double& z) const
		{
			x = cx + (sx - screen_.x - screen_.w / 2) / ppu();
			z = cz + (sy - screen_.y - screen_.h / 2) / ppu();
		}

		// Zoomed by d about the screen point (px, py): the world point under it stays.
		void zoomAbout(double d, float px, float py)
		{
			if (ppu() <= 0) { return; }
			double x, z;
			toWorld(px, py, x, z);
			zoom += d;
			clampView();
			cx = x - (px - screen_.x - screen_.w / 2) / ppu();
			cz = z - (py - screen_.y - screen_.h / 2) / ppu();
			clampView();
		}

		void step(int d) { zoomAbout(d * fit(), screen_.x + screen_.w / 2, screen_.y + screen_.h / 2); }	// + / -: a fit-zoom about the centre

		void centre()
		{
			if (!state) { return; }
			cx = state->cameraX;
			cz = state->cameraZ;
			clampView();
		}

		MapFrame frame() const { return { screen_.x, screen_.y, screen_.w, screen_.h, cx, cz, ppu() }; }

		bool shows(const MapPlace& p) const { return p.tier == MapTier::kMin || (p.tier == MapTier::kMid && level() >= 1) || level() >= 2; }

		// The original's steps (ui.map_icons_stepped): level 0 the frame x 0.5; level 1 a town x 0.75, the rest x 0.85; level 2 a town x 1,
		// the rest x 0.85. Smooth (the default, k16.2.206.1): piecewise-linear in zoom through those sizes, each reached at its level's
		// middle (level 0's taken as kLevel1 less half of level 1, level 2's as halfway to kMaxZoom), clamped outside them. Pixels.
		Rect icon(const MapPlace& p) const
		{
			const Rect t = frameOf(kKinds[(int)p.kind]);
			if (t.w <= 0 || !shows(p)) { return {}; }
			const bool town = p.kind == MapPlaceKind::kTown;
			const float sizes[3] = { 0.5f, town ? 0.75f : 0.85f, town ? 1.0f : 0.85f };
			float f = sizes[level()];
			if (!kStepped.get())
			{
				const double half = (kLevel2 - kLevel1) / 2, mids[3] = { kLevel1 - half, kLevel1 + half, (kLevel2 + kMaxZoom) / 2 };
				const double z = scale_ > 0 ? zoom / scale_ : 0;
				const int i = z < mids[1] ? 0 : 1;
				const double a = std::clamp((z - mids[i]) / (mids[i + 1] - mids[i]), 0.0, 1.0);
				f = (float)(sizes[i] + (sizes[i + 1] - sizes[i]) * a);
			}
			float sx, sy;
			toScreen(p.x, p.z, sx, sy);
			const float w = t.w * f * scale_, h = t.h * f * scale_;
			return { std::round(sx - w / 2), std::round(sy - h / 2), std::round(w), std::round(h) };
		}

		bool labelled(const MapPlace& p) const
		{
			return level() >= 1 && shows(p) && !p.name.empty() && p.kind != MapPlaceKind::kCamp && p.kind != MapPlaceKind::kNest;
		}

		bool moveShown() const { return now() < moveUntil_; }

		// The view, less the bars' strips while the image overflows it (decided on the whole rect); the bars follow cx / cz.
		void place(const Rect& px, float s) override
		{
			Widget::place(px, s);
			clampView();	// the fit follows the size; zoom 0 starts there
			if (!bars[0] || !bars[1]) { return; }
			const double span[2] = { ppu() > 0 ? px.h / ppu() : 0, ppu() > 0 ? px.w / ppu() : 0 };
			const bool on[2] = { state && span[0] < state->extent * 0.999, state && span[1] < state->extent * 0.999 };
			const float b = std::round(kBar * s);
			const Rect view = { px.x, px.y, px.w - (on[0] ? b : 0), px.h - (on[1] ? b : 0) };
			if (on[0] || on[1])
			{
				Widget::place(view, s);
				clampView();
			}
			for (int i = 0; i < 2; i++)
			{
				ScrollBar& bar = *bars[i];
				bar.visible = on[i];
				if (!on[i]) { continue; }
				const double half = state->extent / 2;
				bar.page = i == 0 ? view.h / ppu() : view.w / ppu();
				bar.min = -half + bar.page / 2;
				bar.max = half - bar.page / 2;
				bar.step = bar.page * 0.1;
				bar.value = i == 0 ? cz : cx;
				bar.place(i == 0 ? Rect{ view.right(), px.y, b, view.h } : Rect{ px.x, view.bottom(), view.w, b }, s);
			}
		}

		void scrollTo(int bar, double v)
		{
			(bar == 0 ? cz : cx) = v;
			clampView();
		}

		bool onPress(Btn b, float x, float y) override
		{
			btn_ = b;
			lastX_ = x;
			lastY_ = y;
			return true;
		}

		void onDrag(float x, float y) override
		{
			if (btn_ == Btn::kRight || ppu() <= 0) { return; }
			cx -= (x - lastX_) / ppu();
			cz -= (y - lastY_) / ppu();
			lastX_ = x;
			lastY_ = y;
			clampView();
		}

		void onRelease(Btn b, float x, float y) override
		{
			if (b != Btn::kRight || !screen_.contains(x, y) || ppu() <= 0) { return; }
			toWorld(x, y, moveX_, moveZ_);
			moveUntil_ = now() + kMoveSeconds;
			if (map && map->moveTo) { map->moveTo(moveX_, moveZ_, (mods() & SAPP_MODIFIER_SHIFT) != 0); }
		}

		bool onScroll(float dy) override
		{
			float mx, my;
			mouse(mx, my);
			zoomAbout(dy * kWheel, mx, my);	// the original's delta x 0.000625: a fine-stepped wheel or a touchpad zooms by its fraction of a notch
			return true;
		}

		std::string tip() const override	// the topmost place under the pointer with a tip
		{
			if (!state) { return {}; }
			float mx, my;
			mouse(mx, my);
			const MapPlace* best = nullptr;	// Min draws last: on top
			int bestTier = 99;
			for (const MapPlace& p : state->places)
			{
				if (p.tip.empty() || (int)p.tier > bestTier || !icon(p).contains(mx, my)) { continue; }
				best = &p;
				bestTier = (int)p.tier;
			}
			return best ? best->tip : std::string();
		}

	protected:
		void paint() const override
		{
			style().draw(Look::kInset, screen_, scale_, State::kNormal, false);
			if (map && map->draw && ppu() > 0)
			{
				const MapFrame f = frame();
				gfx::host(screen_, [draw = map->draw, f](float, float, float, float) { draw(f); });	// copied into the list: runs at the replay
			}
			if (!state) { return; }
			gfx::pushClip(screen_);
			for (int t = (int)MapTier::kMax; t >= (int)MapTier::kMin; t--)	// Max < Mid < Min
			{
				for (const MapPlace& p : state->places) { if ((int)p.tier == t) { drawPlace(p); } }
			}
			float sx, sy;
			if (level() >= 1)
			{
				const Rect dot = frameOf("Character");
				const float d = kDot * scale_;
				for (const auto& [x, z] : state->characters)
				{
					toScreen(x, z, sx, sy);
					gfx::sprite({ std::round(sx - d / 2), std::round(sy - d / 2), d, d }, dot, style().colour("MarkerPlayer", {}));
				}
			}
			toScreen(state->cameraX, state->cameraZ, sx, sy);
			gfx::spriteRotated(sx, sy, kCamera * scale_, kCamera * scale_, state->cameraYaw, frameOf("Camera"), style().colour("MarkerPlayer", {}));	// a small blue arrow
			if (moveShown())	// anchored at its bottom centre
			{
				toScreen(moveX_, moveZ_, sx, sy);
				const float w = kMoveW * scale_, h = kMoveH * scale_;
				gfx::sprite({ std::round(sx - w / 2), std::round(sy - h), w, h }, frameOf("Movement"), style().colour("MarkerMovement", {}));
			}
			gfx::popClip();
		}

	private:
		Btn btn_ = Btn::kLeft;
		float lastX_ = 0, lastY_ = 0;
		double moveX_ = 0, moveZ_ = 0, moveUntil_ = -1;

		static Rect frameOf(const char* name)
		{
			const ImageSet* is = res::images("Kenshi_MapMarkers");
			if (!is) { return {}; }
			auto it = is->frames.find(name);
			return it != is->frames.end() ? it->second : Rect{};
		}

		// The icon tinted by relation, the NewDiscovered frame over it (green), the name centred under it (a dark outline).
		void drawPlace(const MapPlace& p) const
		{
			const Rect r = icon(p);
			if (r.w <= 0) { return; }
			const Color c = style().colour(kRelations[(int)p.relation], {});
			gfx::sprite(r, frameOf(kKinds[(int)p.kind]), c);
			if (p.isNew) { gfx::sprite(r, frameOf("NewDiscovered"), res::colour("MapNew", { 0, 1, 0, 1 })); }	// stretched over the icon (map24_static)
			if (!labelled(p)) { return; }
			const Font* f = fonts::get(kLabelFont);
			if (!f) { return; }
			const float x = std::round(r.x + r.w / 2 - f->width(p.name) / 2), y = std::round(r.bottom() + kLabelGap * scale_);
			const Color dark = { 0, 0, 0, 0.85f };
			for (const auto& [dx, dy] : { std::pair{ -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }) { f->draw(x + dx, y + dy, p.name, dark); }
			f->draw(x, y, p.name, c);
		}
	};

	Overview::Overview(OverviewActions actions, MapActions map) : act_(std::move(actions)), map_(std::move(map))
	{
		using A = Widget::Align;
		win_ = &root().add<CaptionWindow>("", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, kPad);
		win_->halign = A::kCenter;
		win_->rect = { 0, kTop, kW, kH };
		win_->visible = false;
		Panel& body = win_->body();
		body.rect.h = kH - CaptionWindow::kHeaderH;	// the window's width sets the body's
		tabs_ = &body.add<TabView>(140, 35);
		tabs_->halign = tabs_->valign = A::kStretch;
		tabs_->gap = 0;	// the layout's six 140-unit tabs fill the header; with AI they shrink
		tabs_->onSelect = [this](size_t i)
		{
			win_->caption().text = captions_[i].empty() ? kCaptions[i] : captions_[i];
			if ((OverviewPage)i == OverviewPage::kMap) { centreMap(); }
			if (act_.page) { act_.page((OverviewPage)i); }
		};
		for (const char* c : kCaptions) { tabs_->add(c); }
		static_cast<Button*>(tabs_->tab((size_t)OverviewPage::kMap))->sound = "Map_Open";	// the original's tab handler 14049bf90: case 0 Map_Open,
		static_cast<Button*>(tabs_->tab((size_t)OverviewPage::kResearch))->sound = "Tech_Open";	// case 2 Tech_Open, the others Tab
		win_->caption().text = kCaptions[0];

		// MAP: the map over the page's top 94 %, - + O at the bottom right (Dark UI's Kenshi_OverviewWindow MapTab; vanilla has O at the left).
		// The scroll bars come first: the map view places them after the page's layout.
		Widget& mp = pageWidget(OverviewPage::kMap);
		ScrollBar* bars[2] = { &mp.add<ScrollBar>(true), &mp.add<ScrollBar>(false) };
		mapView_ = &mp.add<MapView>();
		for (int i = 0; i < 2; i++)
		{
			bars[i]->visible = false;
			bars[i]->onChange = [this, i](double v) { mapView_->scrollTo(i, v); };
			mapView_->bars[i] = bars[i];
		}
		mapView_->map = &map_;
		mapView_->state = &mapState_;
		mapView_->halign = mapView_->valign = A::kStretch;
		mapView_->rect = { 0, 0, 0, 40 };
		const char* labels[3] = { "O", "-", "+" };
		const char* tips[3] = { "Centre the map", "Zoom out", "Zoom in" };
		for (int i = 0; i < 3; i++)
		{
			Button& b = mp.add<Button>(labels[i], Look::kButton, "Kenshi_PaintedTextFont_Medium");
			b.valign = A::kEnd;
			b.halign = A::kEnd;
			b.rect = { i == 0 ? 0.0f : i == 1 ? 82.0f : 41.0f, 7, 25, 26 };	// 25 x 26, 41 apart, O's right edge on the map's (the shot)
			b.tooltip = tips[i];
			b.onClicked = [this, i]
			{
				if (i == 0) { centreMap(); }
				else { mapView_->step(i == 1 ? -1 : 1); }
			};
			mapButtons_[i] = &b;
		}
		// The pages still waiting for their systems.
		for (OverviewPage p : { OverviewPage::kFaction, OverviewPage::kResearch, OverviewPage::kCrafting, OverviewPage::kDialogue, OverviewPage::kAi })
		{
			Label& l = pageWidget(p).add<Label>("Not available yet", "Kenshi_StandardFont_Medium");
			l.halign = l.valign = A::kCenter;
			l.colour = res::colour("Main").alpha(0.6f);
		}
	}

	Overview::~Overview()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	bool Overview::shown() const { return win_->visible; }
	bool Overview::shown(OverviewPage p) const { return win_->visible && page() == p; }
	OverviewPage Overview::page() const { return (OverviewPage)tabs_->selected(); }
	Widget& Overview::pageWidget(OverviewPage p) const { return *tabs_->page((size_t)p); }
	const std::string& Overview::caption() const { return win_->caption().text; }

	void Overview::setCaption(OverviewPage p, const std::string& text)
	{
		captions_[(int)p] = text;
		if (page() == p) { win_->caption().text = text.empty() ? kCaptions[(int)p] : text; }
	}

	void Overview::show(bool on)
	{
		if (on && !win_->visible && page() == OverviewPage::kMap) { centreMap(); }
		if (on != win_->visible)
		{
			const bool tech = page() == OverviewPage::kResearch;
			if (on) { sound(page() == OverviewPage::kMap ? "Map_Open" : tech ? "Tech_Open" : "Tab"); }
			else { sound(tech ? "Tech_Close" : "Map_Close"); }
		}
		win_->visible = on;
		if (on)
		{
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	void Overview::open(OverviewPage p)
	{
		if (page() != p)
		{
			if (win_->visible) { sound(tabs_->tab((size_t)p)->sound); }	// a key that switches the open window's page sounds as the tab's click
			tabs_->select((size_t)p);
		}
		else if (!win_->visible && act_.page) { act_.page(p); }
		show(true);
	}

	void Overview::toggle(OverviewPage p)
	{
		if (shown(p))
		{
			show(false);
			if (act_.close) { act_.close(); }
		}
		else { open(p); }
	}

	Widget* Overview::window() const { return win_; }
	Widget* Overview::tab(OverviewPage p) const { return tabs_->tab((size_t)p); }
	Widget* Overview::mapArea() const { return mapView_; }
	Widget* Overview::mapButton(int i) const { return i >= 0 && i < 3 ? mapButtons_[i] : nullptr; }
	MapFrame Overview::mapFrame() const { return mapView_->frame(); }
	double Overview::mapZoom() const { return mapView_->zoom; }
	int Overview::mapLevel() const { return mapView_->level(); }
	Rect Overview::mapMarker(size_t i) const { return i < mapState_.places.size() ? mapView_->icon(mapState_.places[i]) : Rect{}; }
	bool Overview::mapLabel(size_t i) const { return i < mapState_.places.size() && mapView_->labelled(mapState_.places[i]); }
	bool Overview::mapMoveShown() const { return mapView_->moveShown(); }
	ScrollBar* Overview::mapBar(int i) const { return i >= 0 && i < 2 ? mapView_->bars[i] : nullptr; }
	Rect Overview::mapBadge(size_t i) const { return i < mapState_.places.size() && mapState_.places[i].isNew ? mapView_->icon(mapState_.places[i]) : Rect{}; }
	const char* Overview::mapLabelFont() { return MapView::kLabelFont; }
	float Overview::mapCameraUnits() { return kCamera; }

	void Overview::updateMap(const MapState& s)
	{
		if (!(s == mapState_)) { mapState_ = s; }
	}

	void Overview::centreMap()
	{
		mapView_->centre();
	}
}
