#include "ui/menus.hpp"

#include <cmath>

#include "ui/progressbars.hpp"
#include "ui/prompt.hpp"
#include "ui/savepanel.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const char* const kMainCaptions[7] = { "CONTINUE", "NEW GAME", "LOAD GAME", "IMPORT GAME", "OPTIONS", "CREDITS", "EXIT" };
		// The buttons' tops at 1080 lines (the layout's 0.138889 ... 0.805556); all 300 x 69 (0.15625 x 0.0638889) with their left at 0.260417.
		const float kMainTops[7] = { 150, 270, 389, 509, 629, 749, 870 };
		const char* const kGameCaptions[6] = { "NEW GAME", "SAVE GAME", "LOAD GAME", "OPTIONS", "EXIT GAME", "RESUME" };
		const float kGameTops[6] = { 15, 65, 115, 165, 215, 285 };	// in the 200 x 344 panel; 150 x 45 buttons, 25 in
	}

	// ---- MainMenu ----

	// The title art filling the screen, cut to keep its aspect; dark without art.
	class MainMenu::Art : public Widget
	{
	public:
		PortraitRef art;

		Art()
		{
			solid = true;
			halign = valign = Align::kStretch;
		}

	protected:
		void paint() const override
		{
			gfx::fill(screen_, Color{ 0.05f, 0.05f, 0.06f, 1 });
			if (art.tex == 0 || screen_.w <= 0 || screen_.h <= 0) { return; }
			const ImageSet* set = res::images("Kenshi_TitleScreen");
			const float aspect = set && set->h > 0 ? set->w / set->h : 16.0f / 9, sa = screen_.w / screen_.h;
			float fu = 1, fv = 1;	// the fraction of the art shown
			if (sa > aspect) { fv = aspect / sa; }
			else { fu = sa / aspect; }
			const float* uv = art.uv;
			const float du = uv[2] - uv[0], dv = uv[3] - uv[1];
			const float u0 = uv[0] + du * (1 - fu) / 2, v0 = uv[1] + dv * (1 - fv) / 2;
			gfx::textured(art.tex, screen_, u0, v0, u0 + du * fu, v0 + dv * fv, {});
		}
	};

	// The status panel's step bar (kenshi-k16.2.431.1): flat, a thin track and fill; original, the skinned Kenshi_ProgressBarFill.
	class MainMenu::StepBar : public Widget
	{
	public:
		float progress = 0;
		Size preferred() const override { return { rect.w, style().natural(Look::kSlotBar).h > 12 ? 20.0f : 8.0f }; }

	protected:
		void paint() const override
		{
			if (style().natural(Look::kSlotBar).h > 12)
			{
				drawProgressBar(screen_, progress, scale_);
				return;
			}
			const Rect fill{ screen_.x, screen_.y, std::round(screen_.w * std::clamp(progress, 0.0f, 1.0f)), screen_.h };
			style().draw(Look::kProgressTrack, screen_, scale_, State::kNormal, false);
			if (fill.w > 0) { style().draw(Look::kProgressFill, fill, scale_, State::kNormal, false); }
		}
	};

	// The working cue: three dots, one lit in turn (a step a third of a second), so a long stage never looks frozen.
	class MainMenu::Cue : public Widget
	{
	public:
		Size preferred() const override { return { 3 * kDot + 2 * kGap, kDot }; }

	protected:
		void paint() const override
		{
			const Color lit = style().colour("Accent", Color{ 0.85f, 0.64f, 0.26f, 1 });
			const Color dim{ lit.r, lit.g, lit.b, 0.3f };
			const int on = (int)std::fmod(now() * 3, 3.0);
			const float d = std::round(kDot * scale_), y = std::round(screen_.y + (screen_.h - d) / 2);
			for (int i = 0; i < 3; i++) { gfx::fill({ std::round(screen_.x + i * (kDot + kGap) * scale_), y, d, d }, i == on ? lit : dim); }
		}

	private:
		static constexpr float kDot = 8, kGap = 6;
	};

	MainMenu::MainMenu(MainMenuActions actions, SavePanel* saves) : act_(std::move(actions)), saves_(saves)
	{
		using A = Widget::Align;
		screen_ = &root().add<Widget>();
		screen_->halign = screen_->valign = A::kStretch;
		screen_->visible = false;
		art_ = &screen_->add<Art>();

		// Offsets from the screen's centre, so the column keeps its place on the art at any aspect.
		for (int i = 0; i < 7; i++)
		{
			Button& b = art_->add<Button>(kMainCaptions[i], Look::kButton, "Kenshi_PaintedTextFont_Large");
			b.halign = b.valign = A::kCenter;
			b.rect = { 500 + 150 - 960, kMainTops[i] + 34.5f - 540, 300, 69 };
			b.hoverSound = true;	// the only buttons the original hover-clicks (kenshi-k16.3.20)
			buttons_[i] = &b;
		}
		const auto call = [](const std::function<void()>& f) { if (f) { f(); } };
		buttons_[0]->onClicked = [this, call] { call(act_.continueGame); };
		buttons_[1]->onClicked = [this, call] { call(act_.newGame); };
		buttons_[2]->onClicked = [this] { if (saves_) { saves_->open(SaveMode::kLoad); } };
		buttons_[3]->onClicked = [this, call] { call(act_.import); };
		buttons_[4]->onClicked = [this, call] { call(act_.options); };
		buttons_[5]->onClicked = [this] { showCredits(!creditsShown()); };
		buttons_[6]->onClicked = [this, call] { call(act_.exit); };
		// SCENARIOS (the playtest list; client coordinator 2026-11-07): beside OPTIONS, smaller, only with --debug. Dev builds only (kenshi-k16.2.153.2).
#ifdef KCLIENT_DEV_TOOLS
		scenarios_ = &art_->add<Button>("SCENARIOS", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		scenarios_->halign = scenarios_->valign = A::kCenter;
		scenarios_->rect = { 500 + 300 + 20 + 100 - 960, kMainTops[4] + 34.5f - 540, 200, 50 };
		scenarios_->textColour = res::colour("Main");
		scenarios_->visible = false;
		scenarios_->onClicked = [this, call] { call(act_.scenarios); };
#endif

		// CreditsPanel: 0.520312 0.37963 0.364062 0.564815 of the screen.
		credits_ = &art_->add<Panel>(Look::kInset, 10);
		credits_->halign = credits_->valign = A::kCenter;
		credits_->rect = { 999 + 349.5f - 960, 410 + 305 - 540, 699, 610 };
		credits_->visible = false;
		ScrollView& scroll = credits_->add<ScrollView>();
		scroll.halign = scroll.valign = A::kStretch;
		creditsText_ = &scroll.body().add<Label>("", "Kenshi_StandardFont_Medium");
		creditsText_->wrap = true;
		creditsText_->align = TextAlign::kCenter;
		creditsText_->rect.w = 699 - 2 * 10 - 8;
		creditsText_->colour = Color{ 1, 1, 1, 1 };

		// The footer (kenshi-379.11.2): the public name, the host's version and the not-affiliated line, where the layout has VersionText
		// (bottom right, clear of the column at any aspect down to 4:3).
		version_ = &art_->add<Label>("", "Kenshi_StandardFont_Medium");
		version_->halign = version_->valign = A::kEnd;
		version_->rect = { 16, 8, 0, 0 };
		version_->shadow = true;
		setVersion({});

		// The status panel (kenshi-k16.2.431.1, user 2026-11-20: "The main menu needs better feedback if the server isn't ready"): right of the
		// column, from CONTINUE's top, clear of the credits box (from 410) for a few lines of failure text. A heading with the working cue, the
		// host's text wrapped, the step bar and "2 / 4", the held click's line.
		const float kStatusW = 540, kStatusPad = 16, kTextW = kStatusW - 2 * kStatusPad;
		status_ = &art_->add<Panel>(Look::kPanel, kStatusPad);
		status_->halign = A::kCenter;	// as the column: its centre's offset from the screen's, so it keeps its place at any aspect
		status_->valign = A::kStart;	// its top at CONTINUE's, growing down
		status_->rect = { 500 + 300 + 30 + kStatusW / 2 - 960, kMainTops[0], kStatusW, 0 };
		status_->visible = false;
		Box& col = status_->add<Box>(true, 10);
		Box& head = col.add<Box>(false, 14);
		heading_ = &head.add<Label>("", "Kenshi_PaintedTextFont_Medium");
		heading_->valign = A::kCenter;
		cue_ = &head.add<Cue>();
		cue_->valign = A::kCenter;
		offline_ = &col.add<Label>("", "Kenshi_StandardFont_Large");
		offline_->wrap = true;
		offline_->rect.w = kTextW;
		Box& row = col.add<Box>(false, 12);
		stepRow_ = &row;
		bar_ = &row.add<StepBar>();
		bar_->rect.w = kTextW - 70;
		bar_->valign = A::kCenter;
		stepsText_ = &row.add<Label>("", "Kenshi_StandardFont_Medium");
		stepsText_->valign = A::kCenter;
		pendingText_ = &col.add<Label>("", "Kenshi_StandardFont_Medium");
		pendingText_->wrap = true;
		pendingText_->rect.w = kTextW;
		pendingText_->visible = false;
		applyButtons();	// LOAD GAME greyed without a save panel
	}

	MainMenu::~MainMenu()
	{
		if (ready() && screen_) { root().remove(screen_); }
	}

	bool MainMenu::shown() const { return screen_->visible; }

	void MainMenu::show(bool on)
	{
		screen_->visible = on;
		if (scenarios_) { scenarios_->visible = act_.scenarios != nullptr; }	// the host sets it with --debug or --playtest (kenshi-k16.2.110.1)
		if (on)
		{
			screen_->raise();	// over whatever the host left up; the windows it opens (options, saves) raise themselves over it
			clearMessages();	// the overlay draws over every screen: the game's toasts and zone banner would show through the title
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	void MainMenu::setBackground(const PortraitRef& art) { art_->art = art; }

	void MainMenu::setContinue(bool enabled, const std::string& tip)
	{
		continueOn_ = enabled;
		continueTip_ = tip;
		applyButtons();
	}

	void MainMenu::setOffline(const std::string& text, int step, int steps) { applyStatus(text, step, steps, false); }
	void MainMenu::setError(const std::string& text) { applyStatus(text, 0, 0, !text.empty()); }

	void MainMenu::applyStatus(const std::string& text, int step, int steps, bool error)
	{
		steps = std::max(steps, 0);
		step = std::clamp(step, 0, steps);
		if (text == offline_->text && step == step_ && steps == steps_ && error == error_) { return; }
		const bool tips = text != offline_->text;
		offline_->text = text;
		step_ = step;
		steps_ = steps;
		error_ = error;
		status_->visible = !text.empty();
		const Color main = style().colour("Main", Color{ 1, 1, 1, 1 }), bad = style().colour("BadBright", Color{ 0.93f, 0.46f, 0.37f, 1 });
		heading_->text = error ? "GAME SERVER ERROR" : "GAME SERVER STARTING";
		heading_->colour = error ? bad : style().colour("Accent", main);	// "Title" is a dark red in the original style, unreadable on its panel
		offline_->colour = error ? bad : main;
		cue_->visible = !error;
		stepRow_->visible = !error && steps > 0;
		bar_->progress = steps > 0 ? (float)step / steps : 0;
		stepsText_->text = std::to_string(step) + " / " + std::to_string(steps);
		stepsText_->colour = style().colour("Secondary", main);
		applyPendingLine();
		if (tips) { applyButtons(); }
	}

	// While the host holds a click and the server is on its way: which button will run.
	void MainMenu::applyPendingLine()
	{
		pendingText_->visible = pending_ >= 0 && !error_ && !offline_->text.empty();
		pendingText_->text = pending_ >= 0 ? std::string(kMainCaptions[pending_]) + " will start when the server is ready" : std::string();
		pendingText_->colour = style().colour("Accent", Color{ 0.85f, 0.64f, 0.26f, 1 });
	}

	float MainMenu::statusFill() const { return bar_->progress; }
	Widget* MainMenu::statusBar() const { return bar_; }
	Widget* MainMenu::statusCue() const { return cue_; }

	// CONTINUE from setContinue, LOAD GAME from the save panel; offline greys nothing (the host holds the click), the hint is their tip.
	void MainMenu::applyButtons()
	{
		const std::string& hint = offline_->text;
		buttons_[0]->enabled = continueOn_;
		buttons_[0]->tooltip = continueOn_ && !hint.empty() ? hint : continueTip_;
		buttons_[1]->enabled = true;
		buttons_[2]->enabled = saves_ != nullptr;
		buttons_[3]->enabled = true;
		for (int i = 1; i < 4; i++) { buttons_[i]->tooltip = hint; }
	}

	void MainMenu::setPending(int button)
	{
		pending_ = button >= 0 && button < 4 ? button : -1;
		for (int i = 0; i < 4; i++) { buttons_[i]->held = i == pending_; }
		applyPendingLine();
	}

	void MainMenu::setCredits(const std::string& text) { creditsText_->text = text; }
	void MainMenu::setVersion(const std::string& version)
	{
		version_->text = "OpenKenshi " + (version.empty() ? std::string("dev") : version)
			+ " - not affiliated with or endorsed by Lo-Fi Games. Requires a purchased copy of Kenshi (Steam or GOG).";
	}
	void MainMenu::showCredits(bool on) { credits_->visible = on; }
	bool MainMenu::creditsShown() const { return credits_->visible; }
	Widget* MainMenu::screen() const { return screen_; }
	Button* MainMenu::button(int i) const { return i >= 0 && i < 7 ? buttons_[i] : nullptr; }
	Widget* MainMenu::creditsBox() const { return credits_; }

	// ---- GameMenu ----

	// The dimmed game under the menu (the layout's WhiteSkin root): takes the mouse; a click gives the keys back to the menu.
	class GameMenu::Backdrop : public Widget
	{
	public:
		Widget* owner = nullptr;

		Backdrop()
		{
			solid = true;
			halign = valign = Align::kStretch;
		}

		void onClick(Btn) override { if (owner && owner->visible) { focus(owner); } }

	protected:
		void paint() const override { gfx::fill(screen_, Color{ 0, 0, 0, 0.35f }); }
	};

	GameMenu::GameMenu(GameMenuActions actions, SavePanel* saves) : act_(std::move(actions)), saves_(saves)
	{
		using A = Widget::Align;
		backdrop_ = &root().add<Backdrop>();
		backdrop_->visible = false;
		win_ = &root().add<Window>(Look::kPanel, 0);	// MainMenuPopupPanel: 0.104167 x 0.318519, centred
		backdrop_->owner = win_;
		win_->halign = win_->valign = A::kCenter;
		win_->rect = { 0, 0, 200, 344 };
		win_->visible = false;
		win_->onEscape = [this] { show(false); };
		for (int i = 0; i < 6; i++)
		{
			Button& b = win_->add<Button>(kGameCaptions[i], Look::kButton, "Kenshi_PaintedTextFont_Medium");
			b.rect = { 25, kGameTops[i], 150, 45 };
			buttons_[i] = &b;
		}
		buttons_[0]->onClicked = [this]
		{
			ask("NEW GAME", "Start a new game? Unsaved progress will be lost.", act_.newGame);
		};
		// The panel takes the menu's place (the two would overlap); the menu comes back when it closes, unless the menu was closed meanwhile.
		const auto openSaves = [this](SaveMode m)
		{
			if (!saves_) { return; }
			win_->visible = false;
			saves_->open(m, {}, [this]
			{
				if (!backdrop_->visible) { return; }
				win_->visible = true;
				win_->raise();
				focus(win_);
			});
		};
		buttons_[1]->onClicked = [openSaves] { openSaves(SaveMode::kSave); };
		buttons_[2]->onClicked = [openSaves] { openSaves(SaveMode::kLoad); };
		buttons_[1]->enabled = buttons_[2]->enabled = saves_ != nullptr;
		buttons_[3]->onClicked = [this] { if (act_.options) { act_.options(); } };
		buttons_[4]->onClicked = [this]
		{
			ask("EXIT GAME", "Exit the game? Unsaved progress will be lost.", act_.exitGame);	// it quits the program (kenshi-k16.2.212)
		};
		buttons_[5]->onClicked = [this] { show(false); };
#ifdef KCLIENT_DEV_TOOLS
		scenarios_ = &win_->add<Button>("SCENARIOS", Look::kButton, "Kenshi_PaintedTextFont_Medium");	// under RESUME, when the host sets the action; dev builds only
		scenarios_->rect = { 25, kGameTops[5] + 50, 150, 45 };
		scenarios_->visible = false;
		scenarios_->onClicked = [this] { if (act_.scenarios) { act_.scenarios(); } };
#endif
		prompt_ = std::make_unique<Prompt>();
	}

	GameMenu::~GameMenu()
	{
		prompt_.reset();
		if (!ready()) { return; }
		if (win_) { root().remove(win_); }
		if (backdrop_) { root().remove(backdrop_); }
	}

	bool GameMenu::shown() const { return backdrop_->visible; }	// the window steps aside while the save panel is up
	Widget* GameMenu::window() const { return win_; }
	Button* GameMenu::button(int i) const { return i >= 0 && i < 6 ? buttons_[i] : nullptr; }

	void GameMenu::show(bool on)
	{
		if (on == shown()) { return; }
		if (scenarios_) { scenarios_->visible = act_.scenarios != nullptr; }	// the host sets it with --debug or --playtest (kenshi-k16.2.110.1)
		win_->rect.h = scenarios_ && scenarios_->visible ? 394 : 344;	// the panel grows for it
		backdrop_->visible = win_->visible = on;
		if (on)
		{
			backdrop_->raise();
			win_->raise();
			focus(win_);
		}
		else
		{
			prompt_->close();
			if (saves_ && saves_->shown() && !win_->visible) { saves_->close(); }	// the panel it opened
			if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
		}
		if (act_.shown) { auto f = act_.shown; f(on); }
	}

	void GameMenu::ask(const std::string& title, const std::string& text, const std::function<void()>& yes)
	{
		prompt_->open({ title, text, { "Yes", "No" }, 1 }, [this, yes](int i)
		{
			if (i != 0) { return; }
			show(false);
			if (yes) { yes(); }
		});
	}
}
