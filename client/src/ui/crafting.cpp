#include "ui/crafting.hpp"

#include "ui/overview.hpp"
#include "ui/pagekit.hpp"

namespace ui
{
	namespace
	{
		// Kenshi_OverviewWindow.layout's CraftTab, as fractions of the page: the insets, and inside them (converted) their panels.
		const Rect kStationsHead = { 0, 0.0120846f, 0.306306f, 0.0256798f }, kStations = { 0, 0.0317221f, 0.317568f, 0.593656f };
		const Rect kItemsHead = { 0.335586f, 0.0120846f, 0.306306f, 0.0256798f }, kItems = { 0.328829f, 0.0317221f, 0.317568f, 0.593656f };
		const Rect kQueueHead = { 0.675676f, 0.0120846f, 0.306306f, 0.0256798f }, kQueue = { 0.668919f, 0.0317221f, 0.317568f, 0.564955f };
		const Rect kRepeat = { 0.675676f, 0.593656f, 0.306306f, 0.0317221f };
		const Rect kDescHead = { 0, 0.643505f, 0.644144f, 0.0256798f }, kDesc = { 0, 0.664653f, 0.995495f, 0.330816f };
	}

	Crafting::Crafting(Overview& home, CraftingActions actions) : home_(home), act_(std::move(actions))
	{
		Widget& body = home_.pageWidget(OverviewPage::kCrafting);
		FractionPage& page = body.add<FractionPage>();
		page.halign = page.valign = Widget::Align::kStretch;
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);

		page.at(kStationsHead, paintedHeading(page, "STATIONS", TextAlign::kCenter));
		page.at(kItemsHead, paintedHeading(page, "ITEMS", TextAlign::kCenter));
		page.at(kQueueHead, paintedHeading(page, "QUEUE", TextAlign::kCenter));
		page.at(kDescHead, paintedHeading(page, "DESCRIPTION", TextAlign::kStart));

		// STATIONS (CraftingStationsList, Kenshi_ListBoxEmpty, in an inset as the research TYPES).
		Panel& stationsBox = page.at(kStations, page.add<Panel>(Look::kInset, 6));
		ScrollView& ssv = stationsBox.add<ScrollView>();
		ssv.halign = ssv.valign = Widget::Align::kStretch;
		stations_ = &ssv.body().add<List>();
		stations_->halign = Widget::Align::kStretch;
		stations_->padY = 2;
		stations_->colour = stations_->hoverColour = ink;
		stations_->selectedColour = style().colour("ResearchName", stations_->selectedColour);
		stations_->onPick = [this](int i) { if (i >= 0 && i < (int)last_.stations.size() && act_.station) { auto f = act_.station; f(last_.stations[i].id); } };

		// ITEMS: QueueItemsPanel in its inset.
		page.at(kItems, page.add<Panel>(Look::kInset, 0));
		ScrollView& isv = page.at(inside(kItems, { 0, 0.0254453f, 0.93617f, 0.931298f }), page.add<ScrollView>());
		items_ = &isv.body().add<ButtonRows>();
		items_->halign = Widget::Align::kStretch;
		items_->onPick = [this](int i)
		{
			clicked_ = items_->rows[i].id;
			clickedRow_ = -1;
			describe(clicked_, -1);
			if (act_.queue) { auto f = act_.queue; f(last_.station, clicked_); }
		};
		empty_ = &page.at(inside(kItems, { 0.0425532f, 0.0254453f, 0.893617f, 0.3f }), page.add<Label>());	// the list's single line when it is empty
		empty_->wrap = true;
		empty_->fit = false;
		empty_->align = TextAlign::kCenter;
		empty_->vAlign = TextAlign::kStart;
		empty_->colour = ink;

		// QUEUE: CraftQueue in its inset; REPEAT QUEUE (Kenshi_TickButton1) under it.
		page.at(kQueue, page.add<Panel>(Look::kInset, 0));
		ScrollView& qsv = page.at(inside(kQueue, { 0.0545455f, 0.0375f, 0.909091f, 0.9375f }), page.add<ScrollView>());
		queue_ = &qsv.body().add<ButtonRows>();
		queue_->halign = Widget::Align::kStretch;
		queue_->buttonH = 30;	// as research's queue
		queue_->draggable = queue_->removable = true;
		queue_->onPick = [this](int i)
		{
			clicked_ = queue_->rows[i].id;
			clickedRow_ = i;
			describe(clicked_, i);
		};
		queue_->onRight = queue_->onRemove = [this](int i) { if (act_.unqueue) { auto f = act_.unqueue; f(last_.station, i); } };
		queue_->onMove = [this](int from, int to) { if (act_.move) { auto f = act_.move; f(last_.station, from, to); } };
		repeat_ = &page.at(kRepeat, page.add<Button>("REPEAT QUEUE", Look::kToggle, "Kenshi_StandardFont_MediumSmall"));
		repeat_->checkable = true;
		repeat_->onToggle = [this](bool on)
		{
			repeat_->checked = !on;	// the host's state decides
			if (station() && act_.repeat) { auto f = act_.repeat; f(last_.station, on); }
		};

