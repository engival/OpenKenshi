#pragma once
#include <functional>
#include <memory>
#include <string>

#include "ui/widget.hpp"

// The title screen and the in-game menu (docs/UI.md).
//
// MainMenu, after Kenshi_MainMenu.layout: the title art over the whole screen (the host's texture, cropped to fill), CONTINUE / NEW GAME / LOAD GAME /
// IMPORT GAME / OPTIONS / CREDITS / EXIT down the left third, the credits box (the host's text, ours: contributors, libraries and their licences, the
// reverse-engineering references) toggled by CREDITS, the footer (name, version, not-affiliated line) bottom right. A root screen: the host shows it instead of the HUD.
//
// GameMenu, after Kenshi_MainMenuPopupPanel.layout: Escape in the game. A small panel over a dimmed screen with NEW GAME / SAVE GAME / LOAD GAME /
// OPTIONS / EXIT GAME and RESUME. NEW GAME and EXIT GAME ask first (unsaved progress). It never pauses anything itself: the host decides through
// the actions.
//
// LOAD GAME / SAVE GAME open the host's SavePanel (savepanel.hpp) in front: the title screen stays under it; the game menu's panel steps aside
// (the dimming stays, shown() stays true) and comes back when the save panel closes.
namespace ui
{
	class Prompt;
	class SavePanel;

	struct MainMenuActions
	{
		std::function<void()> continueGame, newGame, import, options, exit;
		std::function<void()> scenarios;	// the SCENARIOS button (beside OPTIONS), shown when this is set (the host sets it with --debug or --playtest)
	};

	class MainMenu
	{
	public:
		explicit MainMenu(MainMenuActions actions = {}, SavePanel* saves = nullptr);	// saves null: LOAD GAME greyed
		~MainMenu();
		bool shown() const;
		void show(bool on);
		void setBackground(const PortraitRef& art);	// title_screen1.dds (Kenshi_TitleScreen / TitleScreen1 / Background); none: plain dark
		void setContinue(bool enabled, const std::string& tip = {});	// no save to continue: greyed, the tip says why
		// No game to talk to (the host's server starting, failed or stopped; kenshi-k16.3.12, k16.2.431.1): the text in the status panel beside the
		// column (a heading, the text, a step bar when steps > 0 at step / steps, a live working cue) and as the tooltip of CONTINUE / NEW GAME /
		// LOAD GAME / IMPORT GAME. They stay clickable (kenshi-k16.2.210.2): the click reaches the host, which may hold it (setPending) and act
		// once the game is up. Empty: linked, the panel gone, the tips back. The wording is the host's (e.g. "Loading the world (8 s)", 2, 4).
		// Cheap when unchanged (callable every frame).
		void setOffline(const std::string& text, int step = 0, int steps = 0);
		// As setOffline(text), drawn as a failure: the heading and text in the warning colour, no bar, no working cue, no pending line.
		void setError(const std::string& text);
		bool offlineError() const { return error_; }
		// The click the host holds: 0 CONTINUE, 1 NEW GAME, 2 LOAD GAME, 3 IMPORT GAME drawn pushed until setPending(-1). One at a time.
		void setPending(int button);
		int pending() const { return pending_; }
		void setCredits(const std::string& text);	// '\n' separated, wrapped, centred
		// The footer bottom right: "OpenKenshi <version> - not affiliated with or endorsed by Lo-Fi Games. Requires a purchased copy of Kenshi (Steam or GOG)."
		// The version only (a tag or the git hash); empty or never set: "dev".
		void setVersion(const std::string& version);
		void showCredits(bool on);
		bool creditsShown() const;
		// For scripted tests.
		Widget* screen() const;
		Button* button(int i) const;	// 0 CONTINUE ... 6 EXIT, the layout's order
		Widget* creditsBox() const;
		Label* version() const { return version_; }
		Button* scenariosButton() const { return scenarios_; }
		Label* offlineLine() const { return offline_; }	// the panel's text
		Widget* statusPanel() const { return status_; }
		Label* statusHeading() const { return heading_; }
		Widget* statusBar() const;	// shown when steps > 0
		Label* statusSteps() const { return stepsText_; }	// "2 / 4"
		Widget* statusCue() const;	// the working cue; hidden on an error
		Label* pendingLine() const { return pendingText_; }	// "NEW GAME will start when the server is ready"
		float statusFill() const;	// the bar's fill, 0..1

	private:
		class Art;
		class StepBar;
		class Cue;
		Widget* screen_ = nullptr;
		Art* art_ = nullptr;
		Button* buttons_[7] = {};
		Button* scenarios_ = nullptr;
		Panel* credits_ = nullptr;
		Label* creditsText_ = nullptr;
		Label* version_ = nullptr;
		Label* offline_ = nullptr;
		Panel* status_ = nullptr;
		Label* heading_ = nullptr;
		Label* stepsText_ = nullptr;
		Label* pendingText_ = nullptr;
		StepBar* bar_ = nullptr;
		Cue* cue_ = nullptr;
		Widget* stepRow_ = nullptr;
		bool continueOn_ = true, error_ = false;
		int pending_ = -1, step_ = 0, steps_ = 0;
		std::string continueTip_;
		MainMenuActions act_;
		SavePanel* saves_;
		void applyButtons();
		void applyStatus(const std::string& text, int step, int steps, bool error);
		void applyPendingLine();
	};

	struct GameMenuActions
	{
		std::function<void()> newGame, options, exitGame;	// new game / exit game: after the player confirmed
		std::function<void(bool)> shown;	// the menu opened / closed (RESUME, Escape, or show(false)): the host's pause policy
		std::function<void()> scenarios;	// the SCENARIOS button (under RESUME), shown when this is set (the host sets it with --debug or --playtest)
	};

	class GameMenu
	{
	public:
		explicit GameMenu(GameMenuActions actions = {}, SavePanel* saves = nullptr);	// saves null: SAVE / LOAD greyed
		~GameMenu();
		bool shown() const;
		void show(bool on);
		void toggle() { show(!shown()); }
		// For scripted tests.
		Widget* window() const;
		Button* button(int i) const;	// 0 NEW GAME ... 5 RESUME, the layout's order
		Prompt& prompt() const { return *prompt_; }
		Button* scenariosButton() const { return scenarios_; }

	private:
		class Backdrop;
		Backdrop* backdrop_ = nullptr;
		Window* win_ = nullptr;
		Button* buttons_[6] = {};
		Button* scenarios_ = nullptr;
		std::unique_ptr<Prompt> prompt_;
		GameMenuActions act_;
		SavePanel* saves_;
		void ask(const std::string& title, const std::string& text, const std::function<void()>& yes);
	};
}
