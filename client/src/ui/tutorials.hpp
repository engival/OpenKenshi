#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The tutorial popups (docs/UI.md), after the original's Kenshi_TutorialPanel, Kenshi_TutorialTooltipPanel and Kenshi_TutorialsWindow (all three
// live in kinspect `findings/ui_captures/darkui_tutorials_0927.jpg`):
//  - the tutorial window, top left (500 x 350): the title as its caption, the page's text, DISMISS, "n / N", PREVIOUS and CONTINUE (SKIP on a page
//    that waits for the player to do something; FINISH on the last, where DISMISS hides). FINISH completes it; DISMISS drops it; the close button
//    only hides it.
//  - the hint rows at the left edge under it (250 x 25 each, stacked upward, newest at the bottom, no cap): its title as a button (reopens it on
//    the page it was left on) and a red X (completes it). kinspect `findings/tut17_static.md`.
//  - the TUTORIALS window, centred (1000 x 445): every tutorial in a list, the selected one's pages, << / >> and ACTIVATE.
// Host-fed: the original's tutorials are hard-coded in its exe (no TUTORIAL records in the data), so the host supplies the texts (our own data or a
// mod) and decides when one fires (`hint`, `open`) and when a waiting page is done (`open(id, page + 1)`). The UI never tests a condition.
namespace ui
{
	struct TutorialPage
	{
		std::string text;	// rich text (layoutRich); untabbed lines wrap
		bool waits = false;	// the player has to do something: the button reads SKIP
	};

	struct Tutorial
	{
		std::string id, title;
		std::vector<TutorialPage> pages;
	};

	struct TutorialActions
	{
		std::function<void(const std::string& id)> done;	// FINISH on the last page or a hint's X: completed for good (the hint is gone)
		std::function<void(const std::string& id)> dismiss;	// DISMISS (the window has closed, the hint is gone)
		std::function<void(const std::string& id)> activate;	// the TUTORIALS window's ACTIVATE: the host usually hints and opens it
		std::function<void(const std::string& id, int page)> page;	// the tutorial window turned to a page (PREVIOUS / CONTINUE / SKIP / keys)
	};

	class Tutorials
	{
	public:
		explicit Tutorials(TutorialActions actions = {});
		~Tutorials();
		void setAll(std::vector<Tutorial> all);	// every tutorial the host knows; hints and the open one keep their ids
		const std::vector<Tutorial>& all() const { return all_; }
		void hint(const std::string& id);	// adds its row (newest, at the bottom), once; unknown ids are ignored
		void unhint(const std::string& id);
		const std::vector<std::string>& hints() const { return hints_; }
		void open(const std::string& id, int page);	// the tutorial window, with the keys; the page is clamped
		void open(const std::string& id);	// on the page it was left on (0 the first time)
		void close();	// the tutorial window, without an action
		bool shown() const;
		const std::string& openId() const { return open_; }
		int page() const { return page_; }
		void showBrowser(bool on);
		bool browserShown() const;
		// For scripted tests.
		Widget* window() const;
		Widget* browser() const;
		Button* dismissButton() const { return dismiss_; }
		Button* prevButton() const { return prev_; }
		Button* nextButton() const { return next_; }	// CONTINUE / SKIP / FINISH
		Label* pageLabel() const { return paging_; }
		RichText* text() const { return text_; }
		Button* hintButton(int i) const { return i >= 0 && i < (int)hintName_.size() ? hintName_[i] : nullptr; }	// hints() order: oldest first
		Button* hintClose(int i) const { return i >= 0 && i < (int)hintX_.size() ? hintX_[i] : nullptr; }
		List* list() const { return list_; }
		Button* activateButton() const { return activate_; }
		Button* browsePrev() const { return bPrev_; }
		Button* browseNext() const { return bNext_; }
		RichText* browseText() const { return bText_; }

	private:
		CaptionWindow *win_ = nullptr, *browser_ = nullptr;
		RichText *text_ = nullptr, *bText_ = nullptr;
		ScrollView *scroll_ = nullptr, *bScroll_ = nullptr;
		Label *paging_ = nullptr, *bPaging_ = nullptr;
		Button *dismiss_ = nullptr, *prev_ = nullptr, *next_ = nullptr;
		Button *activate_ = nullptr, *bPrev_ = nullptr, *bNext_ = nullptr;
		List* list_ = nullptr;
		Widget* hintBox_ = nullptr;
		std::vector<Button*> hintName_, hintX_;	// a pool: grows, never shrinks (rows hide)
		std::map<std::string, int> left_;	// the page each tutorial was left on
		std::vector<Tutorial> all_;
		std::vector<std::string> hints_;
		std::string open_;
		int page_ = 0, bPage_ = 0;
		TutorialActions act_;
		const Tutorial* find(const std::string& id) const;
		void turn(int d);	// the tutorial window's page: past the last finishes it
		void browseTurn(int d);
		void finish(bool done);	// done: FINISH; else DISMISS
		void refresh();
		void refreshHints();
		void refreshBrowser();
	};
}
