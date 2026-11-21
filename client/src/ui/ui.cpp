#include "ui/ui.hpp"

#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>

#include "app/config.hpp"
#include "ui/icons.hpp"
#include "ui/keys.hpp"

namespace ui
{
	namespace
	{
		const cfg::Int kScale(cfg::Group{ "Look" }, "ui.scale", 100, 50, 300, "UI size, percent of the game's 1080-line layout");
		const cfg::Text kHudTips{ cfg::Group{ "Look" }, "ui.hud_tips", "box", { "box", "float" }, "Where the HUD's and the inventory stats' hover text shows: box, the original's fixed box beside the medical panel; float, at the pointer" };
		const cfg::Bool kSounds{ cfg::Group{ "Sounds" }, "ui.sounds", true, "The original's interface sounds: clicks, hover, tabs, sliders, windows opening and closing" };
		constexpr double kDoubleClick = 0.35, kTooltipDelay = 0.5;
		const cfg::Bool kControlHints{ cfg::Group{ "HUD and labels" }, "ui.control_hints", false, "Control hints: a small panel on the right lists what the mouse does on the thing under the pointer (click, Shift+Click, double click, ...); F6 toggles it" };
		constexpr size_t kHintLines = 8;	// at most this many input lines; the host keeps it to about five
		std::function<void(const SoundPost&)> soundSink_;

		const cfg::Text kHoverSounds{ cfg::Group{ "Sounds" }, "ui.hover_sounds", "original", { "original", "all", "none" }, "Which buttons click on mouse-over: original, the title menu's only (as the original); all, every button; none (playtest switch)" };

		// The button that posts Hover with the pointer on w, else null: the nearest Button at or above w, enabled and doing something (a silent
		// click still hovers), and with ui.hover_sounds=original a hoverSound widget at or above it. The original attaches its Hover callback
		// 140912e40 only to the title screen's buttons (kinspect findings/rwh96_hover_widgets_static.md, kenshi-k16.3.20).
		const Button* hoverButton(const Widget* w)
		{
			const std::string mode = kHoverSounds.get();
			if (mode == "none") { return nullptr; }
			const Button* b = nullptr;
			for (; w && !b; w = w->parent()) { b = dynamic_cast<const Button*>(w); }
			if (!b || !b->enabled || !(b->onClicked || b->onToggle)) { return nullptr; }
			if (mode == "all") { return b; }
			for (const Widget* p = b; p; p = p->parent()) { if (p->hoverSound) { return b; } }
			return nullptr;
		}

		struct Ctx
		{
			bool ready = false;
			std::unique_ptr<Widget> root, popups, overlay;
			std::function<void(float, float)> dragImage;
			std::function<bool(const sapp_event&)> interceptor;
			Toasts* toasts = nullptr;
			Banner* banner = nullptr;
			HudMessage* message = nullptr;
			const Widget* hoverSounded = nullptr;	// the button that last posted Hover: once per pointer entry, its children included
			Widget *hover = nullptr, *press = nullptr, *focus = nullptr, *lastClick = nullptr, *tipFor = nullptr;
			std::vector<Menu*> held;	// open held menus (openMenu)
			std::vector<const Widget*> claimed;	// claimTips areas
			HintBlock hints;	// the host's (setHints)
			std::function<Rect(float, float)> hintsDock;	// dockHints
			Btn pressBtn = Btn::kLeft, lastBtn = Btn::kLeft;
			float mx = -1, my = -1, scale = 1;
			int vw = 0, vh = 0;	// the framebuffer in pixels, from the host (setViewport)
			bool warnedViewport = false;
			double time = 0, lastClickT = 0, hoverSince = 0;
			uint32_t mods = 0;
			const Font *tipFont = nullptr, *richTipFont = nullptr, *hintFont = nullptr;
		};
		Ctx c_;

		Widget* hitAll(float x, float y)
		{
			if (Widget* w = c_.popups->hit(x, y)) { return w; }
			return c_.root->hit(x, y);
		}

