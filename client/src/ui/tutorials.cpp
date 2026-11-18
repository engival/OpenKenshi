#include "ui/tutorials.hpp"

#include <algorithm>

#include "sokol_app.h"

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const Color kPaging{ 0.4f, 0.380392f, 0.356863f, 1 };	// PagingText's own TextColour in both layouts
		const Color kCross{ 0.8f, 0.266f, 0.266f, 1 };	// the hint's CloseButton

		// Left / Right turn the pages; Escape closes (CaptionWindow).
		class PagedWindow : public CaptionWindow
		{
		public:
			std::function<void(int)> onTurn;
			PagedWindow(std::string caption, std::function<void()> onClose) : CaptionWindow(std::move(caption), std::move(onClose), 0) {}

			bool onKey(int key, bool down) override
			{
				if (key != SAPP_KEYCODE_LEFT && key != SAPP_KEYCODE_RIGHT) { return CaptionWindow::onKey(key, down); }
				if (down && onTurn) { auto f = onTurn; f(key == SAPP_KEYCODE_LEFT ? -1 : 1); }
				return true;
			}
		};

		// A sunken box of scrolling rich text.
		RichText& textBox(Widget& parent, const Rect& r, ScrollView*& scroll)
		{
			Panel& box = parent.add<Panel>(Look::kInset, 8);
			box.rect = r;
			scroll = &box.add<ScrollView>();
			scroll->halign = scroll->valign = Widget::Align::kStretch;
			RichText& t = scroll->body().add<RichText>();
			t.rect.w = r.w - 2 * 8 - 8;
			t.rowH = t.gap = 18;
			t.ink = style().textColour(Look::kInset, State::kNormal, false);
			return t;
		}

		Label& pagingLabel(Widget& parent, const Rect& r)
		{
			Label& l = parent.add<Label>("", "Kenshi_StandardFont_Medium");	// Kenshi_TextboxStandardText
			l.fit = false;
			l.align = TextAlign::kCenter;
			l.colour = kPaging;
			l.rect = r;
			return l;
		}
	}

	Tutorials::Tutorials(TutorialActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		// The tutorial window: Kenshi_TutorialPanel TutorialWindow, 0.26 x 0.324 of the screen at its top left, as in the capture.
		PagedWindow& win = root().add<PagedWindow>("", [this] { close(); });
		win_ = &win;
		win.rect = { 0, 0, 500, 350 };
		win.visible = false;
		win.onTurn = [this](int d) { turn(d); };
		win.body().rect.h = 350 - CaptionWindow::kHeaderH;
		text_ = &textBox(win.body(), { 8, 6, 484, 252 }, scroll_);
		dismiss_ = &win.body().add<Button>("DISMISS", Look::kButton, "Kenshi_PaintedTextFont_Small");
		dismiss_->rect = { 10, 268, 100, 30 };
		dismiss_->onClicked = [this] { finish(false); };
		paging_ = &pagingLabel(win.body(), { 110, 268, 134, 30 });
		prev_ = &win.body().add<Button>("PREVIOUS", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		prev_->rect = { 244, 268, 120, 30 };
		prev_->onClicked = [this] { turn(-1); };
		next_ = &win.body().add<Button>("CONTINUE", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		next_->rect = { 370, 268, 120, 30 };
		next_->onClicked = [this] { turn(1); };

		// The hint rows: TooltipsPanel (250 x 500), its bottom 512 units down (the capture's one row ends there); rows grow upward from it. Takes the
		// mouse only on its buttons.
		hintBox_ = &root().add<Widget>();
		hintBox_->rect = { 0, 12, 250, 500 };

		// TUTORIALS: Kenshi_TutorialsWindow, 0.52 x 0.412 of the screen, centred; the list in its left quarter.
		PagedWindow& br = root().add<PagedWindow>("TUTORIALS", [this] { showBrowser(false); });
		browser_ = &br;
		br.halign = br.valign = A::kCenter;
		br.rect = { 0, 0, 1000, 445 };
		br.visible = false;
		br.onTurn = [this](int d) { browseTurn(d); };
		const float bodyH = 445 - CaptionWindow::kHeaderH;
		br.body().rect.h = bodyH;
		Panel& listBox = br.body().add<Panel>(Look::kInset, 6);	// TutorialList (Kenshi_ListBox)
		listBox.rect = { 6, 6, 240, bodyH - 12 };
		ScrollView& ls = listBox.add<ScrollView>();
		ls.halign = ls.valign = A::kStretch;
		list_ = &ls.body().add<List>();
		list_->rect.w = 240 - 2 * 6 - 8;
		list_->padY = 1;
		list_->colour = style().textColour(Look::kInset, State::kNormal, false);	// dark ink on the original's parchment
		list_->onPick = [this](int) { bPage_ = 0; refreshBrowser(); };
		list_->onActivate = [this](int) { activate_->onClicked(); };
		bText_ = &textBox(br.body(), { 256, 6, 738, bodyH - 56 }, bScroll_);
		activate_ = &br.body().add<Button>("ACTIVATE", Look::kButton, "Kenshi_PaintedTextFont_Small");
		activate_->rect = { 256, bodyH - 42, 100, 30 };
		activate_->onClicked = [this]
		{
			const int i = list_->selected;
			if (i >= 0 && i < (int)all_.size() && act_.activate) { act_.activate(all_[i].id); }
		};
		bPaging_ = &pagingLabel(br.body(), { 360, bodyH - 42, 423, 30 });
		bPrev_ = &br.body().add<Button>("<<", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		bPrev_->rect = { 789, bodyH - 42, 100, 30 };
		bPrev_->onClicked = [this] { browseTurn(-1); };
		bNext_ = &br.body().add<Button>(">>", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		bNext_->rect = { 894, bodyH - 42, 100, 30 };
		bNext_->onClicked = [this] { browseTurn(1); };
		refresh();
		refreshHints();
		refreshBrowser();
	}

	Tutorials::~Tutorials()
	{
		if (!ready()) { return; }
		if (win_) { root().remove(win_); }
		if (hintBox_) { root().remove(hintBox_); }
		if (browser_) { root().remove(browser_); }
	}

	const Tutorial* Tutorials::find(const std::string& id) const
	{
		for (const Tutorial& t : all_)
		{
			if (t.id == id) { return &t; }
		}
		return nullptr;
	}

	void Tutorials::setAll(std::vector<Tutorial> all)
	{
		const int sel = list_->selected;
		const std::string selId = sel >= 0 && sel < (int)all_.size() ? all_[sel].id : std::string();
		all_ = std::move(all);
		hints_.erase(std::remove_if(hints_.begin(), hints_.end(), [this](const std::string& id) { return !find(id); }), hints_.end());
		if (!open_.empty() && !find(open_)) { close(); }
		list_->items.clear();
		list_->selected = all_.empty() ? -1 : 0;
		for (size_t i = 0; i < all_.size(); i++)
		{
			list_->items.push_back(all_[i].title);
			if (all_[i].id == selId) { list_->selected = (int)i; }
		}
		if (list_->selected != sel) { bPage_ = 0; }
		refresh();
		refreshHints();
		refreshBrowser();
	}

	void Tutorials::hint(const std::string& id)
	{
		if (!find(id) || std::find(hints_.begin(), hints_.end(), id) != hints_.end()) { return; }
		hints_.push_back(id);
		refreshHints();
	}

	void Tutorials::unhint(const std::string& id)
	{
		hints_.erase(std::remove(hints_.begin(), hints_.end(), id), hints_.end());
		refreshHints();
	}

	void Tutorials::open(const std::string& id)
	{
		const auto it = left_.find(id);
		open(id, it == left_.end() ? 0 : it->second);
	}

	void Tutorials::open(const std::string& id, int page)
	{
		const Tutorial* t = find(id);
		if (!t) { return; }
		if (open_ != id || page_ != page) { scroll_->offset = 0; }
		open_ = id;
		page_ = std::clamp(page, 0, std::max(0, (int)t->pages.size() - 1));
		left_[id] = page_;
		refresh();
		win_->visible = true;
		win_->raise();
		focus(win_);
	}

	void Tutorials::close()
	{
		open_.clear();
		page_ = 0;
		if (!win_->visible) { return; }
		win_->visible = false;
		if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool Tutorials::shown() const { return win_->visible; }
	bool Tutorials::browserShown() const { return browser_->visible; }
	Widget* Tutorials::window() const { return win_; }
	Widget* Tutorials::browser() const { return browser_; }

	void Tutorials::showBrowser(bool on)
	{
		if (on == browser_->visible) { return; }
		browser_->visible = on;
		if (on)
		{
			refreshBrowser();
			browser_->raise();
			focus(browser_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	void Tutorials::turn(int d)
	{
		const Tutorial* t = find(open_);
		if (!t) { return; }
		const int n = (int)t->pages.size(), to = page_ + d;
		if (to < 0) { return; }
		if (to >= n) { finish(true); return; }
		page_ = to;
		left_[open_] = page_;
		scroll_->offset = 0;
		refresh();
		if (act_.page) { act_.page(open_, page_); }
	}

	void Tutorials::finish(bool done)
	{
		if (open_.empty()) { return; }
		const std::string id = open_;
		unhint(id);
		left_.erase(id);
		close();
		const auto& f = done ? act_.done : act_.dismiss;
		if (f) { f(id); }
	}

	void Tutorials::browseTurn(int d)
	{
		const int i = list_->selected;
		if (i < 0 || i >= (int)all_.size()) { return; }
		bPage_ = std::clamp(bPage_ + d, 0, std::max(0, (int)all_[i].pages.size() - 1));
		bScroll_->offset = 0;
		refreshBrowser();
	}

	void Tutorials::refresh()
	{
		const Tutorial* t = find(open_);
		const int n = t ? (int)t->pages.size() : 0;
		std::string caption = t ? t->title : "";
		for (char& c : caption) { c = (char)std::toupper((unsigned char)c); }
		win_->caption().text = caption;
		text_->text = page_ < n ? t->pages[page_].text : "";
		paging_->text = n ? std::to_string(page_ + 1) + " / " + std::to_string(n) : "";
		prev_->enabled = page_ > 0;
		const bool last = page_ + 1 >= n;
		next_->text = last ? "FINISH" : t->pages[page_].waits ? "SKIP" : "CONTINUE";
		dismiss_->visible = !last;
	}

	void Tutorials::refreshHints()
	{
		const float rowH = 25;	// Kenshi_TutorialTooltipPanel, no gap
		while (hintName_.size() < hints_.size())
		{
			const int i = (int)hintName_.size();
			Button& name = hintBox_->add<Button>("", Look::kButton, "Kenshi_StandardFont_Medium");	// NameButton (Kenshi_Button2)
			name.tooltip = "Open the tutorial";
			name.onClicked = [this, i] { if (i < (int)hints_.size()) { open(hints_[i]); } };
			hintName_.push_back(&name);
			Button& x = hintBox_->add<Button>("X", Look::kButton, "Kenshi_PaintedTextFont_Medium");	// CloseButton
			x.tooltip = "Done with this tutorial";
			x.textColour = kCross;
			x.onClicked = [this, i]
			{
				if (i >= (int)hints_.size()) { return; }
				const std::string id = hints_[i];
				unhint(id);
				left_.erase(id);
				if (open_ == id) { close(); }
				if (act_.done) { act_.done(id); }
			};
			hintX_.push_back(&x);
		}
		const int n = (int)hints_.size();
		for (int i = 0; i < (int)hintName_.size(); i++)
		{
			const Tutorial* t = i < n ? find(hints_[i]) : nullptr;
			const float y = 500 - (n - i) * rowH;	// the newest at the bottom
			hintName_[i]->visible = hintX_[i]->visible = t != nullptr;
			hintName_[i]->text = t ? t->title : "";
			hintName_[i]->rect = { 0, y, 225, rowH };
			hintX_[i]->rect = { 223, y, 27, rowH };
		}
	}

	void Tutorials::refreshBrowser()
	{
		const int i = list_->selected;
		const Tutorial* t = i >= 0 && i < (int)all_.size() ? &all_[i] : nullptr;
		const int n = t ? (int)t->pages.size() : 0;
		bText_->text = bPage_ < n ? t->pages[bPage_].text : "";
		bPaging_->text = n ? std::to_string(bPage_ + 1) + " / " + std::to_string(n) : "";
		bPrev_->enabled = bPage_ > 0;
		bNext_->enabled = bPage_ + 1 < n;
		activate_->enabled = t != nullptr;
	}
}