		// DESCRIPTION: two panels in an inset (QueueDescriptionPanel1 / 2); the details left, the text right.
		page.at(kDesc, page.add<Panel>(Look::kInset, 0));
		Widget& left = page.at(inside(kDesc, { 0.00226244f, 0.0365297f, 0.443439f, 0.926941f }), page.add<Widget>());
		left.clipChildren = true;
		details_ = &left.add<RichText>();
		details_->halign = Widget::Align::kStretch;
		details_->ink = ink;
		Widget& right = page.at(inside(kDesc, { 0.468326f, 0.0365297f, 0.50905f, 0.913242f }), page.add<Widget>());
		right.clipChildren = true;
		text_ = &right.add<RichText>();
		text_->halign = Widget::Align::kStretch;
		text_->ink = ink;
		refresh();
	}

	Crafting::~Crafting()
	{
		if (ready()) { home_.pageWidget(OverviewPage::kCrafting).clear(); }
	}

	bool Crafting::shown() const { return home_.shown(OverviewPage::kCrafting); }

	void Crafting::show(bool on)
	{
		if (on) { home_.open(OverviewPage::kCrafting); }
		else if (shown()) { home_.show(false); }
	}

	Widget* Crafting::items() const { return items_; }
	Widget* Crafting::queueList() const { return queue_; }
	Rect Crafting::itemRow(size_t i) const { return i < items_->rows.size() ? items_->button(i) : Rect{}; }
	Rect Crafting::queueRow(size_t i) const { return i < queue_->rows.size() ? queue_->button(i) : Rect{}; }

	std::vector<std::string> Crafting::listed() const
	{
		std::vector<std::string> ids;
		for (const ButtonRows::Row& r : items_->rows) { ids.push_back(r.id); }
		return ids;
	}

	const CraftStation* Crafting::station() const
	{
		for (const CraftStation& st : last_.stations) { if (st.id == last_.station) { return &st; } }
		return nullptr;
	}

	const CraftRecipe* Crafting::recipe(const std::string& id) const
	{
		if (const CraftStation* st = station())
		{
			for (const CraftRecipe& r : st->recipes) { if (r.id == id) { return &r; } }
		}
		return nullptr;
	}

	void Crafting::update(const CraftingState& s)
	{
		const bool now = shown();
		if (wasShown_ && !now && act_.close) { auto f = act_.close; f(); }	// closed, or another page
		wasShown_ = now;
		if (s != last_)
		{
			last_ = s;
			refresh();
		}
		// The description: the hovered recipe or queue row, else the last clicked one (while listed or queued), else the queue's first.
		const int hi = items_->hot(), hq = queue_->hot();
		std::string id;
		int row = -1;
		if (hi >= 0) { id = items_->rows[hi].id; }
		else if (hq >= 0) { id = queue_->rows[hq].id; row = hq; }
		else if (clickedRow_ < 0 && std::any_of(items_->rows.begin(), items_->rows.end(), [&](const ButtonRows::Row& w) { return w.id == clicked_; })) { id = clicked_; }
		else if (clickedRow_ >= 0 && clickedRow_ < (int)queue_->rows.size() && queue_->rows[clickedRow_].id == clicked_) { id = clicked_; row = clickedRow_; }
		else if (!queue_->rows.empty()) { id = queue_->rows.front().id; row = 0; }
		if (id != described_ || row != describedRow_) { describe(id, row); }
	}

	void Crafting::refresh()
	{
		home_.setCaption(OverviewPage::kCrafting, "");
		const Color blocked = style().colour("ResearchBlocked", { 0.9f, 0.32f, 0.22f, 1 });
		stations_->items.clear();
		stations_->itemColours.clear();
		stations_->itemTips.clear();
		stations_->selected = -1;
		for (size_t i = 0; i < last_.stations.size(); i++)
		{
			const CraftStation& st = last_.stations[i];
			stations_->items.push_back(st.name);
			stations_->itemColours.push_back(st.ok ? Color{ 0, 0, 0, 0 } : blocked);
			stations_->itemTips.push_back(st.status);
			if (st.id == last_.station) { stations_->selected = (int)i; }
		}
		items_->rows.clear();
		queue_->rows.clear();
		const CraftStation* st = station();
		repeat_->enabled = st != nullptr;
		repeat_->checked = st && st->repeat;
		if (st)
		{
			for (const CraftRecipe& r : st->recipes) { items_->rows.push_back({ .id = r.id, .name = r.name }); }
			for (const CraftQueued& q : st->queue)
			{
				const CraftRecipe* r = recipe(q.recipe);
				queue_->rows.push_back({ .id = q.recipe, .name = r ? r->name : q.recipe, .progress = q.progress });
			}
		}
		empty_->text = last_.empty;
		empty_->visible = st && st->recipes.empty();
		describe(described_, describedRow_);
	}

	// The two panels as rich text: a "label\tvalue" line per CraftLine, the value in its palette colour. A queue row: the recipe's left, its own right.
	void Crafting::describe(const std::string& id, int row)
	{
		const CraftRecipe* r = recipe(id);
		const CraftStation* st = station();
		if (!r || (row >= 0 && (!st || row >= (int)st->queue.size())))
		{
			described_.clear();
			describedRow_ = -1;
			details_->text.clear();
			text_->text.clear();
			return;
		}
		described_ = id;
		describedRow_ = row;
		const auto lines = [](const std::vector<CraftLine>& v)
		{
			std::string t;
			for (const CraftLine& l : v) { t += l.value.empty() ? l.label + "\n" : l.label + "\t" + (l.colour.empty() ? "" : "{" + l.colour + "}") + l.value + "\n"; }
			return t;
		};
		details_->text = lines(r->stats);
		text_->text = lines(row >= 0 ? st->queue[row].info : r->info);
	}
}