		void updateHover()
		{
			Widget* h = c_.ready ? hitAll(c_.mx, c_.my) : nullptr;
			if (h != c_.hover)
			{
				c_.hover = h;
				c_.hoverSince = c_.time;
				const Button* b = hoverButton(h);
				if (b != c_.hoverSounded)
				{
					c_.hoverSounded = b;
					if (b) { sound("Hover"); }
				}
			}
		}

		bool button(const sapp_event& e, Btn& b)
		{
			switch (e.mouse_button)
			{
				case SAPP_MOUSEBUTTON_LEFT: b = Btn::kLeft; return true;
				case SAPP_MOUSEBUTTON_RIGHT: b = Btn::kRight; return true;
				case SAPP_MOUSEBUTTON_MIDDLE: b = Btn::kMiddle; return true;
				default: return false;
			}
		}

		// A focused edit box that is disabled (read-only) but shown: it edits nothing, yet takes the keys an enabled one would (all but
		// F1..F25), so a letter typed at it never reaches a game hotkey (kenshi-k16.2.472.2).
		bool lockedEdit()
		{
			return c_.focus && c_.focus->shown() && !c_.focus->enabled && dynamic_cast<const EditBox*>(c_.focus);
		}

		// The old focus hears onBlur (an edit box commits). Blur handlers must not destroy widgets.
		void setFocus(Widget* w)
		{
			Widget* old = c_.focus;
			c_.focus = w;
			if (old && old != w) { old->onBlur(); }
		}

		// A left press raises every window it lands in (a CaptionWindow with its group) and the root's child that holds them, before the
		// press is delivered: one click raises and acts. Nothing is raised over a modal backdrop: a press outside the modal hits the backdrop,
		// which is no window. Popups are a layer of their own over the root and are not touched.
		void raiseOnPress(Widget* w)
		{
			bool any = false;
			Widget* top = nullptr;	// the root's child holding w
			for (Widget* p = w; p && p != c_.root.get(); p = p->parent())
			{
				if (auto* cw = dynamic_cast<CaptionWindow*>(p)) { cw->raiseGroup(); any = true; }
				else if (dynamic_cast<Window*>(p)) { p->raise(); any = true; }
				if (p->parent() == c_.root.get()) { top = p; }
			}
			if (any && top && !dynamic_cast<Window*>(top)) { top->raise(); }	// a layer of windows (the gear screen, the item windows)
		}

		bool mouseDown(Btn b, float x, float y)
		{
			if (!c_.popups->children().empty())
			{
				if (!c_.popups->hit(x, y)) { closePopups(); return true; }	// the press that dismisses a popup does nothing else
			}
			Widget* w = hitAll(x, y);
			Widget* f = w;	// the keyboard goes to the nearest focusable widget at or above the press (a window for a button in it), else nowhere
			while (f && !f->focusable()) { f = f->parent(); }
			setFocus(f);
			if (!w) { return false; }
			if (b == Btn::kLeft) { raiseOnPress(w); }
			if (!w->enabled) { return true; }
			if (w->onPress(b, x, y))
			{
				c_.press = w;
				c_.pressBtn = b;
			}
			return true;
		}

