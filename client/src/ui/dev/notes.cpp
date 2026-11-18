#include "ui/dev/notes.hpp"

#include "sokol_app.h"
#include "ui/keys.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const char* const kVerdicts[] = { "good", "off", "broken" };

		bool blank(const std::string& s) { return s.find_first_not_of(" \t") == std::string::npos; }
	}

	// The note box: Escape keeps the draft (an EditBox's Escape reverts it) and hands the keyboard back to the game (the window stays docked).
	// The playtest.notes chord (Ctrl+N) isn't the box's: it goes on to the host, whose rule closes the window while typing.
	class NotesWindow::Entry : public EditBox
	{
	public:
		bool escaping = false;
		bool onKey(int key, bool down) override
		{
			sapp_event e = {};
			e.type = down ? SAPP_EVENTTYPE_KEY_DOWN : SAPP_EVENTTYPE_KEY_UP;
			e.key_code = (sapp_keycode)key;
			e.modifiers = mods();
			if (down && keys::pressed(e, "playtest.notes")) { return false; }
			if (key != SAPP_KEYCODE_ESCAPE) { return EditBox::onKey(key, down); }
			if (!down) { return true; }
			escaping = true;
			EditBox::onKey(SAPP_KEYCODE_ENTER, true);	// keep the text
			escaping = false;
			focus(nullptr);	// the game's keys again; a second Escape is the game menu's
			return true;
		}
	};

	NotesWindow::NotesWindow(NotesActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("NOTES", [this]
		{
			hide();
			if (act_.close) { act_.close(); }
		}, 0);
		win_->halign = Widget::Align::kEnd;
		win_->rect = { 8, 90, 420, 340 };	// docked at the right edge, out of the playfield (user playtest feedback 2026-11-07)
		win_->resizable = true;
		win_->minSize = { 360, 260 };
		win_->visible = false;
		win_->body().rect.h = 340 - CaptionWindow::kHeaderH;
		layout_ = std::make_unique<Layout>(win_->body(), "notes", [this](Layout& l) { bind(l); });
		if (!entry_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	NotesWindow::~NotesWindow()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void NotesWindow::bind(Layout& l)
	{
		buttons_.clear();
		for (const char* id : kVerdicts)
		{
			Button& b = l.at<Button>(id);
			b.look = Look::kTabTop;
			b.onClicked = [this, id] { if (act_.verdict) { act_.verdict(id); } };
			buttons_.push_back({ id, &b });
		}
		for (const char* id : { "mark", "add" })
		{
			Button& b = l.at<Button>(id);
			b.textColour = res::colour("Main");	// the look's ButtonText reads as greyed
			buttons_.push_back({ id, &b });
		}
		button("add")->onClicked = [this] { submit(); };
		button("mark")->onClicked = [this]
		{
			if (!act_.mark) { return; }
			const std::string stamp = act_.mark();
			entry_->text = stamp + (entry_->text.empty() || stamp.empty() ? "" : " ") + entry_->text;
			focus(entry_);
			entry_->onKey(SAPP_KEYCODE_END, true);	// the caret after the text
		};
		Widget& slot = l.at<Widget>("entry");
		if (slot.children().empty())	// a slot's children stay across a reload
		{
			Entry& e = slot.add<Entry>();
			e.halign = e.valign = Widget::Align::kStretch;
			e.maxBytes = 1000;
			e.tooltip = "A note: what you saw, one line. Enter adds it";
		}
		entry_ = static_cast<EditBox*>(slot.children().front().get());
		entry_->onEnter = [this] { if (!static_cast<Entry*>(entry_)->escaping) { submit(); } };
		warning_ = &l.at<Label>("warning");
		warning_->text = last_.warning;
		warning_->visible = !last_.warning.empty();
		list_ = &l.at<ScrollView>("notes");
		list_->stickToEnd = true;
		static_cast<Box&>(list_->body()).spacing = 4;
		rows_.clear();
		posts_.clear();
		if (built_) { rebuild(); }
	}

	Button* NotesWindow::button(const std::string& id) const
	{
		for (const auto& [k, b] : buttons_) { if (k == id) { return b; } }
		return nullptr;
	}

	void NotesWindow::submit()
	{
		if (!blank(entry_->text) && act_.add) { act_.add(entry_->text); }
		if (!blank(entry_->text)) { entry_->text.clear(); }
		entry_->onKey(SAPP_KEYCODE_HOME, true);
		focus(entry_);	// keep typing
	}

	void NotesWindow::rebuild()
	{
		Widget& body = list_->body();
		body.clear();
		rows_.clear();
		posts_.clear();
		const Color main = res::colour("Main");
		if (last_.notes.empty())
		{
			Label& l = body.add<Label>("No notes yet. Type below; MARK stamps the time, the character and its place.", "Kenshi_StandardFont_Medium");
			l.wrap = true;
			l.halign = Widget::Align::kStretch;
			l.colour = main.alpha(0.5f);
		}
		for (const PlaytestNote& n : last_.notes)
		{
			Label& t = body.add<Label>(std::to_string(n.n) + ". " + n.text, "Kenshi_StandardFont_Medium");
			t.wrap = true;	// the user's observation: never clipped
			t.halign = Widget::Align::kStretch;
			t.colour = main;
			auto under = [&](const std::string& text, Color c)	// indented under its note (a Box ignores rect.x)
			{
				Flex& line = body.add<Flex>(false, 0);
				line.halign = Widget::Align::kStretch;
				line.add<Widget>(FlexItem{ .basis = 24 });
				Label& l = line.add<Label>(FlexItem{ .fill = 1 }, text, "Kenshi_StandardFont_Medium");
				l.wrap = true;
				l.halign = Widget::Align::kStretch;	// its height from its laid-out width
				l.colour = c;
				return &l;
			};
			Label* tr = n.triage.empty() ? nullptr : under(n.triage, main.alpha(0.55f));
			posts_.push_back(n.post.empty() ? nullptr : under(n.post, res::colour("TipBad").alpha(0.7f)));	// not on the item, and why
			rows_.push_back({ &t, tr });
		}
		list_->scrollToEnd();
	}

	void NotesWindow::setTyping(bool on)
	{
		if (on == typing_) { return; }
		typing_ = on;
		if (act_.typing) { act_.typing(on); }
	}

	void NotesWindow::update(const NotesState& s)
	{
		if (!built_ || !(s == last_))
		{
			const bool notes = !built_ || s.notes != last_.notes;
			last_ = s;
			built_ = true;
			if (notes) { rebuild(); }
			warning_->text = s.warning;
			warning_->visible = !s.warning.empty();
			win_->caption().text = "NOTES" + (s.scenario.empty() ? std::string() : "  " + s.scenario) + (s.version.empty() ? std::string() : " " + s.version);
			for (const char* id : kVerdicts) { button(id)->checked = s.verdict == id; }
			if (s.open != win_->visible)
			{
				if (!s.open) { hide(); }
				else
				{
					win_->visible = true;
					win_->raise();	// the keyboard stays the game's until a click or focusEntry()
				}
			}
		}
		// A click anywhere in the window (header, list, a verdict) lands in the box.
		for (Widget* w = focusedWidget(); w && win_->visible && w != entry_; w = w->parent())
		{
			if (w == win_)
			{
				focus(entry_);
				break;
			}
		}
		setTyping(win_->visible && focused(entry_));
	}

	void NotesWindow::focusEntry()
	{
		win_->visible = true;
		last_.open = true;	// the host's next state says so too
		win_->raise();
		focus(entry_);
		setTyping(true);
	}

	void NotesWindow::hide()
	{
		win_->visible = false;
		last_.open = false;	// the host's next state says so too
		if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
		setTyping(false);
	}

	bool NotesWindow::shown() const { return win_->visible; }
}
