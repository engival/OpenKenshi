#include "ui/faction.hpp"

#include <cmath>
#include <cstdio>

#include "ui/overview.hpp"
#include "ui/pagekit.hpp"

namespace ui
{
	namespace
	{
		std::string valueText(float v) { return std::to_string((int)v); }	// the game's (int) cast, toward zero: -11.02 "-11", -0.125 "0"

		Color valueInk(float v)	// the game's thresholds on the raw value
		{
			return v < -30 ? style().colour("RelationBad", { 0.749f, 0.282f, 0.204f, 1 }) : v > 60 ? style().colour("RelationGood", { 0.659f, 0.718f, 0.455f, 1 })
				: style().colour("RelationNeutral", { 0.686f, 0.651f, 0.545f, 1 });
		}

		std::string exact(float v)	// to two places, trailing zeros dropped: "-11.4", "100"
		{
			char b[32];
			snprintf(b, sizeof b, "%.2f", v);
			std::string t = b;
			t.erase(t.find_last_not_of('0') + 1);
			if (t.back() == '.') { t.pop_back(); }
			return t == "-0" ? "0" : t;
		}
		// Kenshi_OverviewWindow.layout's FactionTab, as fractions of the page.
		const Rect kRelations = { 0, 0.0120846f, 0.5f, 0.983384f };
		const Rect kNameHead = { 0.594595f, 0.0468278f, 0.308559f, 0.0256798f }, kName = { 0.601351f, 0.0770393f, 0.308559f, 0.0543807f };
		const Rect kInfoHead = { 0.603604f, 0.146526f, 0.308559f, 0.0256798f }, kInfo = { 0.518018f, 0.170695f, 0.468468f, 0.823263f };
	}

	// The relations, a Kenshi_FactionRelationsLine per faction: a Kenshi_Button1 row 24 high, LeftBar (Kenshi_LifeBarHurt) growing left from the
	// middle below zero, RightBar (Kenshi_LifeBar) right above it, both across the row's middle 58%; FactionName left, FactionValue right.
	class RelationRows : public Widget
	{
	public:
		static constexpr float kRowH = 25.5f, kBoxH = 24;	// the capture's pitch; the line's 0.0222 of 1080
		std::vector<FactionRelation> rows;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");

		RelationRows() { solid = true; }

		Size preferred() const override { return { 0, rows.size() * kRowH }; }

		Rect box(size_t i) const
		{
			const float s = scale_;
			return { std::round(screen_.x), std::round(screen_.y + (i * kRowH + (kRowH - kBoxH) / 2) * s), std::round(screen_.w), std::round(kBoxH * s) };
		}

		Rect bar(size_t i) const	// the line's LeftBar / RightBar: x 0.01 / 0.5, w 0.49, y 0.208333, h 0.583333
		{
			const Rect r = box(i);
			const float half = std::round(r.w * 0.49f), w = std::round(half * std::min(std::abs(rows[i].value), 100.0f) / 100), mid = std::round(r.x + r.w * 0.5f);
			return { rows[i].value < 0 ? mid - w : mid, std::round(r.y + r.h * 0.208333f), w, std::round(r.h * 0.583333f) };
		}

		int at(float x, float y) const
		{
			for (size_t i = 0; i < rows.size(); i++) { if (box(i).contains(x, y)) { return (int)i; } }
			return -1;
		}

		std::string tip() const override
		{
			float x, y;
			mouse(x, y);
			const int i = hovered(this) ? at(x, y) : -1;
			return i >= 0 ? exact(rows[i].value) : "";
		}