		bool mouseUp(Btn b, float x, float y)
		{
			if (!c_.held.empty())	// any button (ctx18_static: a release over a row picks it, elsewhere closes)
			{
				Menu* over = nullptr;
				bool keep = false;	// a held menu that stays open when the release picks nothing (ItemMenu's)
				for (Menu* m : c_.held)
				{
					if (m->screen().contains(x, y)) { over = m; }
					keep = keep || m->keepOnMiss;
				}
				c_.held.clear();
				if (c_.press && b == c_.pressBtn) { c_.press = nullptr; }	// the press that opened it (a widget's hold) clicks nothing
				const int i = over ? over->pickAt(x, y) : -1;
				if (keep && !(over && over->pickable(i))) { return true; }
				if (over) { over->pick(i); }
				else { closePopups(); }
				return true;
			}
			if (!c_.press || b != c_.pressBtn) { return false; }
			Widget* w = c_.press;
			w->onRelease(b, x, y);
			if (!c_.press) { return true; }	// onRelease destroyed it (forget() cleared the press)
			c_.press = nullptr;
			if (hitAll(x, y) != w || !w->enabled) { return true; }	// released elsewhere: no click
			if (c_.lastClick == w && c_.lastBtn == b && c_.time - c_.lastClickT < kDoubleClick)
			{
				c_.lastClick = nullptr;
				w->onDoubleClick(b);
			}
			else
			{
				c_.lastClick = w;
				c_.lastBtn = b;
				c_.lastClickT = c_.time;
				w->onClick(b);
			}
			return true;
		}

		bool under(const Widget* w, const Widget* area)
		{
			for (; w; w = w->parent()) { if (w == area) { return true; } }
			return false;
		}

		// Plain text wraps in Main. A rich tip (any "{" or tab) is layoutRich's markup (widget.hpp) in the game's tooltip font, one width.
		void drawTooltip()
		{
			Widget* w = c_.hover;
			for (const Widget* a : c_.claimed) { if (under(w, a)) { return; } }	// its screen shows it in a fixed box
			const std::string tip = w ? w->tip() : std::string();
			if (!w || c_.press || tip.empty() || c_.time - c_.hoverSince < kTooltipDelay || !c_.tipFont) { return; }
			const bool rich = tip.find_first_of("{\t") != std::string::npos && c_.richTipFont;
			const Font& f = rich ? *c_.richTipFont : *c_.tipFont;
			const float s = c_.scale, rowH = rich ? std::max(f.lineHeight(), std::round(20 * s)) : f.lineHeight();	// the game's rows: 27 px at 1440
			const float pad = std::round((rich ? 13 : 6) * s), wrapW = (rich ? 300 : 320) * s, gap = 24 * s;
			const Color ink = res::colour(rich ? "TipText" : "Main");
			const RichLayout t = layoutRich(tip, f, wrapW, rowH, gap, ink);
			const float tw = rich ? std::max(t.w, wrapW) : t.w;	// the game's tooltips are one width; the values line up on its right
			const float bh = t.h + 2 * pad, bw = tw + 2 * pad;
			const Rect screen = c_.root->screen();
			float x = c_.mx + 16 * s, y = c_.my + 22 * s;
			if (x + bw > screen.right()) { x = screen.right() - bw; }
			if (y + bh > screen.bottom()) { y = c_.my - bh - 4 * s; }
			const Rect box = { std::round(std::max(0.0f, x)), std::round(std::max(0.0f, y)), std::round(bw), std::round(bh) };
			style().draw(Look::kTooltip, box, s, State::kNormal, false);
			drawRich(t, f, box.x + pad, box.y + pad, tw);
		}

		// The block the hints panel shows: the hovered widget's own (the nearest at or above it that lists any), else the host's; empty when off.
		HintBlock currentHints()
		{
			if (!kControlHints.get()) { return {}; }
			for (const Widget* w = c_.hover; w; w = w->parent())
			{
				HintBlock b;
				w->hints(b.lines);
				if (b.lines.empty()) { continue; }
				const Button* btn = dynamic_cast<const Button*>(w);
				b.title = btn && !btn->text.empty() ? btn->text : w->tip().substr(0, w->tip().find('\n'));
				return b;
			}
			return c_.hints.lines.empty() ? HintBlock() : c_.hints;
		}

		// The panel's measures (kenshi-k16.2.294.10.1: a notch under the first cut, Kenshi_StandardFont_MediumSmall, padding 8, column gap 14).
		struct HintsLayout
		{
			Rect box;
			float pad = 0, gap = 0, rowH = 0, w = 0, inW = 0;
			size_t n = 0;
			std::vector<std::string> labels;
		};

