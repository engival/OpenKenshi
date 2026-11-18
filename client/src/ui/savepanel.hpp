#pragma once
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The load and save panels (docs/UI.md), after the original's Kenshi_LoadGamePanel / Kenshi_SaveGamePanel (Kenshi_FloatingPanelSkin, 0.354 x 0.301 of the
// screen: 680 x 325 at 1080 lines): a NAME | DATE list of the saves on the left, the selected save's details in a box on the right, LOAD or SAVE and
// CANCEL under it. Loading offers the layout's "Reset squad positions" tick; saving names the save in an edit box under the list (a picked row fills
// it). Delete asks before deleting; saving over a save asks first. One class, two modes. The host owns the saves (b0's runtime save format through
// the sim): it fills setSaves and does what the actions ask; the panel never touches the filesystem.
namespace ui
{
	class Prompt;

	struct SaveEntry
	{
		std::string name;	// the save's name and its id
		std::time_t when = 0;	// sorts the DATE column
		std::string date;	// shown: the host's format
		std::string info;	// the details box: rich text (layoutRich; "label\tvalue" rows)
	};

	enum class SaveMode { kLoad, kSave };

	struct SaveActions
	{
		std::function<void(const std::string& name, bool resetPositions)> load;	// the panel has closed
		std::function<void(const std::string& name)> save;	// the panel has closed; an existing name was confirmed
		std::function<void(const std::string& name)> remove;	// confirmed; the host deletes it and calls setSaves
		std::function<void()> close;	// CANCEL or Escape (the panel has closed)
		std::function<void(SaveMode)> opening;	// about to show: the host may setSaves with a fresh list
	};

	class SavePanel
	{
	public:
		explicit SavePanel(SaveActions actions = {});
		~SavePanel();
		// Shown in front with the keys, newest save selected. Saving: the name box holds suggested (else the selected save's name). hidden: called
		// once when it hides, for whatever reason, before the action (a menu that stepped aside comes back).
		void open(SaveMode mode, const std::string& suggested = {}, std::function<void()> hidden = {});
		void close();	// without an action
		bool shown() const;
		SaveMode mode() const { return mode_; }
		void setSaves(std::vector<SaveEntry> saves);	// any time; the selection stays on its name
		const std::vector<SaveEntry>& saves() const { return saves_; }
		// For scripted tests.
		Widget* window() const;
		Table* list() const { return list_; }
		Button* actionButton() const { return action_; }	// LOAD / SAVE
		Button* cancelButton() const { return cancel_; }
		TickBox* resetTick() const { return reset_; }	// loading only
		EditBox* nameBox() const { return name_; }	// saving only
		RichText* info() const { return info_; }
		Prompt& prompt() const { return *prompt_; }

	private:
		Window* win_ = nullptr;
		Label* title_ = nullptr;
		Table* list_ = nullptr;
		RichText* info_ = nullptr;
		Button *action_ = nullptr, *cancel_ = nullptr;
		TickBox* reset_ = nullptr;
		EditBox* name_ = nullptr;
		std::unique_ptr<Prompt> prompt_;
		std::vector<SaveEntry> saves_;
		SaveMode mode_ = SaveMode::kLoad;
		SaveActions act_;
		std::function<void()> hidden_;
		Widget* before_ = nullptr;	// the screen (a child of root()) that had the keys when it opened: it gets them back (a menu)
		const SaveEntry* find(const std::string& name) const;
		void selected(const std::string& name);	// the list's selection changed
		void refresh();	// the details box and the action button's state
		void confirm();	// LOAD / SAVE, Enter, a double click
		void askDelete(const std::string& name);
		void hide();
	};
}
