#include "ui/dev/roadspanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kW = 680, kBodyH = 724, kRowH = 24;
		// The columns, header and rows alike (units; 0: the fill one).
		// Sized for the widest text each takes (kenshi-6t03): ranks to 4 digits, "T-junction", "33/40", "162°", "411 u", "UNSURE",
		// a sorted header's arrow; the Road column gets the rest ("#2107 (junctions 4205-4295)").
		constexpr float kColRank = 46, kColRoad = 0, kColKind = 78, kColMarker = 54, kColRatio = 42, kColTurn = 40, kColExtra = 56, kColMark = 62;

		std::string num(float v)
		{
			char b[32];
			snprintf(b, sizeof b, "%.2f", v);
			return b;
		}

		bool shownBy(int filter, int mark)
		{
			switch (filter)
			{
			case RoadsPanel::kUnmarked: return mark == kRoadUnmarked;
			case RoadsPanel::kOk: return mark == kRoadOk;
			case RoadsPanel::kUnsure: return mark == kRoadUnsure;
			case RoadsPanel::kBad: return mark == kRoadBad;
			default: return true;
			}
		}
	}

	const char* roadMarkName(int mark)
	{
		switch (mark)
		{
		case kRoadOk: return "OK";
		case kRoadUnsure: return "UNSURE";
		case kRoadBad: return "BAD";
		default: return "-";
		}
	}

	Color roadMarkColour(int mark, bool inset)
	{
		switch (mark)
		{
		case kRoadOk: return res::colour(inset ? "RoadMarkOk" : "Good");
		case kRoadUnsure: return res::colour(inset ? "RoadMarkUnsure" : "DebugKey");
		case kRoadBad: return res::colour(inset ? "RoadMarkBad" : "BadBright");
		default: return res::colour(inset ? "Secondary" : "Main").alpha(0.5f);
		}
	}

	std::string roadsVec(const RoadsVec& v)
	{
		char b[96];
		snprintf(b, sizeof b, "%.1f %.1f %.1f", v.x, v.y, v.z);
		return b;
	}

	std::string roadsTurn(float deg)
	{
		char b[32];
		snprintf(b, sizeof b, "%.0f\u00b0", deg);
		return b;
	}

	std::string roadsExtra(float u)
	{
		char b[32];
		snprintf(b, sizeof b, "%.0f u", u);
		return b;
	}

	// A list row: a click selects its suspect. The selection has the accent look, hover a faint fill. Kept across frames while the rows stay.
	class RoadsPanel::Row : public Flex
	{
	public:
		bool picked = false;
		Label* mark = nullptr;
		const int suspect;

		Row(RoadsPanel& p, int suspect) : Flex(false, 6), suspect(suspect), panel_(p)
		{
			solid = true;
			tooltip = "Select this marker (the overlay shows it)";
		}
		void onClick(Btn b) override
		{
			if (b == Btn::kLeft && panel_.act_.select) { panel_.act_.select(suspect); }
		}

	protected:
		void paint() const override
		{
			if (picked)
			{
				gfx::fill(screen_, res::colour("Accent").alpha(0.22f));
				gfx::frame(screen_, std::max(1.0f, std::round(scale_)), res::colour("Accent").alpha(0.8f));
			}
			else if (hovered(this)) { gfx::fill(screen_, res::colour("Main").alpha(0.08f)); }
		}

	private:
		RoadsPanel& panel_;
	};

	// A sortable column header: a click sorts by it (again: the other way); the sorted one is bright with its direction.
	class RoadsPanel::Head : public Label
	{
	public:
		Head(RoadsPanel& p, Sort by, std::string text) : Label(text), by(by), base(std::move(text)), panel_(p)
		{
			solid = true;
			tooltip = "Sort by this column (again: the other way)";
		}
		const Sort by;
		const std::string base;
		void onClick(Btn b) override
		{
			if (b == Btn::kLeft) { panel_.sortBy(by); }
		}

	protected:
		void paint() const override
		{
			if (hovered(this)) { gfx::fill(screen_, res::colour("Main").alpha(0.08f)); }
			Label::paint();
		}

	private:
		RoadsPanel& panel_;
	};

	RoadsPanel::RoadsPanel(RoadsPanelActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("ROADS", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->rect = { 8, 120, kW, CaptionWindow::kHeaderH + kBodyH };
		win_->visible = false;
		win_->body().rect.h = kBodyH;
		layout_ = std::make_unique<Layout>(win_->body(), "roadspanel", [this](Layout& l) { bind(l); });
		if (!list_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	RoadsPanel::~RoadsPanel()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void RoadsPanel::bind(Layout& l)
	{
		filter_ = &l.at<Tabs>("filter");
		filter_->clear();
		for (const char* t : { "ALL", "UNMARKED", "OK", "UNSURE", "BAD" }) { filter_->add(t); }
		filter_->select((size_t)filterOn_);
		filter_->onSelect = [this](size_t i) { filter((int)i); };
		kindTabs_ = &l.at<Tabs>("kind");
		kindTabs_->onSelect = [this](size_t i) { filterKind(i > 0 && i <= kinds_.size() ? kinds_[i - 1] : std::string()); };
		kinds_.clear();
		kindTabs_->clear();
		fillKinds();
		head_ = &l.at<Widget>("head");
		list_ = &l.at<ScrollView>("rows");
		static_cast<Box&>(list_->body()).spacing = 0;
		const std::pair<int, const char*> marks[] = { { kRoadOk, "markok" }, { kRoadUnsure, "markunsure" }, { kRoadBad, "markbad" }, { kRoadUnmarked, "markclear" } };
		for (const auto& [m, id] : marks)
		{
			marks_[m] = &l.at<Button>(id);
			marks_[m]->onClicked = [this, m = m] { if (act_.mark && last_.info.valid) { act_.mark(m); } };
		}
		copy_ = &l.at<Button>("copy");
		copy_->onClicked = [this] { if (act_.copy && last_.info.valid) { act_.copy(); } };
		status_ = &l.at<Label>("status");
		info_.clear();
		for (const char* id : { "road", "roads", "marker", "pos", "prev", "next", "suspect", "junction", "mark" }) { info_.emplace_back(id, &l.at<Label>(id)); }
		rows_.clear();
		for (Head*& h : heads_) { h = nullptr; }
		dirty_ = true;
		fillInfo();
	}

	void RoadsPanel::sortBy(Sort by)
	{
		if (by == sort_) { desc_ = !desc_; }
		else
		{
			sort_ = by;
			desc_ = by != Sort::kRank;	// rank: the worst first; extra and mark: the largest (BAD) first
		}
		dirty_ = true;
		rebuild();
		refresh();
	}

	void RoadsPanel::filter(int f)
	{
		f = std::clamp(f, (int)kAll, (int)kBad);
		if (f == filterOn_) { return; }
		filterOn_ = f;
		if (filter_->selected() != (size_t)f) { filter_->select((size_t)f); }
		dirty_ = true;
		rebuild();
		refresh();
	}

	void RoadsPanel::filterKind(const std::string& kind)
	{
		const auto it = std::find(kinds_.begin(), kinds_.end(), kind);
		const std::string k = it == kinds_.end() ? std::string() : kind;
		const size_t tab = it == kinds_.end() ? 0 : (size_t)(it - kinds_.begin()) + 1;
		if (kindTabs_->selected() != tab) { kindTabs_->select(tab); }
		if (k == kindOn_) { return; }
		kindOn_ = k;
		dirty_ = true;
		rebuild();
		refresh();
	}

	// ALL, then each kind in the suspects (first seen first; an empty kind has no tab). A kind gone from the data drops its filter.
	void RoadsPanel::fillKinds()
	{
		std::vector<std::string> kinds;
		for (const RoadsSuspect& q : last_.suspects)
		{
			if (!q.kind.empty() && std::find(kinds.begin(), kinds.end(), q.kind) == kinds.end()) { kinds.push_back(q.kind); }
		}
		if (kinds == kinds_ && kindTabs_->count() == kinds_.size() + 1) { return; }
		kinds_ = std::move(kinds);
		kindTabs_->clear();
		kindTabs_->add("ALL", "Every kind of suspect");
		for (const std::string& k : kinds_)
		{
			std::string up = k;
			for (char& c : up) { c = (char)std::toupper((unsigned char)c); }
			kindTabs_->add(up, "Only the " + k + " suspects");
		}
		if (std::find(kinds_.begin(), kinds_.end(), kindOn_) == kinds_.end())
		{
			kindOn_.clear();
			dirty_ = true;
		}
		const auto it = std::find(kinds_.begin(), kinds_.end(), kindOn_);
		kindTabs_->select(kindOn_.empty() ? 0 : (size_t)(it - kinds_.begin()) + 1);
	}

	void RoadsPanel::update(const RoadsPanelState& s)
	{
		if (!(s == last_))
		{
			const bool kinds = !(s.suspects == last_.suspects);
			if (kinds) { dirty_ = true; }	// the selection alone: in place
			last_ = s;
			if (kinds) { fillKinds(); }
			status_->text = s.status;
			fillInfo();
			if (!dirty_) { refresh(); }
		}
		if (dirty_)
		{
			rebuild();
			refresh();
		}
	}

	void RoadsPanel::fillInfo()
	{
		const RoadsInfo& in = last_.info;
		auto set = [&](const char* id, const std::string& t, Color c)
		{
			for (auto& [n, l] : info_)
			{
				if (n == id)
				{
					l->text = t;
					l->colour = c;
				}
			}
		};
		const Color main = res::colour("Main"), dim = res::colour("Main").alpha(0.6f);
		for (Button* b : marks_) { if (b) { b->enabled = in.valid; } }
		if (copy_) { copy_->enabled = in.valid; }
		if (!in.valid)
		{
			set("road", "Nothing selected: click a row, or a marker in the world", dim);
			for (const char* id : { "roads", "marker", "pos", "prev", "next", "suspect", "junction", "mark" }) { set(id, "", main); }
			return;
		}
		set("road", in.road, main);
		set("roads", in.roads.empty() ? "-" : in.roads, in.roads.empty() ? dim : main);
		set("marker", std::to_string(in.marker) + " of " + std::to_string(in.count), main);
		set("pos", roadsVec(in.pos), main);
		set("prev", in.hasPrev ? roadsVec(in.prev) : "none (road end)", in.hasPrev ? main.alpha(0.8f) : dim);
		set("next", in.hasNext ? roadsVec(in.next) : "none (road end)", in.hasNext ? main.alpha(0.8f) : dim);
		if (in.suspect >= 0 && in.suspect < (int)last_.suspects.size())
		{
			const RoadsSuspect& q = last_.suspects[in.suspect];
			set("suspect", "#" + std::to_string(q.rank) + (q.kind.empty() ? "" : " " + q.kind) + "   ratio " + num(q.ratio) + "   turn " + roadsTurn(q.turn) + "   extra " + roadsExtra(q.extra),
				res::colour("Accent"));
		}
		else { set("suspect", "not a suspect", dim); }
		if (!in.junction.empty()) { set("junction", in.junction + ", degree " + std::to_string(in.degree), main); }
		else { set("junction", "not a road end", dim); }
		set("mark", in.mark == kRoadUnmarked ? "unmarked" : roadMarkName(in.mark), roadMarkColour(in.mark, false));
	}

	// The selection and the marks into the kept rows.
	void RoadsPanel::refresh()
	{
		for (Row* r : rows_)
		{
			r->picked = r->suspect == last_.selected;
			const int m = last_.suspects[r->suspect].mark;
			r->mark->text = roadMarkName(m);
			r->mark->colour = roadMarkColour(m, true);
		}
	}

	void RoadsPanel::rebuild()
	{
		dirty_ = false;
		auto cells = [](Flex& r, auto&& cell)
		{
			cell(kColRank, TextAlign::kEnd, 0);
			cell(kColRoad, TextAlign::kStart, 1);
			cell(kColKind, TextAlign::kStart, 2);
			cell(kColMarker, TextAlign::kEnd, 3);
			cell(kColRatio, TextAlign::kEnd, 4);
			cell(kColTurn, TextAlign::kEnd, 5);
			cell(kColExtra, TextAlign::kEnd, 6);
			cell(kColMark, TextAlign::kCenter, 7);
			r.add<Widget>(FlexItem{ .basis = 14 });	// the scroll bar's width, so the header lines up with the rows
		};
		auto item = [](float w) { return w > 0 ? FlexItem{ .basis = w } : FlexItem{ .fill = 1 }; };

		// The header: one Flex in the slot, built once per bind.
		if (!heads_[0])
		{
			head_->clear();
			Flex& h = head_->add<Flex>(false, 6);
			h.halign = h.valign = Widget::Align::kStretch;
			static const char* names[] = { "Rank", "Road", "Kind", "Marker", "Ratio", "Turn", "Extra", "Mark" };
			cells(h, [&](float w, TextAlign a, int i)
			{
				Label* l = nullptr;
				if (i == 0 || i == 6 || i == 7)
				{
					const Sort s = i == 0 ? Sort::kRank : i == 6 ? Sort::kExtra : Sort::kMark;
					Head& hd = h.add<Head>(item(w), *this, s, names[i]);
					heads_[(int)s] = &hd;
					l = &hd;
				}
				else
				{
					l = &h.add<Label>(item(w), names[i]);
					l->colour = res::colour("Main").alpha(0.6f);
				}
				l->align = a;
				l->valign = Widget::Align::kStretch;
			});
		}
		for (Head* hd : heads_)
		{
			const bool on = hd->by == sort_;
			hd->text = on ? hd->base + (desc_ ? " v" : " ^") : hd->base;
			hd->colour = on ? res::colour("Accent") : res::colour("Main").alpha(0.6f);
		}

		// The view: the filter, then the sort (stable, ties by rank).
		std::vector<int> order;
		for (int i = 0; i < (int)last_.suspects.size(); i++) { if (shownBy(filterOn_, last_.suspects[i].mark) && (kindOn_.empty() || last_.suspects[i].kind == kindOn_)) { order.push_back(i); } }
		auto key = [&](int i) -> float
		{
			const RoadsSuspect& q = last_.suspects[i];
			return sort_ == Sort::kRank ? (float)q.rank : sort_ == Sort::kExtra ? q.extra : (float)q.mark;
		};
		std::stable_sort(order.begin(), order.end(), [&](int a, int b)
		{
			const float ka = key(a), kb = key(b);
			if (ka != kb) { return desc_ ? ka > kb : ka < kb; }
			return last_.suspects[a].rank < last_.suspects[b].rank;
		});

		Widget& body = list_->body();
		body.clear();
		rows_.clear();
		for (int i : order)
		{
			const RoadsSuspect& q = last_.suspects[i];
			Row& r = body.add<Row>(*this, i);
			r.halign = Widget::Align::kStretch;
			r.rect.h = kRowH;
			const std::string text[] = { std::to_string(q.rank), q.road, q.kind.empty() ? "-" : q.kind, std::to_string(q.marker) + "/" + std::to_string(q.count), num(q.ratio),
				roadsTurn(q.turn), roadsExtra(q.extra), "" };
			cells(r, [&](float w, TextAlign a, int c)
			{
				Label& l = r.add<Label>(item(w), text[c]);
				l.align = a;
				l.valign = Widget::Align::kStretch;
				l.colour = c == 0 ? res::colour("Secondary").alpha(0.65f) : res::colour("Secondary");	// the inset's ink (dark on the original's parchment)
				if (c == 7) { r.mark = &l; }
			});
			rows_.push_back(&r);
		}
		if (order.empty())
		{
			Label& l = body.add<Label>(last_.suspects.empty() ? "No suspect markers" : "No suspect with this mark and kind");
			l.colour = res::colour("Secondary").alpha(0.5f);
			l.rect.h = kRowH;
		}
	}

	void RoadsPanel::show(bool on)
	{
		if (win_->visible == on) { return; }
		win_->visible = on;
		if (on)
		{
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool RoadsPanel::shown() const { return win_->visible; }
	Widget* RoadsPanel::row(size_t i) const { return i < rows_.size() ? rows_[i] : nullptr; }
	int RoadsPanel::rowSuspect(size_t i) const { return i < rows_.size() ? rows_[i]->suspect : -1; }
	Label* RoadsPanel::markLabel(size_t i) const { return i < rows_.size() ? rows_[i]->mark : nullptr; }
	Widget* RoadsPanel::header(Sort s) const { return heads_[(int)s]; }

	Label* RoadsPanel::infoLabel(const std::string& id) const
	{
		for (const auto& [n, l] : info_) { if (n == id) { return l; } }
		return nullptr;
	}
}
