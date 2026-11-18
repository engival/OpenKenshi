#include "ui/dev/trailspanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kW = 380, kBodyH = 440, kRowH = 24;

		class Swatch : public Widget
		{
		public:
			Color colour;

		protected:
			void paint() const override
			{
				gfx::fill(screen_, colour);
				gfx::frame(screen_, std::max(1.0f, std::round(scale_)), res::colour("Main").alpha(0.6f));
			}
		};
	}

	// A list row: click picks its character (the All row: id 0). The pick has the accent look, hover a faint fill. Kept across frames while the
	// row set stays (counts and the pick change in place), so a press held over it survives to its release.
	class TrailsPanel::Row : public Flex
	{
	public:
		bool picked = false;
		Label *points = nullptr, *pops = nullptr;

		Row(TrailsPanel& p, uint64_t id) : Flex(false, 8), panel_(p), id_(id)
		{
			solid = true;
			tooltip = id ? "Show only this character's trails (again: all)" : "Show every character's trails";
		}
		void onClick(Btn b) override { if (b == Btn::kLeft) { panel_.picked(id_); } }

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
		TrailsPanel& panel_;
		uint64_t id_;
	};

	std::string trailsSeconds(float s)
	{
		if (!(s > 0)) { return "0"; }
		char b[32];
		snprintf(b, sizeof b, "%.2f", s);
		std::string t = b;
		while (t.back() == '0') { t.pop_back(); }
		if (t.back() == '.') { t.pop_back(); }
		return t;
	}

	TrailsPanel::TrailsPanel(TrailsPanelActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("TRAILS", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->rect = { 8, 120, kW, CaptionWindow::kHeaderH + kBodyH };
		win_->visible = false;
		win_->body().rect.h = kBodyH;
		layout_ = std::make_unique<Layout>(win_->body(), "trailspanel", [this](Layout& l) { bind(l); });
		if (!list_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	TrailsPanel::~TrailsPanel()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void TrailsPanel::bind(Layout& l)
	{
		on_ = &l.at<TickBox>("on");
		on_->checked = last_.on;
		on_->onToggle = [this](bool on) { if (act_.setOn) { act_.setOn(on); } };
		clear_ = &l.at<Button>("clear");
		clear_->onClicked = [this] { if (act_.clear) { act_.clear(); } };
		seconds_ = &l.at<EditBox>("seconds");
		seconds_->text = trailsSeconds(last_.window);
		seconds_->onCommit = [this](const std::string& t) { commitWindow(t); };
		cap_ = &l.at<Button>("whole");
		capText();
		cap_->onClicked = [this]
		{
			seconds_->text = "0";
			if (act_.window) { act_.window(0); }
		};
		status_ = &l.at<Label>("status");
		status_->text = last_.status;
		list_ = &l.at<ScrollView>("rows");
		static_cast<Box&>(list_->body()).spacing = 0;
		rows_.clear();
		swatches_.clear();
		pops_.clear();
		dirty_ = true;
	}

	void TrailsPanel::capText()
	{
		const std::string n = trailsSeconds(last_.cap);
		cap_->text = "LAST " + n + " S";
		cap_->tooltip = "Window 0: the newest " + n + " s (debug.trails_seconds); older points go to the dump";
	}

	// Seconds >= 0 (a number and nothing else); anything else puts the shown value back.
	void TrailsPanel::commitWindow(const std::string& text)
	{
		char* end = nullptr;
		const float s = std::strtof(text.c_str(), &end);
		const bool ok = end != text.c_str() && std::all_of((const char*)end, text.c_str() + text.size(), [](char c) { return c == ' '; }) && std::isfinite(s) && s >= 0;
		if (!ok)
		{
			seconds_->text = trailsSeconds(last_.window);
			return;
		}
		seconds_->text = trailsSeconds(s);
		if (act_.window) { act_.window(s); }
	}

	void TrailsPanel::picked(uint64_t id)
	{
		const uint64_t to = id != 0 && id == last_.pick ? 0 : id;
		if (act_.pick) { act_.pick(to); }
	}

	void TrailsPanel::update(const TrailsPanelState& s)
	{
		if (!(s == last_))
		{
			auto sameRows = [](const std::vector<TrailsChar>& a, const std::vector<TrailsChar>& b)
			{
				return std::equal(a.begin(), a.end(), b.begin(), b.end(),
					[](const TrailsChar& x, const TrailsChar& y) { return x.id == y.id && x.name == y.name && x.colour == y.colour; });
			};
			if (!sameRows(s.chars, last_.chars)) { dirty_ = true; }	// the counts and the pick alone: in place (refresh), the rows stay
			last_ = s;
			on_->checked = s.on;
			if (focusedWidget() != seconds_) { seconds_->text = trailsSeconds(s.window); }	// not under the typing
			status_->text = s.status;
			capText();
			if (!dirty_) { refresh(); }
		}
		if (dirty_)
		{
			rebuild();
			refresh();
		}
	}

	// The counts and the pick into the kept rows.
	void TrailsPanel::refresh()
	{
		uint32_t points = 0, pops = 0;
		for (const TrailsChar& c : last_.chars)
		{
			points += c.points;
			pops += c.pops;
		}
		for (size_t i = 0; i < rows_.size(); i++)
		{
			const TrailsChar* c = i ? &last_.chars[i - 1] : nullptr;
			const uint32_t pts = c ? c->points : points, pp = c ? c->pops : pops;
			rows_[i]->picked = (c ? c->id : 0) == last_.pick;
			rows_[i]->points->text = std::to_string(pts);
			rows_[i]->pops->text = std::to_string(pp);
			rows_[i]->pops->colour = pp > 0 ? res::colour("BadBright") : res::colour("Main").alpha(0.8f);	// a pop is what the user looks for
		}
	}

	void TrailsPanel::rebuild()
	{
		dirty_ = false;
		Widget& body = list_->body();
		body.clear();
		rows_.clear();
		swatches_.clear();
		pops_.clear();
		auto line = [&](uint64_t id, const std::string& colour, const std::string& idText, const std::string& name, bool all)
		{
			Row& r = body.add<Row>(*this, id);
			r.halign = Widget::Align::kStretch;
			r.rect.h = kRowH;
			Widget& cell = r.add<Widget>(FlexItem{ .basis = 22 });
			cell.valign = Widget::Align::kStretch;
			Color c;
			Widget* sw = nullptr;
			if (!colour.empty() && Color::parse(colour, c))
			{
				Swatch& s = cell.add<Swatch>();
				s.colour = c;
				s.rect = { 4, 0, 14, 14 };
				s.valign = Widget::Align::kCenter;
				sw = &s;
			}
			Label& i = r.add<Label>(FlexItem{ .basis = 56 }, idText);
			i.colour = res::colour("Main").alpha(0.65f);
			Label& n = r.add<Label>(FlexItem{ .fill = 1 }, name);
			n.colour = res::colour("Main");
			Label& p = r.add<Label>(FlexItem{ .basis = 56 });
			p.align = TextAlign::kEnd;
			p.colour = res::colour("Main").alpha(0.8f);
			Label& q = r.add<Label>(FlexItem{ .basis = 44 });
			q.align = TextAlign::kEnd;
			r.points = &p;
			r.pops = &q;
			for (Widget* w : { (Widget*)&i, (Widget*)&n, (Widget*)&p, (Widget*)&q }) { w->valign = Widget::Align::kStretch; }
			r.add<Widget>(FlexItem{ .basis = 0 });	// the row gap after it keeps the numbers off the pick's frame
			rows_.push_back(&r);
			swatches_.push_back(sw);
			pops_.push_back(all ? nullptr : &q);
		};
		line(0, {}, "", "All (" + std::to_string(last_.chars.size()) + ")", true);
		for (const TrailsChar& c : last_.chars) { line(c.id, c.colour, std::to_string(c.id), c.name, false); }
	}

	void TrailsPanel::show(bool on)
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

	bool TrailsPanel::shown() const { return win_->visible; }
	Widget* TrailsPanel::row(size_t i) const { return i < rows_.size() ? rows_[i] : nullptr; }
}