		// Our own drawn look in both styles: no frame, a dark see-through box docked by the HUD (dockHints: above its bottom-right cluster),
		// else against the right edge, vertically centred; the title, then one row per input, the inputs in a column. It grows upward. A
		// greyed row (not doable now) says why after its label.
		HintsLayout hintsLayout(const HintBlock& b)
		{
			HintsLayout L;
			const Font& f = *c_.hintFont;
			const float s = c_.scale, maxW = 400 * s;
			L.pad = std::round(8 * s);
			L.gap = std::round(14 * s);
			L.rowH = std::round(f.lineHeight() + 2 * s);
			L.n = std::min(b.lines.size(), kHintLines);
			for (size_t i = 0; i < L.n; i++)
			{
				const HintLine& l = b.lines[i];
				L.inW = std::max(L.inW, f.width(l.input));
				L.labels.push_back(!l.enabled && !l.why.empty() ? l.label + "  (" + l.why + ")" : l.label);
			}
			L.w = f.width(b.title);
			for (const std::string& l : L.labels) { L.w = std::max(L.w, L.inW + L.gap + f.width(l)); }
			L.w = std::min(L.w, maxW);
			const float bh = std::round((b.title.empty() ? 0 : L.rowH) + L.n * L.rowH + 2 * L.pad), bw = std::round(L.w + 2 * L.pad);
			const Rect docked = c_.hintsDock ? c_.hintsDock(bw, bh) : Rect();
			if (docked.w > 0) { L.box = docked; return L; }
			const Rect screen = c_.root->screen();
			L.box = { std::round(screen.right() - bw), std::round(screen.y + (screen.h - bh) / 2), bw, bh };
			return L;
		}

		void drawHints()
		{
			const HintBlock b = currentHints();
			if (b.lines.empty() || !c_.hintFont) { return; }
			const Font& f = *c_.hintFont;
			const HintsLayout L = hintsLayout(b);
			gfx::fill(L.box, { 0.03f, 0.03f, 0.03f, 0.62f });
			const auto clip = [&](const std::string& t, float room) { return t.substr(0, f.fit(t, room)); };
			float y = L.box.y + L.pad;
			if (!b.title.empty())
			{
				f.draw(L.box.x + L.pad, y, clip(b.title, L.w), res::colour("Title"));
				y += L.rowH;
			}
			const Color main = res::colour("Main"), input = res::colour("Special");
			for (size_t i = 0; i < L.n; i++, y += L.rowH)
			{
				const bool on = b.lines[i].enabled;
				f.draw(L.box.x + L.pad, y, b.lines[i].input, on ? input : main.alpha(0.4f));
				f.draw(L.box.x + L.pad + L.inW + L.gap, y, clip(L.labels[i], L.w - L.inW - L.gap), on ? main : main.alpha(0.4f));
			}
		}
	}

	namespace detail
	{
		void forget(const Widget* w)
		{
			if (c_.hover == w) { c_.hover = nullptr; }
			if (c_.hoverSounded == w) { c_.hoverSounded = nullptr; }
			if (c_.press == w) { c_.press = nullptr; }
			if (c_.focus == w) { c_.focus = nullptr; }
			if (c_.lastClick == w) { c_.lastClick = nullptr; }
			if (c_.tipFor == w) { c_.tipFor = nullptr; }
			std::erase(c_.claimed, w);
			std::erase(c_.held, w);
		}
	}

