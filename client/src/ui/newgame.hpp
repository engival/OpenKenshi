#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The new game panel (docs/UI.md; our layout, not the original's arrows: kenshi-k16.17.4.1): a 900 x 550 window, centred, 54 from the top.
// CHOOSE YOUR BEGINNING across the top; on the left a side pick list (the options panel's Tabs, kLeft) of the starts' titles in the host's
// order; on the right the chosen start's title over its description box; ADVANCED OPTIONS and BEGIN under them, CANCEL bottom right. Up / Down
// move the choice (stopping at the ends), Enter begins, Escape cancels. The host owns the starts (its NEW_GAME_STARTOFF records: title, squad,
// money, the description), their order and starting a world; the panel only picks one.
namespace ui
{
	struct StartEntry
	{
		std::string id;	// the host's, e.g. the record's string id
		std::string title;
		std::string info;	// the description box: rich text (layoutRich; "label\tvalue" rows, untabbed lines wrap)
	};

	struct NewGameActions
	{
		std::function<void(const std::string& id)> begin;	// the panel has closed
		std::function<void()> advanced;	// ADVANCED OPTIONS toggles the host's AdvancedOptions (openBelow(*window()) / show(false), and hidden on `hidden`); null: greyed
		std::function<void()> cancel;	// CANCEL or Escape (the panel has closed)
	};

	class NewGamePanel
	{
	public:
		explicit NewGamePanel(NewGameActions actions = {});
		~NewGamePanel();
		// Shown in front with the keys, on start `index` (clamped). hidden: called once when it hides, for whatever reason, before the action.
		void open(std::function<void()> hidden = {}, int index = 0);
		void close();	// without an action
		bool shown() const;
		void setStarts(std::vector<StartEntry> starts);	// any time; the choice stays on its id, else the first
		const std::vector<StartEntry>& starts() const { return starts_; }
		int current() const { return cur_; }	// -1: none
		const std::string& currentId() const;
		void pick(int i);	// clamped to the list
		// For scripted tests.
		Widget* window() const;
		Tabs* list() const { return list_; }
		Button* beginButton() const { return begin_; }
		Button* advancedButton() const { return advanced_; }
		Button* cancelButton() const { return cancel_; }
		Label* title() const { return title_; }
		RichText* info() const { return info_; }

	private:
		Window* win_ = nullptr;
		Tabs* list_ = nullptr;
		Button *begin_ = nullptr, *advanced_ = nullptr, *cancel_ = nullptr;
		Label* title_ = nullptr;
		ScrollView* scroll_ = nullptr;
		RichText* info_ = nullptr;
		std::vector<StartEntry> starts_;
		int cur_ = -1;
		NewGameActions act_;
		std::function<void()> hidden_;
		Widget* before_ = nullptr;	// the screen (a child of root()) that had the keys when it opened: it gets them back
		void rebuild();	// the list's rows from starts_
		void refresh();
		void confirm();
		void hide();
	};
}
