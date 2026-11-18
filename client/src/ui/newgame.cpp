#include "ui/newgame.hpp"

#include <algorithm>

#include "sokol_app.h"

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const float kW = 900, kH = 550;	// ours (kenshi-k16.17.4.1): the list column, then the title and description
		const float kPad = 10, kListW = 260, kTop = 44, kBodyH = 386;	// the list and the right column, kTop .. kTop + kBodyH
		const float kRightX = kPad + kListW + kPad, kRightW = kW - kRightX - kPad;
		const float kPlateH = 50, kBoxPad = 10;

		// Enter begins, Up / Down step the list; Escape cancels (Window).
		class Frame : public Window
		{
		public:
			std::function<void()> onEnter;
			std::function<void(int)> onStep;
			Frame() : Window(Look::kPanel, 0) {}

			bool onKey(int key, bool down) override
			{
				if (key == SAPP_KEYCODE_UP || key == SAPP_KEYCODE_DOWN)
				{
					if (down && onStep) { onStep(key == SAPP_KEYCODE_UP ? -1 : 1); }
					return true;
				}
				if (key != SAPP_KEYCODE_ENTER && key != SAPP_KEYCODE_KP_ENTER) { return Window::onKey(key, down); }
				if (down && onEnter) { auto f = onEnter; f(); }
				return true;
			}
		};
	}

	NewGamePanel::NewGamePanel(NewGameActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		Frame& win = root().add<Frame>();
		win_ = &win;
		win.halign = A::kCenter;	// centred near the top: the advanced options window opens below it
		win.rect = { 0, 54, kW, kH };
		win.visible = false;
		win.onEscape = [this]
		{
			hide();
			if (act_.cancel) { act_.cancel(); }
		};
		win.onEnter = [this] { confirm(); };
		win.onStep = [this](int d) { if (cur_ >= 0) { pick(cur_ + d); } };

		Label& head = win.add<Label>("CHOOSE YOUR BEGINNING", "Kenshi_PaintedTextFont_Large");
		head.fit = false;
		head.align = TextAlign::kCenter;
		head.rect = { 0, 1, kW, 35 };

		list_ = &win.add<Tabs>(Tabs::Side::kLeft);	// the options panel's side list
		list_->rect = { kPad, kTop, kListW, kBodyH };
		list_->onSelect = [this](size_t i) { if ((int)i != cur_) { pick((int)i); } };

		Panel& plate = win.add<Panel>(Look::kField, 0);
		plate.rect = { kRightX, kTop, kRightW, kPlateH };
		title_ = &plate.add<Label>("", "Kenshi_StandardFont_Large");
		title_->halign = title_->valign = A::kStretch;
		title_->fit = false;
		title_->align = TextAlign::kCenter;
		title_->colour = style().textColour(Look::kField, State::kNormal, false);

		Panel& box = win.add<Panel>(Look::kInset, kBoxPad);	// the description
		box.rect = { kRightX, kTop + kPlateH + 6, kRightW, kBodyH - kPlateH - 6 };
		scroll_ = &box.add<ScrollView>();
		scroll_->halign = scroll_->valign = A::kStretch;
		info_ = &scroll_->body().add<RichText>();
		info_->rect.w = kRightW - 2 * kBoxPad - 8;
		info_->ink = style().textColour(Look::kInset, State::kNormal, false);

		advanced_ = &win.add<Button>("ADVANCED OPTIONS", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		advanced_->rect = { (kW - 210) / 2, 440, 210, 35 };
		advanced_->onClicked = [this] { if (act_.advanced) { act_.advanced(); } };	// a toggle: the host shows or hides its window
		advanced_->enabled = act_.advanced != nullptr;
		begin_ = &win.add<Button>("BEGIN", Look::kButton, "Kenshi_PaintedTextFont_Large");
		begin_->rect = { (kW - 240) / 2, 485, 240, 55 };
		begin_->onClicked = [this] { confirm(); };
		cancel_ = &win.add<Button>("CANCEL", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		cancel_->rect = { kW - kPad - 100, 505, 100, 35 };
		cancel_->onClicked = [this]
		{
			hide();
			if (act_.cancel) { act_.cancel(); }
		};
		refresh();
	}

	NewGamePanel::~NewGamePanel()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	bool NewGamePanel::shown() const { return win_->visible; }
	Widget* NewGamePanel::window() const { return win_; }

	const std::string& NewGamePanel::currentId() const
	{
		static const std::string none;
		return cur_ >= 0 ? starts_[cur_].id : none;
	}

	void NewGamePanel::setStarts(std::vector<StartEntry> starts)
	{
		const std::string was = currentId();
		starts_ = std::move(starts);
		cur_ = starts_.empty() ? -1 : 0;
		for (size_t i = 0; i < starts_.size(); i++)
		{
			if (starts_[i].id == was) { cur_ = (int)i; }
		}
		rebuild();
		refresh();
	}

	void NewGamePanel::rebuild()
	{
		list_->clear();
		for (const StartEntry& e : starts_) { list_->add(e.title); }
	}

	void NewGamePanel::pick(int i)
	{
		const int n = (int)starts_.size();
		if (n == 0) { return; }
		const int was = cur_;
		cur_ = std::clamp(i, 0, n - 1);
		if (cur_ != was) { scroll_->offset = 0; }
		refresh();
	}

	void NewGamePanel::open(std::function<void()> hidden, int index)
	{
		if (win_->visible && hidden_) { auto f = std::move(hidden_); f(); }	// reopened by someone else: the last opener hears it closed
		hidden_ = std::move(hidden);
		pick(index);
		before_ = focusedWidget();
		while (before_ && before_->parent() && before_->parent() != &root()) { before_ = before_->parent(); }
		if (before_ == win_) { before_ = nullptr; }
		win_->visible = true;
		win_->raise();
		focus(win_);
	}

	void NewGamePanel::hide()
	{
		if (!win_->visible) { return; }
		win_->visible = false;
		if (hidden_) { auto f = std::move(hidden_); hidden_ = nullptr; f(); }
		if (before_ && before_->shown()) { focus(before_); }
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
		before_ = nullptr;
	}

	void NewGamePanel::close() { hide(); }

	void NewGamePanel::refresh()
	{
		const StartEntry* e = cur_ >= 0 ? &starts_[cur_] : nullptr;
		title_->text = e ? e->title : "";
		info_->text = e ? e->info : "";
		if (cur_ >= 0 && (size_t)cur_ != list_->selected()) { list_->select((size_t)cur_); }	// onSelect sees cur_ already there
		begin_->enabled = e != nullptr;
	}

	void NewGamePanel::confirm()
	{
		if (!win_->visible || !begin_->enabled) { return; }
		const std::string id = currentId();
		hide();
		if (act_.begin) { act_.begin(id); }
	}
}
