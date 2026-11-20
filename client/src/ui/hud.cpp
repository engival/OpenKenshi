#include "ui/hud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "app/config.hpp"
#include "ui/diamond.hpp"
#include "ui/keys.hpp"
#include "ui/prompt.hpp"
#include "ui/trade.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const Look kSpeedLooks[4] = { Look::kPause, Look::kSpeed1, Look::kSpeed2, Look::kSpeed3 };
		const float kTabsW = 762;	// the squad tabs' header: Kenshi_SquadTabControl, 508 wide at 720 lines
		const float kInfoW = 243;	// the status panel: Kenshi_ExtendedStatsPanelSkin's width
		const float kPlateW = 405;	// the name plate, out over the medical panel (the user's Dark UI shots: 540 px at 1440)
		const float kStatsH = 163, kSlideH = 159;	// the stats panel's height; how far the extended panel rises
		const char* const kMoveTips[4] = { "Walk", "Jog", "Run: top speed; the quick leave the slow behind",
			"Group: everyone keeps to the slowest member's pace, in formation" };

		// A stroke of thickness t from two triangles (pixels).
		void stroke(float x0, float y0, float x1, float y1, float t, const Color& c)
		{
			const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
			if (len <= 0) { return; }
			const float nx = -dy / len * t / 2, ny = dx / len * t / 2;
			gfx::triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
			gfx::triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
		}

		// A stick figure in r (ours, after the original's speed pictures), shifted dx of its width: 0 upright and walking, 1 mid-stride, 2 leaning
		// into a run. Points are in the box, -0.5 .. 0.5; the head a square.
		void figure(const Rect& r, const Color& c, int gait, float dx)
		{
			struct Seg { float x0, y0, x1, y1; };
			static const Seg kWalk[] = { { 0, -0.26f, 0, 0.06f }, { 0, -0.2f, -0.1f, 0.02f }, { 0, -0.2f, 0.1f, 0 }, { 0, 0.06f, -0.1f, 0.46f },
				{ 0, 0.06f, 0.12f, 0.46f } };
			static const Seg kRun[] = { { 0.1f, -0.24f, -0.02f, 0.06f }, { 0.08f, -0.2f, 0.2f, -0.08f }, { 0.2f, -0.08f, 0.32f, -0.16f },
				{ 0.08f, -0.2f, -0.08f, -0.12f }, { -0.08f, -0.12f, -0.16f, -0.24f }, { -0.02f, 0.06f, 0.18f, 0.2f }, { 0.18f, 0.2f, 0.14f, 0.46f },
				{ -0.02f, 0.06f, -0.12f, 0.28f }, { -0.12f, 0.28f, -0.34f, 0.3f } };
			static const Seg kJog[] = { { 0.04f, -0.25f, 0, 0.06f }, { 0.03f, -0.2f, 0.14f, -0.08f }, { 0.14f, -0.08f, 0.2f, -0.16f },
				{ 0.03f, -0.2f, -0.08f, -0.08f }, { -0.08f, -0.08f, -0.12f, 0 }, { 0, 0.06f, 0.14f, 0.24f }, { 0.14f, 0.24f, 0.12f, 0.46f },
				{ 0, 0.06f, -0.06f, 0.26f }, { -0.06f, 0.26f, -0.2f, 0.4f } };
			static const float kHeadX[3] = { 0.02f, 0.06f, 0.14f }, kHeadY[3] = { -0.38f, -0.37f, -0.36f };
			const float cx = r.x + r.w * (0.5f + dx), cy = r.y + r.h / 2, t = 0.08f * r.h, head = 0.16f * r.h;
			const float hx = kHeadX[gait], hy = kHeadY[gait];
			gfx::fill({ std::round(cx + hx * r.w - head / 2), std::round(cy + hy * r.h - head / 2), head, head }, c);
			auto draw = [&](const auto& segs) { for (const Seg& g : segs) { stroke(cx + g.x0 * r.w, cy + g.y0 * r.h, cx + g.x1 * r.w, cy + g.y1 * r.h, t, c); } };
			if (gait == 2) { draw(kRun); }
			else if (gait == 1) { draw(kJog); }
			else { draw(kWalk); }
		}

		const cfg::Text kSpeedIcons(cfg::Group{ "HUD and labels" }, "ui.speed_icons", "atlas", { "atlas", "drawn" },
			"The move speed diamond's pictures: atlas (the game's walk / jog / run / group icons) or drawn (our stick figures).");

		// The speed diamond's picture by mode: the atlas's icon (Kenshi_CharacterSpeedSet/Icons, kenshi-inspect ui_captures/hud_floor_speed.md;
		// its own colours, the ink's alpha only), or ours: a walker, a jogger, a runner; group: two runners, the rear one faded. atlas: only when
		// a theme (Dark UI) ships the atlas: its figures are light grey for a dark panel; vanilla's are black, for parchment, lost on our diamond.
		void mover(const Rect& r, const Color& c, int mode, bool atlas)
		{
			static const char* const kIcon[4] = { "Walk", "Jog", "Run", "Group" };
			const ImageSet* is = atlas ? res::images("Kenshi_CharacterSpeedSet") : nullptr;
			auto it = is ? is->frames.find(kIcon[std::clamp(mode, 0, 3)]) : std::map<std::string, Rect>::const_iterator{};
			if (is && it != is->frames.end())
			{
				// The figure fills about 3/5 of its frame; the user's 1440p shot: about 19 units tall, the frame 32 (0.88 of the picture box).
				const float k = 0.88f * std::min(r.w, r.h);
				gfx::sprite({ std::round(r.x + r.w / 2 - k / 2), std::round(r.y + r.h / 2 - k / 2), std::round(k), std::round(k) }, it->second, Color{}.alpha(c.a));
				return;
			}
			if (mode == 3)
			{
				figure(r, c.alpha(0.4f), 2, -0.14f);
				figure(r, c, 2, 0.1f);
			} else {
				figure(r, c, std::clamp(mode, 0, 2), 0);
			}
		}

		// A medical slat, the original's seven layers bottom to top (kenshi-inspect findings/medical_bars.md): stunned (always full: what shows past
		// the rest is the stun), hurt, healed (bandaging), splint, the health itself (fraction), crushed (a full overlay), robot (over the health
		// at its width). Each is a fraction of the slat from its left.
		class Bar : public Widget
		{
		public:
			float fraction = 1, hurt = 1, healed = 0, splint = 0;
			bool robot = false, crushed = false;
			Color tint{ 0, 0, 0, 0 };	// alpha > 0: a building bar, this fill over HudBarTrack
			bool slat = false;	// a spare or unknown row: an empty slat

		protected:
			void cut(Look l, float f) const
			{
				if (f <= 0) { return; }
				gfx::pushClip({ screen_.x, screen_.y, std::round(screen_.w * std::min(1.0f, f)), screen_.h });
				style().draw(l, screen_, scale_, State::kNormal, false);
				gfx::popClip();
			}

			void paint() const override
			{
				if (slat)
				{
					const float e = std::max(1.0f, std::round(scale_));
					gfx::fill(screen_, res::colour("HudBarEdge"));
					gfx::fill({ screen_.x + e, screen_.y + e, screen_.w - 2 * e, screen_.h - 2 * e }, res::colour("HudBarSlat"));
					return;
				}
				if (tint.a > 0)
				{
					gfx::fill(screen_, res::colour("HudBarTrack"));
					if (fraction > 0) { gfx::fill({ screen_.x, screen_.y, std::round(screen_.w * std::min(1.0f, fraction)), screen_.h }, tint); }
					return;
				}
				style().draw(Look::kBarStun, screen_, scale_, State::kNormal, false);
				cut(Look::kBarHurt, hurt);
				cut(Look::kBarHealed, healed);
				cut(Look::kBarSplint, splint);
				cut(Look::kBarFill, fraction);
				if (crushed) { style().draw(Look::kBarCrushed, screen_, scale_, State::kNormal, false); }
				if (robot) { cut(Look::kBarRobot, fraction); }
			}
		};

		std::string whole(float v)
		{
			char b[16];
			snprintf(b, sizeof b, "%d", (int)std::lround(v));
			return b;
		}

		std::string hhmm(float hour)
		{
			const int minutes = (int)std::floor(hour * 60);
			char b[16];
			snprintf(b, sizeof b, "%02d:%02d", minutes / 60 % 24, minutes % 60);
			return b;
		}
	}

	struct Hud::Portrait
	{
		uint64_t id = 0;
		Button* frame = nullptr;
		PortraitFace* face = nullptr;	// inside the frame: the rendered portrait and its layers
		Label* initial = nullptr;	// over the layers while there is no portrait image
		Label* name = nullptr;
		Bar* bar = nullptr;
	};

	// The bottom-centre column (RUN / STOP, the squad tabs, the portrait bar), stretched over the space between the corner clusters (update
	// sets the margins), so it never runs under them at any aspect (kenshi-k16.3.41). Laid out into that span: the tabs share min(header,
	// span); portraits that don't fit page behind < > arrows at the bar's ends (and the wheel over it).
	class Hud::Bottom : public Box
	{
	public:
		Bottom() : Box(true, 6) {}
		Box* tabs = nullptr;
		Box* squad = nullptr;
		Button *prev = nullptr, *next = nullptr;
		size_t first = 0;
		static constexpr float kArrowW = 24;

		void place(const Rect& px, float s) override
		{
			const float span = px.w / s;
			const size_t nTabs = tabs->children().size();
			for (const auto& t : tabs->children()) { t->rect.w = std::min(std::min(kTabsW, span) / nTabs - tabs->spacing, 200.0f); }
			const auto& cols = squad->children();
			const size_t n = cols.size();
			float total = 0;
			for (const auto& c : cols) { c->visible = true; total += c->preferred().w + squad->spacing; }
			total -= squad->spacing;
			const bool paged = squad->visible && n > 1 && total > span;
			prev->visible = next->visible = paged;
			if (!paged) { first = 0; }
			else
			{
				const float pitch = total / n + squad->spacing * (n - 1) / n;	// a column and its gap
				const size_t fit = (size_t)std::max(1.0f, std::floor((span - 2 * (kArrowW + squad->spacing) + squad->spacing) / pitch));
				first = std::min(first, n - std::min(n, fit));
				for (size_t i = 0; i < n; i++) { cols[i]->visible = i >= first && i < first + fit; }
				prev->enabled = first > 0;
				next->enabled = first + fit < n;
			}
			Box::place(px, s);
		}

		bool onScroll(float dy) override
		{
			if (!prev->visible) { return false; }
			if (dy > 0 && prev->enabled) { first--; }
			else if (dy < 0 && next->enabled) { first++; }
			return true;
		}
	};

	// The status panel's rows: label left, value right-aligned at the right edge, a fixed pitch.
	class Hud::Rows : public Widget
	{
	public:
		std::vector<HudLine> lines;
		Color labelInk, valueInk;
		float rowH = 21;	// the original's 14 px at 720 lines
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");

		std::string tip() const override
		{
			float x, y;
			mouse(x, y);
			const int i = (int)std::floor((y - screen_.y) / (rowH * scale_));
			return screen_.contains(x, y) && i >= 0 && i < (int)lines.size() ? lines[i].tip : tooltip;
		}

	protected:
		void paint() const override
		{
			if (!font) { return; }
			for (size_t i = 0; i < lines.size(); i++)
			{
				const HudLine& l = lines[i];
				const float y = std::round(screen_.y + (i * rowH * scale_) + (rowH * scale_ - font->lineHeight()) / 2);
				font->draw(screen_.x, y, l.label, labelInk);
				font->draw(screen_.right() - font->width(l.value), y, l.value, l.colour.empty() ? valueInk : res::colour(l.colour, valueInk));
			}
		}
	};

	// The tag row: the tag left, a value right-aligned at the right edge.
	class Hud::TagRow : public Widget
	{
	public:
		std::string text, value;
		Color textInk, valueInk;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");

		std::string drawn() const { return text + (value.empty() ? "" : " " + value); }

	protected:
		void paint() const override
		{
			if (!font) { return; }
			const float y = std::round(screen_.y + (screen_.h - font->lineHeight()) / 2);
			font->draw(screen_.x, y, text, textInk);
			if (!value.empty()) { font->draw(screen_.right() - font->width(value), y, value, valueInk); }
		}
	};

	// A hover text's lines as layoutRich's markup.
	static std::string tipMarkup(const std::vector<HudTipLine>& lines)
	{
		std::string out;
		for (const HudTipLine& l : lines)
		{
			if (!out.empty()) { out += '\n'; }
			if (!l.labelColour.empty()) { out += "{" + l.labelColour + "}"; }
			out += l.label;
			if (l.value.empty()) { continue; }
			out += '\t';
			if (!l.valueColour.empty()) { out += "{" + l.valueColour + "}"; }
			out += l.value;
		}
		return out;
	}

	struct Hud::Limb
	{
		Bar* bar = nullptr;
		Label* name = nullptr;
		Label* value = nullptr;
		Label* right = nullptr;	// a building bar's
		Widget* row = nullptr;
	};

	Hud::Hud(HudActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		top_ = &root().add<Widget>();
		top_->halign = top_->valign = A::kStretch;

		// PAUSED, top centre (Kenshi_MainPanel.layout PausedPanel: 0.40 0.005 0.19 0.10 of the screen, Kenshi_UI_Messages).
		Panel& paused = top_->add<Panel>(Look::kPanel, 0);
		paused.halign = A::kCenter;
		paused.rect = { 0, 5, 370, 110 };
		paused.visible = false;
		Label& pausedText = paused.add<Label>("PAUSED", "Kenshi_UI_Messages");
		pausedText.halign = pausedText.valign = A::kCenter;
		pausedText.colour = res::colour("Banner");
		pausedPanel_ = &paused;

		// Spectating: the Watching panel, top centre (PAUSED moves under it): who, the goal and task on one clipped line each, FOLLOW / STOP and
		// CAST. Our own; the original has no spectator.
		watch_ = &top_->add<Panel>(Look::kPanel, 0);
		watch_->halign = A::kCenter;
		watch_->rect = { 0, 6, 640, 64 };
		watch_->visible = false;
		Flex& wrow = watch_->add<Flex>(false, 8, 8);
		wrow.halign = wrow.valign = A::kStretch;
		Flex& wcol = wrow.add<Flex>(FlexItem{ .fill = 1 }, true, 0);
		wcol.valign = A::kStretch;
		for (Label** l : { &watchName_, &watchGoal_ })
		{
			Widget& cell = wcol.add<Widget>(FlexItem{ .basis = 24 });
			cell.halign = A::kStretch;
			cell.clipChildren = true;
			cell.solid = true;	// for the tip
			*l = &cell.add<Label>("", "Kenshi_StandardFont_Medium");
			(*l)->valign = A::kCenter;
			(*l)->colour = res::colour("Main");
		}
		followBtn_ = &wrow.add<Button>(FlexItem{ .basis = 100 }, "FOLLOW", Look::kButton, "Kenshi_PaintedTextFont_Small");
		followBtn_->valign = A::kCenter;
		followBtn_->rect.h = 34;
		followBtn_->textColour = res::colour("Main");
		followBtn_->onClicked = [this]
		{
			if (!act_.follow || !last_.watch.id) { return; }
			act_.follow(last_.watch.following ? 0 : last_.watch.id);
		};
		castBtn_ = &wrow.add<Button>(FlexItem{ .basis = 80 }, "CAST", Look::kButton, "Kenshi_PaintedTextFont_Small");
		castBtn_->valign = A::kCenter;
		castBtn_->rect.h = 34;
		castBtn_->textColour = res::colour("Main");
		castBtn_->tooltip = "The characters here: click one to inspect it, double click to follow";
		castBtn_->onClicked = [this] { castOpen_ = !castOpen_; if (cast_) { cast_->show(castOpen_); } };

		// The bottom-right cluster, every rect anchored to the screen's bottom-right corner (x, y: the right and bottom margins) at Dark UI's spots
		// (its Kenshi_MainPanel.layout at 1920 x 1080, the user's muscle memory; vanilla has the same columns, higher): the floor diamond
		// [1374,743,101,101] over the move speed [1334,812,101,101]. The speed and money boxes are where Dark UI's skins draw them, inside their
		// layout rects (the user's 1440p shots): the speed box [1437,824,202,58], left-aligned on the orders panel, its top-left corner cut;
		// the money box [1645,824,191,58]. 6 between panels.
		Panel& time = top_->add<Panel>(Look::kSpeedPanel, 0);
		time.halign = time.valign = A::kEnd;
		time.rect = { 281, 198, 202, 58 };
		const char* tips[4] = { "Pause", "Normal speed", "Speed x2", "Speed x5" };
		for (int i = 0; i < 4; i++)
		{
			Button& b = time.add<Button>("", kSpeedLooks[i]);
			b.rect = { 9 + 46.0f * i, 7, 46, 44 };
			b.tooltip = tips[i];
			b.sound = kSilent;	// the host posts Pause_Game / Resume_Game / Game_Speed_*
			b.onClicked = [this, i]
			{
				if (i == 0) { if (act_.pause) { act_.pause(!last_.paused); } return; }
				if (act_.setSpeed) { act_.setSpeed(kSpeeds[i - 1]); }
				if (last_.paused && act_.pause) { act_.pause(false); }
			};
			speed_[i] = &b;
		}
		time_ = &time;

		// The diamonds overlap like tiles (kenshi-inspect ui_captures/hud_floor_speed.md): the move speed 40 left of and 69 under the floor's.
		Widget& dia = top_->add<Widget>();
		dia.halign = dia.valign = A::kEnd;
		dia.rect = { 445, 167, 141, 170 };
		diamonds_ = &dia;
		floor_ = &dia.add<Diamond>(true);
		floor_->rect = { 40, 0, 101, 101 };
		floor_->onStep = [this](int side)
		{
			sound("Change_Level");
			if (act_.floor) { act_.floor(-side); }
		};
		moveSpeed_ = &dia.add<Diamond>(false);
		moveSpeed_->rect = { 0, 69, 101, 101 };
		moveSpeed_->onStep = [this](int side) { if (act_.moveSpeed) { act_.moveSpeed((last_.moveSpeed + side + 4) % 4); } };
		speedAtlas_ = kSpeedIcons.get() == "atlas" && res::themed("gfx/Kenshi_UI.png") && res::images("Kenshi_CharacterSpeedSet");
		moveSpeed_->picture = [this](const Rect& r, const Color& ink) { mover(r, ink, last_.moveSpeed, speedAtlas_); };

		// Selection toggles above the squad bar, bottom centre: the column spans the space between the corner clusters (update sets its margins).
		Bottom& bottom = top_->add<Bottom>();
		bottom.halign = A::kStretch;
		bottom.valign = A::kEnd;
		bottom.rect = { 0, 16, 0, 0 };
		bottom_ = &bottom;
		toggles_ = &bottom.add<Box>(false, 4);
		toggles_->halign = A::kCenter;
		auto tick = [this](Widget& parent, HudToggle t, const char* caption, const char* tip)
		{
			Button& b = parent.add<Button>(caption, Look::kToggle, "Kenshi_StandardFont_MediumSmall");
			b.tooltip = tip;
			if (t == HudToggle::kSneak) { b.sound = kSilent; }	// the host's action posts Stealth / General_Click (the original's 140721410), not a generic click
			b.onClicked = [this, t, &b] { if (act_.toggle) { act_.toggle(t, !b.checked); } };
			toggle_[(int)t] = &b;
			return &b;
		};
		tick(*toggles_, HudToggle::kRun, "RUN", "Run");
		Button& stop = toggles_->add<Button>("STOP", Look::kToggle, "Kenshi_StandardFont_MediumSmall");
		stop.tooltip = "Stop what they are doing";
		stop.onClicked = [this] { if (act_.stop) { act_.stop(); } };
		tabRow_ = &bottom.add<Box>(false, 4);
		tabRow_->halign = A::kCenter;
		Box& strip = bottom.add<Box>(false, 6);	// the portrait bar between its page arrows
		strip.halign = A::kCenter;
		bottom.prev = &strip.add<Button>("", Look::kScrollLeft);
		squad_ = &strip.add<Box>(false, 6);
		bottom.next = &strip.add<Button>("", Look::kScrollRight);
		for (Button* b : { bottom.prev, bottom.next })
		{
			b->rect = { 0, 0, Bottom::kArrowW, 72 };
			b->visible = false;
		}
		bottom.prev->tooltip = "Earlier squad members";
		bottom.next->tooltip = "Later squad members";
		bottom.prev->onClicked = [&bottom] { if (bottom.first > 0) { bottom.first--; } };
		bottom.next->onClicked = [&bottom] { if (bottom.next->enabled) { bottom.first++; } };
		bottom.tabs = tabRow_;
		bottom.squad = squad_;

		// The ShortcutsPanel left of the OrdersPanel (Dark UI [1356,914,77,255], its foot off the screen): a button per window, the tooltip naming
		// its key.
		Panel& sc = top_->add<Panel>(Look::kPanel, 0);
		sc.halign = sc.valign = A::kEnd;
		sc.rect = { 489, 0, 77, 166 };
		shortcuts_ = &sc;
		const char* captions[6] = { "INV", "STA", "MAP", "TCH", "SQD", "HLP" };
		for (int i = 0; i < 6; i++)
		{
			Button& b = sc.add<Button>(captions[i], Look::kButton, "Kenshi_PaintedTextFont_Medium");
			b.rect = { 7, 5 + 26.0f * i, 63, 25 };
			b.onClicked = [this, i] { if (act_.shortcut) { act_.shortcut(kShortcuts[i]); } };
			shortcut_[i] = &b;
		}

		// A playtest run: NOTES left of the ShortcutsPanel at the screen's foot (above it is the move speed diamond); the cluster itself stays.
		// Dev builds only (kenshi-k16.2.153.2): without KCLIENT_DEV_TOOLS there is no button and notesButton() is null.
#ifdef KCLIENT_DEV_TOOLS
		Panel& notes = top_->add<Panel>(Look::kPanel, 0);
		notes.halign = notes.valign = A::kEnd;
		notes.rect = { 489 + 77 + 6, 0, 77, 34 };
		notes.visible = false;
		notesBtn_ = &notes.add<Button>("NOTES", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		notesBtn_->rect = { 7, 5, 63, 25 };
		notesBtn_->onClicked = [this] { if (act_.notes) { act_.notes(); } };
#endif

		// The TimeMoneyPanel right of the speed buttons: "Money:" and the cats over "Day: N" and the time, in Dark UI's text (#9c9c9c, a black
		// shadow: Kenshi_TextboxStandardText), 11 in from the left, 13 from the right.
		Panel& money = top_->add<Panel>(Look::kHudPanel, 0);
		money.halign = money.valign = A::kEnd;
		money.rect = { 84, 198, 191, 58 };
		money_ = &money;
		auto text = [&money](const char* s, A h, const Rect& r)
		{
			Label& l = money.add<Label>(s, "Kenshi_StandardFont_Medium");
			l.halign = h;
			l.rect = r;
			return &l;
		};
		moneyLabel_ = text("Money:", A::kStart, { 11, 7, 64, 20 });
		moneyAmount_ = text("", A::kEnd, { 13, 7, 0, 20 });
		day_ = text("", A::kStart, { 11, 32, 97, 20 });
		clock_ = text("", A::kEnd, { 13, 32, 0, 20 });

		// The BuildPanel in the corner right of the money box (Dark UI's [1838,823,83,65]; its skin draws [1842,824,78,58]): the build button,
		// the toggle_build key's action.
		Panel& build = top_->add<Panel>(Look::kBuildPanel, 0);
		build.halign = build.valign = A::kEnd;
		build.rect = { 0, 198, 78, 58 };
		build_ = &build.add<Button>("", Look::kBuild);
		build_->rect = { 7, 6, 64, 46 };
		build_->tooltip = "Build";
		build_->onClicked = [this] { if (act_.shortcut) { act_.shortcut("toggle_build"); } };

		// OrdersPanel, the bottom-right corner (Dark UI [1437,888,483,193]): the order ticks down its left (Kenshi_TickButton1, 118 x 25 at 8, 6,
		// 26 apart), the jobs list (the frame at 128, 7, 350 x 156), RESCUE / MEDIC / PROSPECT along the bottom under it.
		Panel& orders = top_->add<Panel>(Look::kPanel, 0);
		orders.halign = orders.valign = A::kEnd;
		orders.rect = { 0, 0, 483, 193 };
		orders_ = &orders;
		Widget& ticks = orders.add<Widget>();
		ticks.rect = { 8, 6, 118, 181 };
		ticks_ = &ticks;
		const struct { HudToggle t; const char* caption; const char* tip; } kTicks[] = {
			{ HudToggle::kBlock, "BLOCK", "Block: hold a defensive stance" },
			{ HudToggle::kHold, "HOLD", "Hold: stay in place and fight from there" },
			{ HudToggle::kPassive, "PASSIVE", "Passive: never start a fight, only defend" },
			{ HudToggle::kJobs, "JOBS", "Jobs: work through the jobs list (off: only the jobs that always run)" },
			{ HudToggle::kRanged, "RANGED", "Ranged: fight with a ranged weapon" },
			{ HudToggle::kTaunt, "TAUNT", "Taunt: draw the enemy's attacks" },
			{ HudToggle::kSneak, "SNEAK", "Sneak: move in stealth mode" } };
		for (size_t i = 0; i < std::size(kTicks); i++) { tick(ticks, kTicks[i].t, kTicks[i].caption, kTicks[i].tip)->rect = { 0, 26.0f * i, 118, 25 }; }
		auto button = [&orders](const char* caption, const char* font, const Rect& r, std::function<void()>& action)
		{
			Button& b = orders.add<Button>(caption, Look::kButton, font);
			b.rect = r;
			b.onClicked = [&action] { if (action) { action(); } };
			return &b;
		};
		rescue_ = button("RESCUE", "Kenshi_PaintedTextFont_Small", { 131, 162, 112, 28 }, act_.rescue);
		medic_ = button("MEDIC", "Kenshi_PaintedTextFont_Small", { 247, 162, 112, 28 }, act_.medic);
		prospect_ = button("PROSPECT", "Kenshi_PaintedTextFont_Small", { 363, 162, 112, 28 }, act_.prospect);
		prospect_->sound = "Prospect_Click";
		jobs_ = &orders.add<JobList>();
		jobs_->rect = { 128, 7, 350, 152 };
		jobs_->visible = false;
		jobs_->onRemove = [this](uint64_t id) { if (act_.removeJob) { act_.removeJob(id); } };
		jobs_->onMove = [this](uint64_t id, int index) { if (act_.moveJob) { act_.moveJob(id, index); } };

		// The medical panel, bottom left: a title, then the original's slats (Dark UI's LifeBar1..9, 238 x 23, 24 apart): Blood, a row per body part,
		// Hunger, each the name centred in the bar, the arrows at its right end, the value right of it. Unused rows are hidden.
		Panel& med = top_->add<Panel>(Look::kPanel, 10);
		med.valign = A::kEnd;
		med.rect = { kInfoW + 8, 0, 0, 0 };	// right of the status panel, on the screen's foot
		med.visible = false;
		medical_ = &med;
		Box& col = med.add<Box>(true, 6);	// the original's pitch: 24 (a bar 18, a gap 6)
		// The original's HealthText box ("Normal", "Discovered!"): its own frame over the medical panel, as wide, 37 high, 7 above it (update
		// places it on the panel's size); the text left at 10. No name row: the name is on the status panel's plate.
		status_ = &top_->add<Panel>(Look::kPanel, 0);
		status_->valign = A::kEnd;
		status_->rect = { kInfoW + 8, 0, 0, 37 };
		status_->solid = true;	// for its tip
		statusText_ = &status_->add<Label>("", "Kenshi_StandardFont_Medium");
		statusText_->fit = false;
		statusText_->halign = statusText_->valign = A::kStretch;
		statusText_->rect = { 10, 0, 10, 0 };

		// The original's fixed tip box (Dark UI TooltipPanel [559,809,398,72], 199 off the bottom): right of the medical panel (update places it
		// beside its width), the HUD's hover text in it instead of at the pointer; shown only while there is one.
		hintBox_ = &top_->add<Panel>(Look::kPanel, 0);
		hintBox_->valign = A::kEnd;
		hintBox_->rect = { kInfoW + 8, 199, 398, 72 };
		hint_ = &hintBox_->add<RichText>("", "Kenshi_StandardFont_Medium");
		hint_->rect = { 12, 10, 374, 52 };

		// The status panel, bottom left (InformationPanel; kenshi-inspect hud_panel16.md): the stats panel (163 high) stays put and is drawn on
		// top; above it the slider holds the tag, the name plate and the extended panel (271: the state rows, then the extended rows). Collapsed,
		// the extended rows sit behind the stats; the ^ button slides the lot up 159 to show them. Sizes are the layout's at 1080 lines, the
		// plate out over the medical panel as Dark UI's (it clears the status box). Offsets are from the bottom; the panel sits in the screen's
		// corner (user: the left panels on the screen's edges, as the right ones).
		info_ = &top_->add<Widget>();
		info_->valign = A::kEnd;
		info_->rect = { 0, 0, kPlateW, 520 };
		info_->visible = false;
		const Color inset = style().textColour(Look::kInset, State::kNormal, false);
		Widget& clip = info_->add<Widget>();	// what slides is cut at the stats panel's top (the flat panel is not quite opaque)
		clip.valign = A::kEnd;
		clip.rect = { 0, kStatsH - 2, kPlateW, 520 - (kStatsH - 2) };
		clip.clipChildren = true;
		slider_ = &clip.add<Widget>();
		slider_->valign = A::kEnd;
		slider_->rect = { 0, 0, kPlateW, 353 };
		extPanel_ = &slider_->add<Panel>(Look::kExtStatsPanel, 0);
		extPanel_->valign = A::kEnd;
		extPanel_->rect = { 0, 0, kInfoW, 271 };	// collapsed its top is 275 off the foot, level with the status box's (Dark UI)
		state_ = &extPanel_->add<Rows>();
		state_->solid = true;	// for the rows' tips
		state_->rect = { 8, 11, kInfoW - 16, 84 };
		ext_ = &extPanel_->add<Rows>();
		ext_->rect = { 8, 111, kInfoW - 22, 126 };
		state_->labelInk = state_->valueInk = ext_->valueInk = inset;
		ext_->labelInk = res::colour("Main").alpha(0.8f);
		plate_ = &slider_->add<Panel>(Look::kNamePlate, 0);
		plate_->valign = A::kEnd;
		plate_->rect = { 0, 265, kPlateW, 61 };
		for (Label** l : { &name_, &faction_ })
		{
			*l = &plate_->add<Label>("", "Kenshi_StandardFont_Medium");
			(*l)->fit = false;
			(*l)->halign = (*l)->valign = A::kStretch;
			(*l)->rect = { 12, 17, 12, 12 };	// the flat plate's box
		}
		faction_->align = TextAlign::kEnd;
		tag_ = &slider_->add<Panel>(Look::kField, 0);
		tag_->valign = A::kEnd;
		tag_->rect = { 0, 313, 209, 40 };
		tag_->solid = true;	// for its tip
		tagRow_ = &tag_->add<TagRow>();
		tagRow_->halign = tagRow_->valign = A::kStretch;
		tagRow_->rect = { 8, 8, 8, 8 };
		statsPanel_ = &info_->add<Panel>(Look::kStatsPanel, 0);
		statsPanel_->valign = A::kEnd;
		statsPanel_->rect = { 0, 0, kInfoW, kStatsH };
		stats_ = &statsPanel_->add<Rows>();
		stats_->rect = { 8, 14, kInfoW - 22, 140 };
		stats_->labelInk = res::colour("Main").alpha(0.8f);
		stats_->valueInk = inset;
		// A building's buttons under the state rows and ticks under the stats rows (placed by updateBuilding).
		bldButtonBox_ = &extPanel_->add<Box>(true, 3);
		bldButtonBox_->rect = { 8, 0, kInfoW - 16, 0 };
		bldTickBox_ = &statsPanel_->add<Box>(true, 3);
		bldTickBox_->rect = { 8, 0, kInfoW - 16, 0 };
		extButton_ = &info_->add<Button>("", Look::kExtStatsButton);
		extButton_->valign = A::kEnd;
		extButton_->rect = { (kInfoW - 36) / 2, 141, 36, 33 };
		extButton_->tooltip = "Show or hide more of the character's state";
		extButton_->sound = kSilent;
		extButton_->onClicked = [this]
		{
			extended_ = !extended_;
			sound("Stats_Preview", "Stats_Preview", extended_ ? "Stats_Up" : "Stats_Down");
			slideFrom_ = slide_;
			slideAt_ = now();
			extButton_->checked = extended_;
			if (act_.extend) { act_.extend(extended_); }
		};
		place();
		dockHints([this](float w, float h) { return hintsDock(w, h); });
		for (int i = 0; i < kLimbs; i++)
		{
			auto* l = new Limb;
			Box& row = col.add<Box>(false, 6);
			l->bar = &row.add<Bar>();
			l->bar->rect = { 0, 0, 238, 18 };
			l->bar->solid = true;	// for its tooltip
			l->name = &l->bar->add<Label>("", "Kenshi_StandardFont_MediumSmall");
			l->name->fit = false;
			l->name->shadow = true;
			l->name->halign = l->name->valign = A::kStretch;
			l->name->rect = { 8, 0, 8, 0 };
			l->right = &l->bar->add<Label>("", "Kenshi_StandardFont_MediumSmall");
			l->right->fit = false;
			l->right->shadow = true;
			l->right->align = TextAlign::kEnd;
			l->right->halign = l->right->valign = A::kStretch;
			l->right->rect = { 8, 0, 8, 0 };
			l->value = &row.add<Label>("", "Kenshi_StandardFont_Medium");
			l->value->fit = false;
			l->value->align = TextAlign::kEnd;
			l->value->shadow = true;
			l->value->rect = { 0, 0, 44, 18 };
			l->row = &row;
			limbs_[i] = l;
		}
	}

	Hud::~Hud()
	{
		prompt_.reset();
		for (Limb* l : limbs_) { delete l; }
		for (Portrait* p : portraits_) { delete p; }
		if (ready()) { dockHints(nullptr); }
		if (ready() && top_) { root().remove(top_); }
	}

	float Hud::panelGap() const { return build_->parent()->screen().x - money_->screen().right(); }

	Rect Hud::hintsDock(float w, float h) const
	{
		if (!top_->visible) { return {}; }
		const float right = build_->parent()->screen().right();
		float top = top_->screen().bottom();
		for (const Widget* p : std::initializer_list<const Widget*>{ time_, money_, build_->parent(), floor_, moveSpeed_, orders_ })
		{
			const Rect r = p->screen();
			if (p->visible && r.right() > right - w && r.x < right) { top = std::min(top, r.y); }
		}
		const float foot = std::round(top - panelGap());
		return { std::round(right - w), foot - h, w, h };
	}

	Widget* Hud::squadSpan() const { return bottom_; }
	Widget* Hud::squadArrow(int side) const { return side < 0 ? bottom_->prev : bottom_->next; }
	size_t Hud::squadFirst() const { return bottom_->first; }
	Widget* Hud::portrait(size_t i) const { return i < portraits_.size() ? portraits_[i]->frame : nullptr; }
	Widget* Hud::portraitMark(size_t i) const { return i < portraits_.size() ? portraits_[i]->face->mark : nullptr; }
	const PortraitFace* Hud::portraitFace(size_t i) const { return i < portraits_.size() ? portraits_[i]->face : nullptr; }
	const Label* Hud::portraitInitial(size_t i) const { return i < portraits_.size() ? portraits_[i]->initial : nullptr; }
	Widget* Hud::medicalBar(int i) const { return i >= 0 && i < kLimbs ? limbs_[i]->bar : nullptr; }
	Widget* Hud::medicalValue(int i) const { return i >= 0 && i < kLimbs ? limbs_[i]->value : nullptr; }
	Label* Hud::medicalRight(int i) const { return i >= 0 && i < kLimbs ? limbs_[i]->right : nullptr; }
	size_t Hud::infoRows() const { return state_->lines.size() + ext_->lines.size() + stats_->lines.size(); }
	float Hud::medicalFill(int i) const { return i >= 0 && i < kLimbs ? limbs_[i]->bar->fraction : 0; }

	bool Hud::shown() const { return top_->visible; }
	const std::string& Hud::hintText() const { return hint_->text; }
	std::string Hud::infoTagText() const { return tagRow_->drawn(); }
	Color Hud::infoTagInk(bool value) const { return value ? tagRow_->valueInk : tagRow_->textInk; }
	void Hud::show(bool on) { top_->visible = on; }

	const std::string& Hud::watchText() const { return watchName_->text; }
	const std::string& Hud::watchGoalText() const { return watchGoal_->text; }

	void Hud::updateSpectator(const HudState& s)
	{
		if (s.spectating != spectating_)
		{
			spectating_ = s.spectating;
			if (spectating_ && !cast_)
			{
				CastActions ca;
				ca.watch = [this](uint64_t id) { if (act_.watch) { act_.watch(id); } };
				ca.follow = [this](uint64_t id) { if (act_.follow) { act_.follow(id); } };
				ca.close = [this] { castOpen_ = false; };
				cast_ = std::make_unique<CastWindow>(std::move(ca));
			}
			castOpen_ = spectating_;	// opens each time spectating starts
			if (cast_) { cast_->show(castOpen_); }
		}
		watch_->visible = spectating_;
		pausedPanel_->rect.y = spectating_ ? 6 + 64 + 6 : 5;
		if (!spectating_) { return; }
		if (cast_)
		{
			cast_->update(s.cast, s.watch.id);
			if (cast_->shown() != castOpen_) { cast_->show(castOpen_); }
		}
		const HudWatch& w = s.watch;
		const std::string name = w.name.empty() ? "#" + std::to_string(w.id) : w.name;
		watchName_->text = w.id ? "Watching: " + name + (w.faction.empty() ? "" : "   " + w.faction) + (w.following ? "   (following)" : "") : "Watching: nobody";
		watchName_->colour = w.following ? res::colour("Accent") : res::colour("Main");
		watchName_->parent()->tooltip = w.id ? name + (w.faction.empty() ? "" : "\n" + w.faction) : "Click a character in the world or in CAST";
		const std::string goal = w.goal.empty() ? "-" : w.goal, task = w.task.empty() ? "-" : w.task;
		watchGoal_->text = "Goal: " + goal + "   Task: " + task;
		watchGoal_->parent()->tooltip = w.id ? "Goal: " + goal + "\nTask: " + task : std::string();
		followBtn_->text = w.following ? "STOP" : "FOLLOW";
		followBtn_->enabled = w.id != 0;
		followBtn_->tooltip = w.following ? "Stop following" : "The camera follows this character";
		castBtn_->checked = castOpen_;
	}

	void Hud::update(const HudState& s)
	{
		last_ = s;
		pausedPanel_->visible = s.paused;
		updateSpectator(s);
		const bool box = fixedTips();
		claimTips(top_, box);
		hint_->text = box ? claimedTip() : std::string();
		hintBox_->visible = !hint_->text.empty();	// only while there is a tip (user, 2026-11-03; the original keeps it up)
		moneyLabel_->visible = moneyAmount_->visible = s.money >= 0;
		if (s.money >= 0) { moneyAmount_->text = trade::cats(s.money); }
		const Color ink = res::colour("HudMoney", res::colour("Main"));	// Dark UI's grey on our panel, dark on the original's parchment
		for (Label* l : { moneyLabel_, moneyAmount_, day_, clock_ }) { l->colour = ink; l->shadow = ink.r + ink.g + ink.b > 1.2f; }	// a black shadow under light ink only
		if (slideAt_ >= 0)	// the slide: an ease in and out, the time in proportion to the way left
		{
			const float to = extended_ ? 1.0f : 0.0f, way = std::fabs(to - slideFrom_);
			const float t = way > 0 ? std::min(1.0f, (float)((now() - slideAt_) / (kSlideTime * way))) : 1.0f;
			slide_ = slideFrom_ + (to - slideFrom_) * t * t * (3 - 2 * t);
			if (t >= 1) { slideAt_ = -1; }
			place();
		}
		for (int i = 0; i < 6; i++)
		{
			const keys::Action* a = keys::find(kShortcuts[i]);
			const std::string key = keys::label(kShortcuts[i]);
			shortcut_[i]->tooltip = (a ? a->title : kShortcuts[i]) + (key.empty() ? "" : " (" + key + ")");
		}
		if (notesBtn_)
		{
			notesBtn_->parent()->visible = s.playtestRun;
			notesBtn_->checked = s.notesOpen;
			const std::string notesKey = keys::label("playtest.notes");
			notesBtn_->tooltip = "Playtest notes" + (notesKey.empty() ? "" : " (" + notesKey + ")");
		}
		day_->text = s.clockValid ? "Day: " + std::to_string(s.day) : "Day: --";
		clock_->text = s.clockValid ? hhmm(s.hour) : "--:--";
		int cur = 0;
		if (!s.paused)
		{
			float best = 1e9f;
			for (int i = 0; i < 3; i++) { if (std::fabs(kSpeeds[i] - s.speed) < best) { best = std::fabs(kSpeeds[i] - s.speed); cur = i + 1; } }
		}
		for (int i = 0; i < 4; i++)
		{
			speed_[i]->checked = s.clockValid && i == cur;
			speed_[i]->enabled = s.clockValid;
		}
		floor_->visible = s.floorValid;
		const bool target = s.floorUp || s.floorDown || !s.floorName.empty();	// none: the host keeps the diamond up with both arrows off
		floor_->text = target ? "Floor " + std::to_string(s.floor) : "Floor";
		const std::string building = s.floorName.empty() ? "" : s.floorName + "\n";
		floor_->firstTip = building + "Up a floor";
		floor_->secondTip = building + "Down a floor";
		floor_->firstEnabled = s.floorUp;
		floor_->secondEnabled = s.floorDown;
		floor_->active = moveSpeed_->active = s.driving;
		moveSpeed_->tooltip = kMoveTips[std::clamp(s.moveSpeed, 0, 3)];

		// Squad tabs (the host's: squads with members): rebuilt when the squads or their names change. As Kenshi_SquadTabControl: each
		// min(header / count, 200) wide, so many squads shrink the tabs and clip their captions; no arrows (kenshi-inspect squads.md (g)).
		std::vector<uint64_t> ids;
		std::vector<std::string> names;
		for (const HudSquadTab& t : s.squadTabs)
		{
			ids.push_back(t.id);
			names.push_back(t.name);
		}
		if (ids != tabIds_ || names != tabNames_)
		{
			tabIds_ = ids;
			tabNames_ = names;
			tabRow_->clear();
			tabs_.clear();
			for (const HudSquadTab& t : s.squadTabs)
			{
				std::string caps = t.name;
				for (char& c : caps) { c = (char)toupper((unsigned char)c); }
				Button& b = tabRow_->add<Button>(caps, Look::kButton, "Kenshi_StandardFont_Medium");
				b.rect = { 0, 0, std::min(kTabsW / s.squadTabs.size() - tabRow_->spacing, 200.0f), 34 };
				const uint64_t id = t.id;
				b.onClicked = [this, id] { if (act_.setActive) { act_.setActive(id); } };
				tabs_.push_back(&b);
			}
		}
		for (size_t i = 0; i < tabs_.size(); i++) { tabs_[i]->checked = tabIds_[i] == s.activeSquad; }
		tabRow_->visible = !tabs_.empty();

		// Portraits: rebuilt when the squad's ids change, else updated in place.
		bool same = portraits_.size() == s.squad.size();
		for (size_t i = 0; same && i < s.squad.size(); i++) { same = portraits_[i]->id == s.squad[i].id; }
		if (!same)
		{
			for (Portrait* p : portraits_) { delete p; }
			portraits_.clear();
			squad_->clear();
			for (const HudMember& m : s.squad)
			{
				auto* p = new Portrait;
				p->id = m.id;
				Box& col = squad_->add<Box>(true, 2);
				p->frame = &col.add<Button>("", Look::kPortrait, "Kenshi_PaintedTextFont_Medium");
				p->frame->rect = { 0, 0, 72, 72 };
				p->face = &p->frame->add<PortraitFace>();
				p->face->rect = { 3, 3, 3, 3 };	// inside the frame's border (and the selection outline)
				p->initial = &p->frame->add<Label>("", "Kenshi_PaintedTextFont_Medium");
				p->initial->halign = p->initial->valign = Widget::Align::kCenter;
				const uint64_t id = m.id;
				p->frame->onClicked = [this, id] { if (act_.select) { act_.select(id, (mods() & SAPP_MODIFIER_SHIFT) != 0); } };
				p->frame->onDoubleClicked = [this, id] { if (act_.focus) { act_.focus(id); } };
				p->bar = &col.add<Bar>();
				p->bar->rect = { 0, 0, 72, 8 };
				p->name = &col.add<Label>("", "Kenshi_StandardFont_MediumSmall");
				p->name->halign = Widget::Align::kCenter;
				p->name->shadow = true;
				portraits_.push_back(p);
			}
		}
		int nSel = 0;
		bool all[kHudToggles];
		std::fill(std::begin(all), std::end(all), true);
		for (size_t i = 0; i < s.squad.size(); i++)
		{
			const HudMember& m = s.squad[i];
			Portrait& p = *portraits_[i];
			const std::string name = m.name.empty() ? "#" + std::to_string(m.id) : m.name;
			p.name->text = name;
			p.face->face->tex = m.portrait;
			std::copy(std::begin(m.portraitUv), std::end(m.portraitUv), p.face->face->uv);
			p.face->set({ m.mark, m.back, m.dim, m.hurt }, m.down);
			p.initial->text = m.portrait ? "" : name.substr(0, 1);	// no portrait yet: the initial, over a back overlay
			p.initial->colour = style().textColour(Look::kPortrait, State::kNormal, false);
			p.frame->checked = m.selected;
			// No tooltip: the original's tip box is for UI items (status, bars, stats), never a character (user 2026-11-20, kenshi-k16.2.408.2).
			// The F6 hints panel names the portrait; a downed member shows by its face layers and its name in BadBright.
			p.frame->enabled = true;
			p.name->colour = m.down ? res::colour("BadBright") : res::colour("Main");
			p.bar->fraction = m.health;
			if (!m.selected) { continue; }
			nSel++;
			for (int t = 0; t < kHudToggles; t++) { all[t] = all[t] && m.toggles[t]; }
		}
		for (int t = 0; t < kHudToggles; t++)
		{
			toggle_[t]->checked = nSel > 0 && all[t];
			toggle_[t]->enabled = s.toggleEnabled[t];
		}
		toggles_->visible = ticks_->visible = nSel > 0;
		medic_->enabled = s.medicEnabled;
		rescue_->enabled = s.rescueEnabled;
		prospect_->enabled = s.prospectEnabled;

		jobs_->visible = s.jobsValid;
		jobs_->setJobs(s.jobs);

		// The bottom-centre column's margins: the left cluster (status and medical panels, the medical one's room kept while it is hidden, so
		// the bar doesn't jump with the selection) and the right one (the diamonds over the shortcuts; NOTES left of them in a playtest run),
		// a panel gap off each.
		bottom_->rect.x = medical_->rect.x + medical_->preferred().w + 8;
		bottom_->rect.w = std::max(shortcuts_->rect.x + shortcuts_->rect.w, diamonds_->rect.x + diamonds_->rect.w) + 8;
		if (notesBtn_ && notesBtn_->parent()->visible) { bottom_->rect.w = std::max(bottom_->rect.w, notesBtn_->parent()->rect.x + notesBtn_->parent()->rect.w + 8); }
		// The tip box beside the medical panel, 398 wide where there is room; narrower (its text wrapping) where the screen leaves less before the
		// right cluster (4:3, 5:4), never under the diamonds (k16.3.41). A long tip (a tag's) grows it upwards.
		hintBox_->rect.x = bottom_->rect.x;
		const float room = top_->screen().w / scale() - bottom_->rect.x - bottom_->rect.w;
		hintBox_->rect.w = top_->screen().w > 0 ? std::clamp(room, 120.0f, 398.0f) : 398;
		hint_->rect.w = hintBox_->rect.w - 24;
		hintBox_->rect.h = std::max(72.0f, std::ceil(hint_->preferred().h) + 20);
		hint_->rect.h = hintBox_->rect.h - 20;
		if (s.spectating)	// no player character: nothing to order
		{
			for (Widget* w : { (Widget*)squad_, (Widget*)tabRow_, (Widget*)toggles_, orders_, money_, build_->parent(), (Widget*)moveSpeed_, (Widget*)shortcut_[0], (Widget*)shortcut_[4] }) { w->visible = false; }
		}
		else
		{
			for (Widget* w : { (Widget*)squad_, orders_, money_, build_->parent(), (Widget*)moveSpeed_, (Widget*)shortcut_[0], (Widget*)shortcut_[4] }) { w->visible = true; }
		}
		if (building_ != s.building.valid)
		{
			building_ = s.building.valid;
			place();
		}
		bldButtonBox_->visible = bldTickBox_->visible = building_;
		extButton_->visible = !building_;
		if (building_)
		{
			updateBuilding(s.building);
			return;
		}
		updateInfo(s.info);

		// Medical panel: the inspect focus, else the first selected member that has medical data. A focus without parts: the empty frame.
		const HudMember* med = s.focusSet ? &s.focus : nullptr;
		for (const HudMember& m : s.squad) { if (!med && !s.spectating && m.selected && !m.parts.empty()) { med = &m; } }
		medical_->visible = med != nullptr;
		status_->visible = false;
		medicalName_.clear();
		if (!med) { return; }
		if (med->parts.empty())
		{
			for (int i = 0; i < kLimbs; i++) { limbs_[i]->row->visible = false; }
			return;
		}
		medicalName_ = med->name.empty() ? "#" + std::to_string(med->id) : med->name;
		const Color main = res::colour("BarText", res::colour("Main"));
		status_->tooltip = med->statusTip;
		showStatus(med->status, med->statusColour);
		// Rows: Blood, the parts (up to 7), Hunger. Numbers floor(x + 0.1) as the original's; an unknown value: "-" over an empty slat.
		const int parts = std::min((int)med->parts.size(), kLimbs - 2);
		for (int i = 0; i < kLimbs; i++)
		{
			Limb& l = *limbs_[i];
			l.row->visible = i <= parts + 1;
			Bar& b = *l.bar;
			b.tint = { 0, 0, 0, 0 };
			b.slat = b.crushed = false;
			b.robot = med->robot;
			b.healed = b.splint = 0;
			l.name->align = TextAlign::kCenter;
			l.name->colour = l.right->colour = main;
			l.name->shadow = l.right->shadow = true;
			l.right->visible = true;
			l.right->text.clear();
			l.value->colour = main;
			if (i == 0)
			{
				// Blood ("Oil" for a robot): its own max; the arrows its bleeding speed, one per 0.05 a second.
				const bool known = med->blood >= 0;
				l.name->text = med->robot ? "Oil" : "Blood";
				b.slat = !known || med->bloodMax <= 0;
				b.fraction = b.hurt = known && med->bloodMax > 0 ? med->blood / med->bloodMax : 0;
				l.value->text = known ? whole(std::floor(med->blood + 0.1f)) : "-";
				l.right->text = std::string(std::min((int)std::ceil(std::fabs(med->bleed) / 0.05f), 30), '<');
				l.right->colour = res::colour("Alert", res::colour("BadBright"));
				b.tooltip = l.name->text + (known ? "\n" + whole(med->blood) + (med->bloodMax > 0 ? " / " + whole(med->bloodMax) : "") : "");
			} else if (i <= parts) {
				const HudPart& p = med->parts[i - 1];
				const float max = p.max > 0 ? p.max : 100, effective = p.health - p.stun;
				b.fraction = effective / max;
				b.hurt = (max - p.stun) / max;
				b.healed = (p.health + p.bandage - p.stun) / max;
				b.splint = p.crushed ? 0 : std::clamp(b.fraction + 0.01f * p.splint, 0.0f, 1.0f);
				b.robot = med->robot || p.robot;
				b.crushed = p.crushed;
				l.name->text = p.name;
				l.value->text = whole(std::floor(effective + 0.1f));
				l.value->colour = effective <= 0 ? res::colour("BadBright") : main;
				b.tooltip = p.name + "\n" + whole(effective) + " / " + whole(max) + (p.stun > 0.5f ? "\nStunned " + whole(p.stun) : "")
					+ (p.bandage > 0.5f ? "\nBandaged " + whole(p.bandage) : "");
			} else if (i == parts + 1) {
				// Hunger, 0..3 in three bands on one slat: red fills 0 -> 1, yellow 1 -> 2, green 2 -> 3. ">" digesting, "<" falling, "<!" below 2.
				const bool known = med->hunger >= 0 && !med->robot;
				const float h = med->hunger;
				l.name->text = "Hunger";
				b.robot = false;
				b.slat = !known;
				b.fraction = known ? h - 2 : 0;
				b.healed = known ? h - 1 : 0;
				b.hurt = known ? h : 0;
				l.value->text = known ? std::to_string((int)(h * 100)) : "-";
				l.right->text = !known ? "" : med->fed ? ">" : h < 2 ? "<!" : med->resting ? "" : "<";
				b.tooltip = known ? "Hunger\n" + std::to_string((int)(h * 100)) + " / 300" : "";
			}
		}
	}

	// The status box over the medical panel, as wide as it and 7 above it; hidden without a status.
	void Hud::showStatus(const std::string& text, const std::string& colour)
	{
		status_->visible = !text.empty();
		statusText_->text = text;
		const Color ink = res::colour("Main");
		statusText_->colour = colour.empty() ? ink : res::colour(colour, ink);
		const Size m = medical_->preferred();
		status_->rect.x = medical_->rect.x;
		status_->rect.w = m.w;
		status_->rect.y = medical_->rect.y + m.h + 7;
	}

	void Hud::updateInfo(const HudInfo& in)
	{
		info_->visible = in.valid;
		if (!in.valid) { return; }
		if (in.blank)
		{
			HudInfo empty;
			empty.valid = true;
			updateInfo(empty);
			return;
		}
		const Color plate = style().textColour(Look::kNamePlate, State::kNormal, false);
		tag_->visible = !in.tag.empty();
		tagRow_->text = in.tag;
		tagRow_->value = in.tagValue;
		const Color field = style().textColour(Look::kField, State::kNormal, false);
		tagRow_->textInk = in.tagColour.empty() ? field : res::colour(in.tagColour, field);
		tagRow_->valueInk = in.tagValueColour.empty() ? tagRow_->textInk : res::colour(in.tagValueColour, tagRow_->textInk);
		tag_->tooltip = tipMarkup(in.tagTip);
		name_->text = in.name;
		faction_->text = in.faction;
		name_->colour = faction_->colour = in.nameColour.empty() ? plate : res::colour(in.nameColour, plate);
		state_->lines = in.state;
		ext_->lines = in.extended;
		stats_->lines = in.stats;
	}

	// The building's panels: rebuilt buttons and ticks when their set changes, the bars over the medical rows, the spare rows as empty slats.
	void Hud::updateBuilding(const HudBuildingInfo& b)
	{
		buildingId_ = b.id;
		HudInfo in;
		in.valid = true;
		in.name = b.name;
		in.faction = b.faction;
		in.nameColour = "HudBuildingName";	// the game's Title for both; the plate's ink in the flat style
		in.state = b.state;
		in.stats = b.stats;
		updateInfo(in);
		const float rowH = 21;
		bldButtonBox_->rect.y = 11 + rowH * b.state.size() + 2;
		bldTickBox_->rect.y = 14 + rowH * b.stats.size() + 2;
		if (b.buttons != bldButtonDefs_)
		{
			bldButtonDefs_ = b.buttons;
			bldButtonBox_->clear();
			bldButtons_.clear();
			for (const HudBuildingButton& d : b.buttons)
			{
				if (d.tag)
				{
					Label& l = bldButtonBox_->add<Label>(d.text, "Kenshi_StandardFont_Medium");
					l.halign = Widget::Align::kCenter;
					l.colour = res::colour(d.colour.empty() ? "GreyedBright" : d.colour, l.colour);
					continue;
				}
				Button& btn = bldButtonBox_->add<Button>(d.text, Look::kButton, "Kenshi_StandardFont_Medium");
				btn.rect.h = 22;	// the capture's 14 px at 720 lines
				btn.halign = Widget::Align::kStretch;
				if (!d.colour.empty()) { btn.textColour = res::colour(d.colour); }
				btn.onClicked = [this, d]
				{
					const uint64_t id = buildingId_;
					const auto send = [this, id, button = d.id] { if (act_.buildingButton) { auto f = act_.buildingButton; f(id, button); } };
					if (d.confirm.empty()) { send(); return; }
					if (!prompt_) { prompt_ = std::make_unique<Prompt>(); }
					prompt_->open({ d.text, d.confirm, { "Yes", "No" }, 0, 1 }, [send](int i) { if (i == 0) { send(); } });
				};
				bldButtons_.push_back(&btn);
			}
		}
		if (b.ticks != bldTickDefs_)
		{
			bldTickDefs_ = b.ticks;
			bldTickBox_->clear();
			bldTicks_.clear();
			for (const HudBuildingTick& d : b.ticks)
			{
				Button& btn = bldTickBox_->add<Button>(d.text, Look::kToggle, "Kenshi_StandardFont_Medium");
				btn.rect.h = 22;
				btn.halign = Widget::Align::kStretch;
				btn.checkable = true;
				btn.checked = d.checked;
				btn.onToggle = [this, &btn, id = d.id](bool on)
				{
					btn.checked = !on;	// the host's state decides
					if (act_.buildingTick) { auto f = act_.buildingTick; f(buildingId_, id, on); }
				};
				bldTicks_.push_back(&btn);
			}
		}
		medical_->visible = true;
		medicalName_.clear();
		status_->tooltip.clear();
		showStatus(b.status, b.statusColour);
		const Color main = res::colour("Main"), barInk = res::colour("HudBarText");
		for (int i = 0; i < kLimbs; i++)
		{
			Limb& l = *limbs_[i];
			const HudBuildingBar* bar = i < (int)b.bars.size() ? &b.bars[i] : nullptr;
			l.row->visible = true;
			l.bar->tint = res::colour(bar && !bar->colour.empty() ? bar->colour : "BarLayerGreen");
			l.bar->slat = bar == nullptr;
			l.bar->fraction = bar ? bar->fraction : 0;
			l.bar->tooltip.clear();
			l.name->text = bar ? bar->label : "";
			l.name->align = TextAlign::kCenter;
			l.right->visible = true;
			l.right->text = bar ? bar->right : "";
			l.value->text = bar ? bar->value : "";
			l.name->colour = l.right->colour = barInk;	// dark ink on the light track, no shadow (the capture)
			l.name->shadow = l.right->shadow = false;
			l.value->colour = main;
		}
	}

	void Hud::place()
	{
		slider_->rect.y = 4 + kSlideH * (building_ ? 0 : slide_) - (kStatsH - 2);	// collapsed, the extended panel ends 4 above the stats panel's foot
	}
}