	bool init(const std::string& dataDir, const std::vector<std::string>& themes)
	{
		if (c_.ready) { return true; }
		res::setRoots(dataDir, themes);
		for (const std::string& t : themes) { fprintf(stderr, "kclient ui: theme %s/gui\n", t.c_str()); }
		if (!gfx::init()) { return false; }
		if (!res::load() || !fonts::load())
		{
			fprintf(stderr, "kclient ui: skins or fonts unusable under %s/gui; no UI\n", dataDir.c_str());
			fonts::shutdown();
			gfx::shutdown();
			return false;
		}
		styles::init();
		c_.ready = true;	// widgets look up styles and fonts as they are made
		c_.root = std::make_unique<Widget>();
		c_.popups = std::make_unique<Widget>();
		c_.overlay = std::make_unique<Widget>();
		Toasts& t = c_.overlay->add<Toasts>();
		t.halign = Widget::Align::kCenter;
		t.rect = { 0, 110, 900, 300 };
		c_.toasts = &t;
		Banner& b = c_.overlay->add<Banner>();
		b.rect = { 0, 259, 0, 129 };	// BiomeTextPanel: the full width, 0.24 - 0.36 of the height
		b.halign = Widget::Align::kStretch;
		c_.banner = &b;
		HudMessage& m = c_.overlay->add<HudMessage>();
		m.rect = { 0, 240, 690, 70 };	// MessagePanel: 0.32 0.713 0.359 0.065 of 1920 x 1080
		m.halign = Widget::Align::kCenter;
		m.valign = Widget::Align::kEnd;
		c_.message = &m;
		c_.tipFont = fonts::get("Kenshi_StandardFont_Medium");
		c_.hintFont = fonts::get("Kenshi_StandardFont_MediumSmall");
		c_.richTipFont = fonts::get("Kenshi_StandardFont_Large");	// item tooltips: measured against the in-game capture
		return true;
	}

	void shutdown()
	{
		if (!c_.ready) { return; }
		c_.root.reset();
		c_.popups.reset();
		c_.overlay.reset();
		icons::shutdown();
		styles::shutdown();
		fonts::shutdown();
		gfx::shutdown();
		c_ = Ctx();
	}

	bool ready() { return c_.ready; }
	Widget& root() { return *c_.root; }

	bool event(const sapp_event& e)
	{
		keys::track(e);
		if (!c_.ready) { return false; }
		c_.mods = e.modifiers;
		if (c_.interceptor)
		{
			auto f = c_.interceptor;	// it may clear itself
			if (f(e)) { return true; }
		}
		Btn b;
		switch (e.type)
		{
			case SAPP_EVENTTYPE_MOUSE_MOVE:
				c_.mx = e.mouse_x;
				c_.my = e.mouse_y;
				if (c_.press) { c_.press->onDrag(c_.mx, c_.my); }
				updateHover();
				return false;
			case SAPP_EVENTTYPE_MOUSE_DOWN:
				c_.mx = e.mouse_x;
				c_.my = e.mouse_y;
				return button(e, b) && mouseDown(b, c_.mx, c_.my);
			case SAPP_EVENTTYPE_MOUSE_UP:
				c_.mx = e.mouse_x;
				c_.my = e.mouse_y;
				return button(e, b) && mouseUp(b, c_.mx, c_.my);
			case SAPP_EVENTTYPE_MOUSE_SCROLL:
				c_.mx = e.mouse_x;
				c_.my = e.mouse_y;
				updateHover();
				if (!c_.hover) { return false; }
				for (Widget* w = c_.hover; w; w = w->parent()) { if (w->enabled && w->onScroll(e.scroll_y)) { break; } }
				return true;
			case SAPP_EVENTTYPE_MOUSE_LEAVE:
				c_.mx = c_.my = -1;
				updateHover();
				return false;
			case SAPP_EVENTTYPE_KEY_DOWN:
			case SAPP_EVENTTYPE_KEY_UP:
			{
				const bool down = e.type == SAPP_EVENTTYPE_KEY_DOWN;
				if (e.key_code == SAPP_KEYCODE_ESCAPE && std::any_of(c_.popups->children().begin(), c_.popups->children().end(), [](const auto& p)
				{
					const Menu* m = dynamic_cast<const Menu*>(p.get());
					return !m || m->escapeCloses;
				}))
				{
					if (down) { closePopups(); }
					return true;
				}
				if (c_.focus && c_.focus->shown() && c_.focus->enabled && c_.focus->onKey(e.key_code, down)) { return true; }
				if (lockedEdit() && !(e.key_code >= SAPP_KEYCODE_F1 && e.key_code <= SAPP_KEYCODE_F25)) { return true; }	// a disabled edit box keeps typing from the world
				if (keys::pressed(e, "toggle_hints"))	// the UI's own setting: no host wiring (a focused key box takes the key first)
				{
					kControlHints.set(!kControlHints.get());
					return true;
				}
				return false;
			}
			case SAPP_EVENTTYPE_CHAR:
				return (c_.focus && c_.focus->shown() && c_.focus->enabled && c_.focus->onChar(e.char_code)) || lockedEdit();
			default:
				return false;
		}
	}

