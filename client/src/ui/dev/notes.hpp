#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/dev/playtest_state.hpp"
#include "ui/widget.hpp"

// The playtest notes window (docs/UI.md "Playtest windows"; docs/reference/PLAYTEST.md; client coordinator 2026-11-07): a movable,
// resizable NOTES window (caption "NOTES  <scenario> <version>"), its body from client/data/ui/notes.layout. GOOD / OFF / BROKEN (the host's
// verdict marked) and MARK on top; the numbered notes, wrapped, each with its triage under it dimmed, in a scroll that keeps to the newest; a
// one-line edit box and ADD below. A note the host could not post to the --bead item carries why, under it in TipBad; the state's warning
// is one TipBad line above the box (kenshi-k16.2.130). Enter / ADD send add(text) when it isn't blank and clear the box (the focus stays in it); MARK puts
// mark()'s stamp before the box's text. typing(true / false) follows the box's keyboard focus (polled in update; false again on hide).
// Docked small at the right edge (user playtest feedback 2026-11-07): the host's open doesn't take the keyboard; a click anywhere in it or
// focusEntry() (Ctrl+N) does. Escape in the box keeps the draft and hands the keyboard back to the game, the window kept; Escape on the
// window (or the X) hides it and calls close(). Non-modal.
namespace ui
{
	class NotesWindow
	{
	public:
		explicit NotesWindow(NotesActions actions = {});
		~NotesWindow();
		void update(const NotesState& s);	// every frame (cheap when nothing changed); s.open shows / hides it
		bool shown() const;
		void focusEntry();	// Ctrl+N: shows it if hidden and puts the caret in the box (the host sets its open too)
		// For scripted tests.
		CaptionWindow* window() const { return win_; }
		const Layout& layout() const { return *layout_; }
		EditBox* entry() const { return entry_; }
		Button* button(const std::string& id) const;	// good, off, broken, mark, add
		size_t noteRows() const { return rows_.size(); }
		Label* noteText(size_t i) const { return i < rows_.size() ? rows_[i].first : nullptr; }
		Label* noteTriage(size_t i) const { return i < rows_.size() ? rows_[i].second : nullptr; }	// null: no triage yet
		Label* notePost(size_t i) const { return i < posts_.size() ? posts_[i] : nullptr; }	// null: posted (or no --bead)
		Label* warning() const { return warning_; }	// hidden when the state's warning is empty
		ScrollView* list() const { return list_; }

	private:
		class Entry;
		void bind(Layout& l);
		void rebuild();
		void submit();
		void hide();
		void setTyping(bool on);
		CaptionWindow* win_ = nullptr;
		std::unique_ptr<Layout> layout_;
		NotesActions act_;
		NotesState last_;
		bool built_ = false, typing_ = false;
		EditBox* entry_ = nullptr;
		ScrollView* list_ = nullptr;
		std::vector<std::pair<std::string, Button*>> buttons_;
		std::vector<std::pair<Label*, Label*>> rows_;
		std::vector<Label*> posts_;
		Label* warning_ = nullptr;
	};
}
