#include "ui/menus.hpp"

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

		// The offline line: under EXIT, from the column's left edge, wrapped (a long server error takes a few lines; 1080 - 955 leaves room
		// for five).
		offline_ = &art_->add<Label>("", "Kenshi_StandardFont_Medium");
		offline_->halign = A::kCenter;
		offline_->valign = A::kStart;
		offline_->rect = { 500 + 260 - 960, kMainTops[6] + 69 + 16, 520, 0 };
		offline_->wrap = true;
		offline_->shadow = true;
		offline_->colour = Color{ 1, 1, 1, 0.9f };	// over the light sand of the art: as the credits text, a little dimmer
		offline_->visible = false;
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

	void MainMenu::setOffline(const std::string& hint)
	{
		if (hint == offline_->text) { return; }
		offline_->text = hint;
		offline_->visible = !hint.empty();
		applyButtons();
	}

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