	void frame(double dt)
	{
		if (!c_.ready) { return; }
		Layout::pollAll();	// saved layout files rebuild their screens
		c_.time += dt;
		if (c_.vh <= 0 && !c_.warnedViewport) { fprintf(stderr, "kclient ui: ui::frame with no viewport; call ui::setViewport(w, h) first\n"); c_.warnedViewport = true; }
		const float W = (float)c_.vw, H = (float)std::max(c_.vh, 1);
		c_.scale = std::max(0.25f, H / 1080.0f * kScale.get() / 100.0f);
		fonts::setScale(c_.scale);
		const Rect screen = { 0, 0, W, H };
		for (Widget* layer : { c_.root.get(), c_.popups.get(), c_.overlay.get() })
		{
			layer->rect = { 0, 0, W / c_.scale, H / c_.scale };
			layer->place(screen, c_.scale);
		}
		updateHover();
	}

	// Between a gfx::begin and gfx::end: every layer, back to front.
	static void drawLayers()
	{
		c_.overlay->draw();	// the game's news (toasts, zone banner, message) at the HUD's level: every window covers it (client coordinator 2026-11-05)
		c_.root->draw();
		c_.popups->draw();
		drawHints();
		if (c_.dragImage) { c_.dragImage(c_.mx, c_.my); }
		drawTooltip();
	}

	void record(dl::List& list)
	{
		if (!c_.ready) { list.begin(c_.vw, c_.vh); return; }
		gfx::begin(list, c_.vw, c_.vh);
		drawLayers();
		gfx::end();
	}

	void setViewport(int w, int h) { c_.vw = w; c_.vh = h; }

	void setClipboardFns(std::function<std::string()> get, std::function<void(const std::string&)> set)
	{
		EditBox::clipboardGet = std::move(get);
		EditBox::clipboardSet = std::move(set);
	}

	void setDragImage(std::function<void(float, float)> paint) { c_.dragImage = std::move(paint); }
	void setInterceptor(std::function<bool(const sapp_event&)> f) { c_.interceptor = std::move(f); }

	float scale() { return c_.scale; }
	bool wantsMouse() { return c_.ready && (c_.hover || c_.press || !c_.held.empty()); }
	void mouse(float& x, float& y) { x = c_.mx; y = c_.my; }
	double now() { return c_.time; }
	uint32_t mods() { return c_.mods; }
	bool hovered(const Widget* w) { return w && c_.hover == w; }
	Widget* hoveredWidget() { return c_.ready ? c_.hover : nullptr; }
	void setHints(const HintBlock& b) { c_.hints = b; }
	bool hintsShown() { return c_.ready && !currentHints().lines.empty(); }
	HintBlock shownHints() { return c_.ready ? currentHints() : HintBlock(); }
	void dockHints(std::function<Rect(float w, float h)> dock) { c_.hintsDock = std::move(dock); }

	Rect hintsRect()
	{
		if (!c_.ready || !c_.hintFont) { return {}; }
		const HintBlock b = currentHints();
		return b.lines.empty() ? Rect() : hintsLayout(b).box;
	}

	std::string claimedTip()
	{
		for (const Widget* a : c_.claimed) { if (std::string t = tipIn(a); !t.empty()) { return t; } }
		return {};
	}

	bool fixedTips() { return kHudTips.get() == "box"; }

