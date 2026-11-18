#include "ui/savepanel.hpp"

#include <cmath>

#include "sokol_app.h"

#include "ui/prompt.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// The panel at 1080 lines (the layouts' 0.354167 x 0.300926 of 1920 x 1080) and its parts, from their fractions of it.
		const float kW = 680, kH = 325;

		std::string trimmed(const std::string& s)
		{
			const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
			return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
		}

		// Enter does the panel's action; Escape closes (Window).
		class Frame : public Window
		{
		public:
			std::function<void()> onEnter;
			Frame() : Window(Look::kPanel, 0) {}

			bool onKey(int key, bool down) override
			{
				if (key != SAPP_KEYCODE_ENTER && key != SAPP_KEYCODE_KP_ENTER) { return Window::onKey(key, down); }
				if (down && onEnter) { auto f = onEnter; f(); }
				return true;
			}
		};
	}

	SavePanel::SavePanel(SaveActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		Frame& win = root().add<Frame>();
		win_ = &win;
		win.halign = win.valign = A::kCenter;
		win.rect = { 0, 0, kW, kH };
		win.visible = false;
		win.onEscape = [this]
		{
			hide();
			if (act_.close) { act_.close(); }
		};
		win.onEnter = [this] { confirm(); };

		title_ = &win.add<Label>("", "Kenshi_PaintedTextFont_Medium");	// lbTitle
		title_->fit = false;
		title_->align = TextAlign::kCenter;
		title_->rect = { 5, 5, kW - 10, 35 };

		list_ = &win.add<Table>(std::vector<Table::Column>{ { "NAME", 0 }, { "DATE", 170 } });	// GamesList
		list_->rect = { 7, 44, 438, 271 };
		list_->sortBy(1, true);	// newest first
		list_->onSelect = [this](const std::string& name) { selected(name); };
		list_->onActivate = [this](const std::string&) { confirm(); };
		list_->onDelete = [this](const std::string& name) { askDelete(name); };

		Panel& box = win.add<Panel>(Look::kInset, 10);	// the details (InfoPanel in a Kenshi_GenericTextBox)
		box.rect = { 450, 43, 222, 167 };
		box.clipChildren = true;
		info_ = &box.add<RichText>();
		info_->halign = A::kStretch;
		info_->ink = style().textColour(Look::kInset, State::kNormal, false);
		info_->rowH = info_->gap = 22;

		reset_ = &win.add<TickBox>("Reset squad positions");	// lbResetSquadPosition + ResetTickBox
		reset_->rect = { 449, 220, 222, 35 };
		reset_->tooltip = "Move every squad back to its home or start position as the game loads";

		name_ = &win.add<EditBox>();	// SaveEditBox
		name_->rect = { 9, 272, 435, 40 };
		name_->maxBytes = 64;
		name_->onChange = [this](const std::string&) { refresh(); };
		name_->onEnter = [this] { confirm(); };

		action_ = &win.add<Button>("", Look::kButton, "Kenshi_PaintedTextFont_Medium");	// LoadButton / SaveButton
		action_->rect = { 453, 272, 100, 40 };
		action_->onClicked = [this] { confirm(); };
		cancel_ = &win.add<Button>("CANCEL", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		cancel_->rect = { 570, 272, 100, 40 };
		cancel_->onClicked = [this]
		{
			hide();
			if (act_.close) { act_.close(); }
		};
		prompt_ = std::make_unique<Prompt>();
	}

	SavePanel::~SavePanel()
	{
		prompt_.reset();
		if (ready() && win_) { root().remove(win_); }
	}

	bool SavePanel::shown() const { return win_->visible; }
	Widget* SavePanel::window() const { return win_; }

	const SaveEntry* SavePanel::find(const std::string& name) const
	{
		for (const SaveEntry& e : saves_)
		{
			if (e.name == name) { return &e; }
		}
		return nullptr;
	}

	void SavePanel::setSaves(std::vector<SaveEntry> saves)
	{
		saves_ = std::move(saves);
		std::vector<Table::Row> rows;
		for (const SaveEntry& e : saves_)
		{
			Table::Row& r = rows.emplace_back();
			r.id = e.name;
			r.cells = { e.name, e.date };
			r.keys = { std::nan(""), (double)e.when };
		}
		const int was = list_->selected();
		list_->setRows(std::move(rows));
		const std::vector<Table::Row>& now = list_->rows();
		if (list_->selected() < 0 && was >= 0 && !now.empty()) { list_->select(now[std::min<size_t>(was, now.size() - 1)].id); }	// deleted: its neighbour
		refresh();
	}

	void SavePanel::open(SaveMode mode, const std::string& suggested, std::function<void()> hidden)
	{
		if (win_->visible && hidden_) { auto f = std::move(hidden_); f(); }	// reopened by someone else: the last opener hears it closed
		hidden_ = std::move(hidden);
		mode_ = mode;
		if (act_.opening) { act_.opening(mode); }
		const bool load = mode == SaveMode::kLoad;
		title_->text = load ? "LOAD GAME" : "SAVE GAME";
		action_->text = load ? "LOAD" : "SAVE";
		reset_->visible = load;
		name_->visible = !load;
		list_->rect.h = load ? 271 : 225;	// the save list stops above the name box
		list_->select(list_->rows().empty() ? "" : list_->rows().front().id);
		if (!load) { name_->text = suggested.empty() ? list_->selectedId() : suggested; }
		refresh();
		before_ = focusedWidget();
		while (before_ && before_->parent() && before_->parent() != &root()) { before_ = before_->parent(); }
		if (before_ == win_) { before_ = nullptr; }
		win_->visible = true;
		win_->raise();
		focus(load ? (Widget*)list_ : (Widget*)name_);
	}

	void SavePanel::hide()
	{
		prompt_->close();
		if (!win_->visible) { return; }
		win_->visible = false;
		if (hidden_) { auto f = std::move(hidden_); hidden_ = nullptr; f(); }
		if (before_ && before_->shown()) { focus(before_); }
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
		before_ = nullptr;
	}

	void SavePanel::close() { hide(); }

	void SavePanel::selected(const std::string& name)
	{
		if (mode_ == SaveMode::kSave && !name.empty()) { name_->text = name; }
		refresh();
	}

	void SavePanel::refresh()
	{
		const SaveEntry* e = find(list_->selectedId());
		info_->text = e ? e->info : "";
		action_->enabled = mode_ == SaveMode::kLoad ? e != nullptr : !trimmed(name_->text).empty();
	}

	void SavePanel::confirm()
	{
		if (!win_->visible || prompt_->shown() || !action_->enabled) { return; }
		if (mode_ == SaveMode::kLoad)
		{
			const std::string name = list_->selectedId();
			const bool reset = reset_->checked;
			hide();
			if (act_.load) { act_.load(name, reset); }
			return;
		}
		const std::string name = trimmed(name_->text);
		const auto save = [this, name]
		{
			hide();
			if (act_.save) { act_.save(name); }
		};
		if (!find(name)) { save(); return; }
		prompt_->open({ "SAVE GAME", "Overwrite the save '" + name + "'?", { "Yes", "No" } }, [save](int i) { if (i == 0) { save(); } });
	}

	void SavePanel::askDelete(const std::string& name)
	{
		if (name.empty() || prompt_->shown()) { return; }
		prompt_->open({ "DELETE SAVE", "Delete the save '" + name + "'?", { "Yes", "No" }, 1 }, [this, name](int i)
		{
			if (i == 0 && act_.remove) { act_.remove(name); }
		});
	}
}
