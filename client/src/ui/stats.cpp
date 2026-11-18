#include "ui/stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Widths (units), after Kenshi_StatsWindow at 1080 lines (1270 x 655): four skill columns, attributes to their left, derived stats on the right.
		const float kAttrW = 240, kColW = 160, kColGap = 16, kPad = 10, kGap = 12;
		const float kSkillsW = 4 * kColW + 3 * kColGap + 2 * kPad, kDerivedW = 270, kLeftW = kAttrW + kGap + kSkillsW;
		const float kAboutW = 360, kTextW = kLeftW - 2 * kPad - kAboutW - 20;	// the description's two panels (the original's 39 % / 56 %)
		const float kTopMinH = 260, kDescMinH = 200;	// the insets' least heights
		const float kRowH = 21, kNextH = 20;	// the original's rows: 14 px at 720

		std::string whole(float v)
		{
			char b[16];
			snprintf(b, sizeof b, "%d", (int)std::lround(v));
			return b;
		}

		bool modified(const StatRow& r) { return r.base >= 0 && std::lround(r.base) != std::lround(r.level); }

		// The rows' ink: dark on the original's parchment.
		Color ink() { return style().textColour(Look::kInset, State::kNormal, false); }

		// A heading over an inset of width w; content goes into the inset.
		Panel& section(Box& parent, const char* title, float w, Label** head = nullptr)
		{
			Box& g = parent.add<Box>(true, 4);
			Label& h = g.add<Label>(title, "Kenshi_PaintedTextFont_Medium");
			h.halign = Widget::Align::kCenter;
			h.colour = res::colour("Main").alpha(0.7f);
			if (head) { *head = &h; }
			Panel& p = g.add<Panel>(Look::kInset, kPad);
			p.rect.w = w;
			return p;
		}

		// Rows are rebuilt only when names, groups or columns change; anything else is written into the rows.
		bool sameShape(const StatsState& a, const StatsState& b)
		{
			auto names = [](const std::vector<StatRow>& x, const std::vector<StatRow>& y)
			{
				return std::equal(x.begin(), x.end(), y.begin(), y.end(), [](const StatRow& p, const StatRow& q) { return p.name == q.name; });
			};
			if (!names(a.attributes, b.attributes) || a.skills.size() != b.skills.size()) { return false; }
			for (size_t i = 0; i < a.skills.size(); i++)
			{
				const StatGroup &p = a.skills[i], &q = b.skills[i];
				if (p.title != q.title || p.column != q.column || !names(p.rows, q.rows)) { return false; }
			}
			return true;
		}
	}

	// A stat: name on the left, level on the right. A click picks it for the description.
	class Stats::Line : public Widget
	{
	public:
		std::string name, level;
		Color nameInk, levelInk;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");
		std::function<void()> onPick;

		Line() { solid = true; }

		void set(const StatRow& r)
		{
			name = r.name;
			level = whole(r.level);
			nameInk = levelInk = ink();
			if (modified(r)) { levelInk = res::colour(r.level > r.base ? "StatUp" : "StatDown", nameInk); }
		}

		void onClick(Btn b) override
		{
			if (b == Btn::kLeft && onPick) { onPick(); }
		}

	protected:
		void paint() const override
		{
			if (!font) { return; }
			const float y = std::round(screen_.y + (screen_.h - font->lineHeight()) / 2);
			font->draw(screen_.x, y, name, nameInk);
			font->draw(screen_.right() - font->width(level), y, level, levelInk);
		}
	};

	// The description's "Next Level:" bar: the progress as a fill across the panel, the label and percentage over it.
	class Stats::NextLevel : public Widget
	{
	public:
		float xp = 0;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");

	protected:
		void paint() const override
		{
			gfx::fill(screen_, res::colour("XpTrack"));
			if (xp > 0) { gfx::fill({ screen_.x, screen_.y, std::round(screen_.w * xp), screen_.h }, res::colour("XpBar")); }
			if (!font) { return; }
			const float pad = std::round(4 * scale_), y = std::round(screen_.y + (screen_.h - font->lineHeight()) / 2);
			const std::string pct = whole(xp * 100) + "%";
			const Color c = res::colour("Main");
			font->draw(screen_.x + pad, y, "Next Level:", c);
			font->draw(screen_.right() - pad - font->width(pct), y, pct, c);
		}
	};

	Stats::Stats(StatsActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		win_ = &root().add<CaptionWindow>("CHARACTER STATS", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 14);
		win_->halign = win_->valign = A::kCenter;
		win_->visible = false;
		win_->pinButton().visible = true;
		empty_ = &win_->body().add<Label>("No stats received yet");
		empty_->halign = empty_->valign = A::kCenter;

		Box& body = win_->body().add<Box>(false, kGap);
		body_ = &body;
		left_ = &body.add<Box>(true, 8);
		Box& top = left_->add<Box>(false, kGap);
		attrInset_ = &section(top, "ATTRIBUTES", kAttrW);
		attrRows_ = &attrInset_->add<Box>(true, 0);
		skillInset_ = &section(top, "SKILLS", kSkillsW);
		Box& cols = skillInset_->add<Box>(false, kColGap);
		for (Box*& c : columns_)
		{
			c = &cols.add<Box>(true, 0);
			c->rect.w = kColW;
		}
		descInset_ = &section(*left_, "DESCRIPTION", kLeftW);
		Box& desc = descInset_->add<Box>(false, 20);
		about_ = &desc.add<RichText>();
		about_->rect.w = kAboutW;
		about_->rowH = kRowH;
		Box& right = desc.add<Box>(true, 4);
		next_ = &right.add<NextLevel>();
		next_->rect = { 0, 0, kTextW, kNextH };
		text_ = &right.add<RichText>();
		text_->rect.w = kTextW;
		text_->rowH = kRowH;
		derivedInset_ = &section(body, "DERIVED STATISTICS", kDerivedW, &derivedHead_);
		derived_ = &derivedInset_->add<RichText>();
		derived_->rect.w = kDerivedW - 2 * kPad;
		refresh();
	}

	Stats::~Stats()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	bool Stats::shown() const { return win_->visible; }
	bool Stats::pinned() const { return win_->pinned(); }

	void Stats::show(bool on)
	{
		if (on != win_->visible) { sound(on ? "Stats_Open" : "Stats_Close"); }	// the original's window ctor / dtor
		win_->visible = on;
		if (on)
		{
			picked_ = -1;
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	Widget* Stats::window() const { return win_; }
	Widget* Stats::derived() const { return derived_; }
	Widget* Stats::about() const { return about_; }
	Widget* Stats::text() const { return text_; }
	std::string Stats::described() const { return shown_ >= 0 ? rows_[shown_].name : std::string(); }

	Widget* Stats::row(const std::string& name) const
	{
		for (Line* l : lines_)
		{
			if (l->name == name) { return l; }
		}
		return nullptr;
	}

	std::string Stats::levelText(const std::string& name) const
	{
		const Widget* w = row(name);
		return w ? static_cast<const Line*>(w)->level : std::string();
	}

	void Stats::update(const StatsState& s)
	{
		const bool changed = !(s == last_);
		if (changed)
		{
			const bool shape = !sameShape(s, last_);
			last_ = s;
			if (shape)
			{
				rebuild();
				picked_ = -1;
			}
			refresh();
		}
		int want = picked_;
		for (size_t i = 0; i < lines_.size(); i++)
		{
			if (hovered(lines_[i])) { want = (int)i; }
		}
		if (changed || want != shown_) { describe(want); }
		fit();
	}

	void Stats::rebuild()
	{
		attrRows_->clear();
		for (Box* c : columns_) { c->clear(); }
		lines_.clear();
		auto line = [this](Box& parent, float w)
		{
			Line& l = parent.add<Line>();
			l.rect = { 0, 0, w, kRowH };
			l.onPick = [this, i = (int)lines_.size()] { picked_ = i; };
			lines_.push_back(&l);
		};
		for (size_t i = 0; i < last_.attributes.size(); i++) { line(*attrRows_, kAttrW - 2 * kPad); }
		for (const StatGroup& g : last_.skills)
		{
			Box& col = *columns_[std::clamp(g.column, 0, 3)];
			if (!g.title.empty())
			{
				Label& h = col.add<Label>("-" + g.title + "-");
				h.halign = Widget::Align::kCenter;
				h.colour = res::colour("StatGroup");
			}
			for (size_t i = 0; i < g.rows.size(); i++) { line(col, kColW); }
		}
	}

	void Stats::refresh()
	{
		const StatsState& s = last_;
		win_->caption().text = s.name.empty() ? "CHARACTER STATS" : s.name;
		empty_->visible = !s.valid;
		body_->visible = s.valid;
		rows_ = s.attributes;
		for (const StatGroup& g : s.skills) { rows_.insert(rows_.end(), g.rows.begin(), g.rows.end()); }
		for (size_t i = 0; i < lines_.size(); i++) { lines_[i]->set(rows_[i]); }
		derived_->text = s.derived;
		derived_->ink = about_->ink = text_->ink = ink();
	}

	void Stats::describe(int i)
	{
		shown_ = i;
		next_->visible = i >= 0;
		if (i < 0)
		{
			about_->text.clear();
			text_->text.clear();
			return;
		}
		const StatRow& r = rows_[i];
		std::string about = "{StatGroup}" + r.name;
		if (modified(r))
		{
			const long d = std::lround(r.level) - std::lround(r.base);
			about += "\t{" + std::string(d > 0 ? "StatUp" : "StatDown") + "}" + whole(r.level) + " (" + (d > 0 ? "+" : "") + std::to_string(d) + ")";
		}
		if (!r.affects.empty()) { about += "\n{StatLabel}Stats Affected:"; }
		for (const std::string& a : r.affects) { about += "\n-" + a; }
		if (!r.training.empty()) { about += "\n{StatLabel}Ways To Train:"; }
		for (const std::string& t : r.training) { about += "\n-" + t; }
		about_->text = about;
		next_->xp = std::clamp(r.xp, 0.0f, 1.0f);
		text_->text = r.description.empty() ? std::string() : "{StatLabel}Description:\n" + r.description;
	}

	void Stats::fit()
	{
		for (Panel* p : { attrInset_, skillInset_, descInset_, derivedInset_ }) { p->rect.h = 0; }
		const float top = std::max({ attrInset_->preferred().h, skillInset_->preferred().h, kTopMinH });
		attrInset_->rect.h = skillInset_->rect.h = top;
		descInset_->rect.h = std::max(descInset_->preferred().h, kDescMinH);
		derivedInset_->rect.h = std::max(derivedInset_->preferred().h, left_->preferred().h - derivedHead_->preferred().h - 4);
	}
}