	void setSoundSink(std::function<void(const SoundPost&)> sink) { soundSink_ = std::move(sink); }

	void sound(const std::string& event, const std::string& group, const std::string& value)
	{
		if (soundSink_ && kSounds.get() && !event.empty() && event != kSilent) { soundSink_({ event, group, value }); }
	}

	void claimTips(const Widget* area, bool on)
	{
		std::erase(c_.claimed, area);
		if (on && area) { c_.claimed.push_back(area); }
	}

	std::string tipIn(const Widget* area) { return c_.hover && !c_.press && under(c_.hover, area) ? c_.hover->tip() : std::string(); }
	bool pressed(const Widget* w) { return w && c_.press == w; }
	bool focused(const Widget* w) { return w && c_.focus == w; }
	void focus(Widget* w) { setFocus(w); }
	Widget* focusedWidget() { return c_.focus; }

	Menu& openMenu(std::unique_ptr<Menu> m, float x, float y, bool held, bool add, bool rightEdge)
	{
		if (!add) { closePopups(); }
		const float s = c_.scale;
		const Size p = m->preferred();
		const Rect screen = c_.root->screen();
		m->rect = { rightEdge ? x / s - p.w : std::clamp(x / s, 0.0f, std::max(0.0f, screen.w / s - p.w)), std::clamp(y / s, 0.0f, std::max(0.0f, screen.h / s - p.h)), p.w, p.h };
		Menu& r = static_cast<Menu&>(c_.popups->adopt(std::move(m)));
		r.layout(screen, s);
		if (held) { c_.held.push_back(&r); }
		updateHover();
		return r;
	}

	bool popupsOpen() { return c_.ready && !c_.popups->children().empty(); }

	ContextMenu& openContextMenu(float x, float y, const std::string& title, const std::vector<ContextRow>& rows, std::function<void(int)> onPick, bool second)
	{
		std::vector<Menu::Item> items = { { title, nullptr, true, true } };
		for (size_t i = 0; i < rows.size(); i++)
		{
			Menu::Item it = { rows[i].label, [onPick, i] { if (onPick) { onPick((int)i); } } };
			if (rows[i].value >= 0) { it.value = std::to_string(std::min(rows[i].value, 100)) + "%"; }
			items.push_back(std::move(it));
		}
		return static_cast<ContextMenu&>(openMenu(std::make_unique<ContextMenu>(std::move(items)), x, y, true, second, second));
	}

	bool contextMenuShown()
	{
		if (!c_.ready) { return false; }
		for (const auto& w : c_.popups->children())
		{
			if (dynamic_cast<const ContextMenu*>(w.get())) { return true; }
		}
		return false;
	}

	void closePopups()
	{
		if (!c_.ready) { return; }
		c_.held.clear();
		c_.popups->clear();
		updateHover();
	}

	void toast(const std::string& text, const Color& c, double seconds)
	{
		printf("toast: %s\n", text.c_str());
		if (c_.toasts) { c_.toasts->push(text, c, seconds); }
	}

	void toast(const std::string& text) { toast(text, res::colour("Main")); }
	Toasts* toasts() { return c_.toasts; }

	void message(const std::string& text, const Color& c, double seconds)
	{
		if (c_.message) { c_.message->show(text, c, seconds); }
	}

	void message(const std::string& text) { message(text, style().textColour(Look::kMessage, State::kNormal, false)); }

	HudMessage* messageBox() { return c_.message; }

	void banner(const std::string& text, double seconds)
	{
		if (c_.banner) { c_.banner->show(text, seconds); }
	}

	void clearMessages()
	{
		if (c_.toasts) { c_.toasts->clear(); }
		if (c_.banner) { c_.banner->clear(); }
		if (c_.message) { c_.message->clear(); }
	}

	bool messagesShown()
	{
		return (c_.toasts && !c_.toasts->empty()) || (c_.banner && c_.banner->shown()) || (c_.message && c_.message->alpha() > 0);
	}
}
