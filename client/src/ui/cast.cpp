#include "ui/cast.hpp"

#include <cctype>
#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		std::string lower(std::string s)
		{
			for (char& c : s) { c = (char)std::tolower((unsigned char)c); }
			return s;
		}

		std::string label(const HudCastRow& r) { return r.name.empty() ? "#" + std::to_string(r.id) : r.name; }
	}

	CastWindow::CastWindow(CastActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("CAST", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->rect = { 8, 40, 400, 480 };	// the left edge, above the status panel (the docked notes take the right)
		win_->resizable = true;
		win_->minSize = { 320, 300 };
		win_->visible = false;
		win_->body().rect.h = 480 - CaptionWindow::kHeaderH;
		layout_ = std::make_unique<Layout>(win_->body(), "cast", [this](Layout& l) { bind(l); });
		if (!table_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	CastWindow::~CastWindow()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void CastWindow::bind(Layout& l)
	{
		filter_ = &l.at<EditBox>("filter");
		filter_->text = filterText_;
		filter_->onChange = [this](const std::string& t)
		{
			filterText_ = t;
			rebuild();
		};
		filter_->onCommit = filter_->onChange;
		Widget& slot = l.at<Widget>("list");
		if (slot.children().empty())	// a slot's children stay across a reload
		{
			Table& t = slot.add<Table>(std::vector<Table::Column>{ { "Name", 0 }, { "Faction", 0 }, { "Id", 70, TextAlign::kEnd } });
			t.halign = t.valign = Widget::Align::kStretch;
			t.sortBy(0, false);
		}
		table_ = static_cast<Table*>(slot.children().front().get());
		table_->onSelect = [this](const std::string& id) { if (!syncing_ && !id.empty() && act_.watch) { act_.watch(std::stoull(id)); } };
		table_->onActivate = [this](const std::string& id) { if (!id.empty() && act_.follow) { act_.follow(std::stoull(id)); } };
		count_ = &l.at<Label>("count");
		if (built_) { rebuild(); }
	}

	void CastWindow::rebuild()
	{
		const std::string needle = lower(filterText_);
		const Color dim = res::colour("Main").alpha(0.45f);
		std::vector<Table::Row> rows;
		for (const HudCastRow& c : cast_)
		{
			const std::string name = label(c);
			if (!needle.empty() && lower(name).find(needle) == std::string::npos && lower(c.faction).find(needle) == std::string::npos) { continue; }
			Table::Row r;
			r.id = std::to_string(c.id);
			r.cells = { name, c.faction, r.id };
			r.keys = { NAN, NAN, (double)c.id };
			if (c.down) { r.colour = dim; }
			r.tip = name + (c.faction.empty() ? "" : "\n" + c.faction) + (c.down ? "\nDown" : "");
			rows.push_back(std::move(r));
		}
		const size_t n = rows.size();
		syncing_ = true;
		table_->setRows(std::move(rows));
		table_->select(watched_ ? std::to_string(watched_) : std::string());
		syncing_ = false;
		const std::string total = std::to_string(cast_.size()) + (cast_.size() == 1 ? " character" : " characters");
		count_->text = needle.empty() ? total : std::to_string(n) + " of " + total;
	}

	void CastWindow::update(const std::vector<HudCastRow>& cast, uint64_t watched)
	{
		if (built_ && cast == cast_ && watched == watched_) { return; }
		cast_ = cast;
		watched_ = watched;
		built_ = true;
		rebuild();
	}

	void CastWindow::show(bool on)
	{
		if (on == win_->visible) { return; }
		win_->visible = on;
		if (on)
		{
			win_->raise();
			focus(table_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool CastWindow::shown() const { return win_->visible; }
}