	protected:
		void paint() const override
		{
			if (!font) { return; }
			const Color name = style().colour("RelationName", { 0.871f, 0.845f, 0.81f, 1 });
			for (size_t i = 0; i < rows.size(); i++)
			{
				const FactionRelation& row = rows[i];
				const Rect r = box(i);
				style().draw(Look::kButton, r, scale_, State::kNormal, false);
				if (row.value != 0) { style().draw(row.value < 0 ? Look::kBarHurt : Look::kBarFill, bar(i), scale_, State::kNormal, false); }
				const std::string v = valueText(row.value);
				const float ty = std::round(r.y + (r.h - font->lineHeight()) / 2), vw = font->width(v);
				gfx::pushClip(r);
				font->draw(std::round(r.x + r.w * 0.02f), ty, row.name, name);	// FactionName at x 0.02
				font->draw(std::round(r.x + r.w * 0.98f - vw), ty, v, valueInk(row.value));	// FactionValue, right-aligned in 0.98
				gfx::popClip();
			}
		}
	};

	Faction::Faction(Overview& home, FactionActions actions) : home_(home), act_(std::move(actions))
	{
		Widget& body = home_.pageWidget(OverviewPage::kFaction);
		for (const auto& c : body.children()) { c->visible = false; }	// the overview's "Not available yet"
		FractionPage& page = body.add<FractionPage>();
		page.halign = page.valign = Widget::Align::kStretch;
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);

		// RELATIONS: RelationsPanel, a bare scroll view.
		ScrollView& rsv = page.at(kRelations, page.add<ScrollView>());
		rows_ = &rsv.body().add<RelationRows>();
		rows_->halign = Widget::Align::kStretch;

		// FACTION NAME: FactionNameText (32 bytes, order 85's limit in code points (?)), renamed when committed (the host need not debounce).
		page.at(kNameHead, paintedHeading(page, "FACTION NAME", TextAlign::kCenter));
		name_ = &page.at(kName, page.add<EditBox>());
		name_->maxBytes = 32;
		name_->onCommit = [this](const std::string& n) { if (act_.rename) { auto f = act_.rename; f(n); } };

		// INFORMATION: FactionInfoPanel in its inset.
		page.at(kInfoHead, paintedHeading(page, "INFORMATION", TextAlign::kCenter));
		page.at(kInfo, page.add<Panel>(Look::kInset, 0));
		Widget& panel = page.at(inside(kInfo, { 0.03125f, 0.0146789f, 0.9375f, 0.965138f }), page.add<Widget>());	// x: the capture's margin (the layout's 0 leans on the skin)
		panel.clipChildren = true;
		info_ = &panel.add<RichText>();
		info_->halign = Widget::Align::kStretch;
		info_->ink = ink;
		refresh();
	}

	Faction::~Faction()
	{
		if (ready()) { home_.pageWidget(OverviewPage::kFaction).clear(); }
	}

	bool Faction::shown() const { return home_.shown(OverviewPage::kFaction); }

	void Faction::show(bool on)
	{
		if (on) { home_.open(OverviewPage::kFaction); }
		else if (shown()) { home_.show(false); }
	}

	Widget* Faction::relations() const { return rows_; }
	Rect Faction::relationRow(size_t i) const { return i < rows_->rows.size() ? rows_->box(i) : Rect{}; }

	std::string Faction::relationText(size_t i) const { return i < rows_->rows.size() ? valueText(rows_->rows[i].value) : ""; }
	Color Faction::relationColour(size_t i) const { return i < rows_->rows.size() ? valueInk(rows_->rows[i].value) : Color{}; }

	Rect Faction::relationBar(size_t i) const
	{
		if (i >= rows_->rows.size() || rows_->rows[i].value == 0) { return {}; }
		return rows_->bar(i);
	}

	void Faction::update(const FactionState& s)
	{
		const bool now = shown();
		if (wasShown_ && !now && act_.close) { auto f = act_.close; f(); }	// closed, or another page
		wasShown_ = now;
		if (s != last_)
		{
			last_ = s;
			refresh();
		}
		if (!focused(name_) && name_->text != last_.name) { name_->text = last_.name; }	// typing wins while it has the focus
	}

	void Faction::refresh()
	{
		home_.setCaption(OverviewPage::kFaction, last_.caption);
		rows_->rows = last_.relations;
		info_->text = last_.info;
	}
}
