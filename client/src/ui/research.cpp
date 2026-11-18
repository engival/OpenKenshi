#include "ui/research.hpp"

#include <algorithm>
#include <cmath>

#include "ui/overview.hpp"
#include "ui/pagekit.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Kenshi_OverviewWindow.layout's ResearchTab, as fractions of the page: the insets, and inside them (converted) their panels.
		const Rect kTypesHead = { 0, 0.0196375f, 0.168919f, 0.0256798f }, kTypes = { 0, 0.0407855f, 0.18018f, 0.351964f };
		const Rect kBlueHead = { 0.191441f, 0.0196375f, 0.382883f, 0.0256798f }, kBlue = { 0.186937f, 0.0407855f, 0.394144f, 0.57855f };
		const Rect kQueueHead = { 0.603604f, 0.0196375f, 0.376126f, 0.0256798f }, kQueue = { 0.596847f, 0.0407855f, 0.38964f, 0.57855f };
		const Rect kDescHead = { 0, 0.643505f, 0.490991f, 0.0256798f }, kDesc = { 0, 0.664653f, 0.995495f, 0.330816f };
	}

	Research::Research(Overview& home, ResearchActions actions) : home_(home), act_(std::move(actions))
	{
		Widget& body = home_.pageWidget(OverviewPage::kResearch);
		FractionPage& page = body.add<FractionPage>();
		page.halign = page.valign = Widget::Align::kStretch;
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);

		page.at(kTypesHead, paintedHeading(page, "TYPES", TextAlign::kCenter));
		page.at(kBlueHead, paintedHeading(page, "BLUEPRINTS", TextAlign::kCenter));
		page.at(kQueueHead, paintedHeading(page, "QUEUE", TextAlign::kCenter));
		page.at(kDescHead, paintedHeading(page, "DESCRIPTION", TextAlign::kStart));

		// TYPES (Kenshi_ListBoxEmpty in an inset, as the build window's lists): plain rows, the picked one gold.
		Panel& typesBox = page.at(kTypes, page.add<Panel>(Look::kInset, 6));
		ScrollView& tsv = typesBox.add<ScrollView>();
		tsv.halign = tsv.valign = Widget::Align::kStretch;
		types_ = &tsv.body().add<List>();
		types_->halign = Widget::Align::kStretch;
		types_->padY = 2;
		types_->colour = types_->hoverColour = ink;
		types_->selectedColour = style().colour("ResearchName", types_->selectedColour);
		types_->onPick = [this](int i)
		{
			if (i < 0 || i >= (int)last_.categories.size()) { return; }
			sound("Research_Tab", "Research_Tabs", last_.categories[i].name);
			if (act_.category) { auto f = act_.category; f(last_.categories[i].id); }
		};

		// BLUEPRINTS: BlueprintsAvailablePanel in its inset.
		page.at(kBlue, page.add<Panel>(Look::kInset, 0));
		ScrollView& bsv = page.at(inside(kBlue, { 0.00571429f, 0.0261097f, 0.925714f, 0.932115f }), page.add<ScrollView>());
		blue_ = &bsv.body().add<ButtonRows>();
		blue_->halign = Widget::Align::kStretch;
		blue_->buttonH = 30;	// 185 x 20 at a 23 pitch (720p)
		blue_->onPick = [this](int i)
		{
			const std::string id = blue_->rows[i].id;
			clicked_ = id;
			describe(id);
			const ResearchEntry* e = entry(id);
			if (e && e->state == ResearchEntry::State::kAvailable && act_.queue) { auto f = act_.queue; f(id); }
		};

		// QUEUE: ResearchQueueList, the bench message over it.
		page.at(kQueue, page.add<Panel>(Look::kInset, 0));
		ScrollView& qsv = page.at(inside(kQueue, { 0.0447761f, 0.0365854f, 0.910448f, 0.926829f }), page.add<ScrollView>());
		queue_ = &qsv.body().add<ButtonRows>();
		queue_->halign = Widget::Align::kStretch;
		queue_->buttonH = 30;	// the ItemBox's cell (?): as a blueprint's
		queue_->draggable = queue_->removable = true;
		queue_->onPick = [this](int i)
		{
			clicked_ = queue_->rows[i].id;
			describe(clicked_);
		};
		queue_->onRight = queue_->onRemove = [this](int i) { if (act_.unqueue) { auto f = act_.unqueue; f(queue_->rows[i].id); } };
		queue_->onMove = [this](int from, int to) { if (act_.move) { auto f = act_.move; f(queue_->rows[from].id, to); } };
		bench_ = &page.at(inside(kQueue, { 0.0895522f, 0.170732f, 0.80597f, 0.231707f }), page.add<Label>());	// ResearchBenchMessage
		bench_->wrap = true;
		bench_->fit = false;
		bench_->align = TextAlign::kCenter;
		bench_->vAlign = TextAlign::kStart;
		bench_->colour = ink.alpha(0.5f);

		// DESCRIPTION: two panels in an inset; the details (label / value lines) left, the text right.
		page.at(kDesc, page.add<Panel>(Look::kInset, 0));
		Widget& left = page.at(inside(kDesc, { 0.00678733f, 0.0365297f, 0.479638f, 0.913242f }), page.add<Widget>());
		left.clipChildren = true;
		details_ = &left.add<RichText>();
		details_->halign = Widget::Align::kStretch;
		details_->ink = ink;
		Widget& right = page.at(inside(kDesc, { 0.50905f, 0.0365297f, 0.468326f, 0.913242f }), page.add<Widget>());
		right.clipChildren = true;
		text_ = &right.add<RichText>();
		text_->halign = Widget::Align::kStretch;
		text_->ink = ink;
		refresh();
	}

	Research::~Research()
	{
		if (ready()) { home_.pageWidget(OverviewPage::kResearch).clear(); }
	}

	bool Research::shown() const { return home_.shown(OverviewPage::kResearch); }

	void Research::show(bool on)
	{
		if (on) { home_.open(OverviewPage::kResearch); }
		else if (shown()) { home_.show(false); }
	}

	Widget* Research::blueprints() const { return blue_; }
	Widget* Research::queueList() const { return queue_; }
	Rect Research::blueprintRow(size_t i) const { return i < blue_->rows.size() ? blue_->button(i) : Rect{}; }
	Rect Research::queueRow(size_t i) const { return i < queue_->rows.size() ? queue_->button(i) : Rect{}; }

	std::vector<std::string> Research::listed() const
	{
		std::vector<std::string> ids;
		for (const ButtonRows::Row& r : blue_->rows) { ids.push_back(r.id); }
		return ids;
	}

	const ResearchEntry* Research::entry(const std::string& id) const
	{
		for (const ResearchEntry& e : last_.entries) { if (e.id == id) { return &e; } }
		return nullptr;
	}

	void Research::update(const ResearchState& s)
	{
		const bool now = shown();
		if (wasShown_ && !now && act_.close) { auto f = act_.close; f(); }	// closed, or another page
		wasShown_ = now;
		if (s != last_)
		{
			last_ = s;
			refresh();
		}
		// The description: the hovered blueprint, else the last clicked entry (while listed or queued), else the queue's first.
		const auto showing = [](const ButtonRows& r, const std::string& id) { return std::any_of(r.rows.begin(), r.rows.end(), [&](const ButtonRows::Row& w) { return w.id == id; }); };
		const int h = blue_->hot();
		const std::string id = h >= 0 ? blue_->rows[h].id : showing(*blue_, clicked_) || showing(*queue_, clicked_) ? clicked_ : !last_.queue.empty() ? last_.queue.front() : "";
		if (id != described_) { describe(id); }
	}

	void Research::refresh()
	{
		home_.setCaption(OverviewPage::kResearch, last_.caption);
		types_->items.clear();
		types_->selected = -1;
		for (size_t i = 0; i < last_.categories.size(); i++)
		{
			types_->items.push_back(last_.categories[i].name);
			if (last_.categories[i].id == last_.category) { types_->selected = (int)i; }
		}
		const Color blocked = style().colour("ResearchBlocked", { 0.9f, 0.32f, 0.22f, 1 });
		blue_->rows.clear();
		for (const ResearchEntry& e : last_.entries)
		{
			const bool queued = std::find(last_.queue.begin(), last_.queue.end(), e.id) != last_.queue.end();
			if (e.category != last_.category || e.state == ResearchEntry::State::kDone || queued) { continue; }
			ButtonRows::Row r = { .id = e.id, .name = e.name, .right = e.time, .progress = e.progress };
			if (e.state == ResearchEntry::State::kLocked)
			{
				r.ink = blocked;
				for (const ResearchNeed& n : e.costs) { if (n.state == ResearchNeed::State::kMissing) { r.tip += (r.tip.empty() ? "" : "\n") + std::string("Missing: ") + n.text; } }
				for (const ResearchNeed& n : e.needs) { if (n.state == ResearchNeed::State::kMissing) { r.tip += (r.tip.empty() ? "" : "\n") + std::string("Requires: ") + n.text; } }
			}
			blue_->rows.push_back(r);
		}
		queue_->rows.clear();
		for (size_t i = 0; i < last_.queue.size(); i++)
		{
			const ResearchEntry* e = entry(last_.queue[i]);
			ButtonRows::Row r = { .id = last_.queue[i], .name = e ? e->name : last_.queue[i], .right = e ? e->time : "", .progress = e ? e->progress : 0 };
			queue_->rows.push_back(r);
		}
		bench_->text = last_.benchMessage;
		describe(described_);
	}

	// The details as rich text: "label\tvalue" rows, "{Colour}" for the name, the warning, the needs by state and the unlocks.
	void Research::describe(const std::string& id)
	{
		described_ = id;
		const ResearchEntry* e = entry(id);
		if (!e)
		{
			described_.clear();
			details_->text.clear();
			text_->text.clear();
			return;
		}
		std::string t = "{ResearchName}" + e->name + "\tlvl " + std::to_string(e->level) + "\n";
		if (!e->eta.empty()) { t += "ETA:\t" + e->eta + "\n"; }
		if (!e->warning.empty()) { t += "{ResearchMissing}" + e->warning + "\n"; }
		const auto ink = [](ResearchNeed::State s, const char* met) { return s == ResearchNeed::State::kMissing ? "{ResearchMissing}" : s == ResearchNeed::State::kQueued ? "{ResearchGood}" : met; };
		for (size_t i = 0; i < e->costs.size(); i++) { t += std::string(i == 0 ? "Cost:" : "") + "\t" + ink(e->costs[i].state, "{ResearchGood}") + e->costs[i].text + "\n"; }
		t += "Requirements:\t";
		for (size_t i = 0; i < e->needs.size(); i++) { t += (i == 0 ? "" : ", ") + std::string(ink(e->needs[i].state, "{ResearchDim}")) + e->needs[i].text; }
		t += e->needs.empty() ? "-\n" : "\n";
		for (const ResearchUnlock& u : e->unlocks) { t += u.label + "\t{ResearchUnlock}" + u.text + "\n"; }
		details_->text = t;
		text_->text = e->description;
	}
}
