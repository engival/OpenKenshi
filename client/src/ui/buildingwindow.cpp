#include "ui/buildingwindow.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "ui/progressbars.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		constexpr float kBarW = 168;	// the layout's ProgressBars: 112 wide at 720 lines (their height is the style's: Look::kSlotBar)
		constexpr float kBottom = 375;	// units from the screen's bottom to the window's: its bottom edge at 470 of 720 lines
		int slot(int g) { return g - kBuildingIn1; }
	}

	// Input1ProgressBar / OutputProgressBar: the style's height (Look::kSlotBar). A thin bar (flat) is a track and its fill, no frame; a tall one
	// (original) is the skinned Kenshi_ProgressBarFill.
	class ProgressStrip : public Widget
	{
	public:
		float progress = 0;
		mutable Rect fill;
		Size preferred() const override { return { kBarW, style().natural(Look::kSlotBar).h }; }

	protected:
		void paint() const override
		{
			if (style().natural(Look::kSlotBar).h > 12)
			{
				fill = drawProgressBar(screen_, progress, scale_);
				return;
			}
			fill = { screen_.x, screen_.y, std::round(screen_.w * std::clamp(progress, 0.0f, 1.0f)), screen_.h };
			style().draw(Look::kProgressTrack, screen_, scale_, State::kNormal, false);
			if (fill.w > 0) { style().draw(Look::kProgressFill, fill, scale_, State::kNormal, false); }
		}
	};

	BuildingWindow::BuildingWindow(BuildingWindowActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		win_ = &root().add<CaptionWindow>("", [this] { if (act_.close) { auto f = act_.close; f(last_.id); } }, 10);
		win_->halign = A::kStart;
		win_->valign = A::kEnd;
		win_->rect.x = 0;
		win_->rect.y = kBottom;
		win_->visible = false;
		Box& body = win_->body().add<Box>(true, 6);
		const Color ink = style().textColour(Look::kField, State::kNormal, false);

		// CraftingPanel: CraftingName (Kenshi_GenericTextBoxFlat, the item in progress in #afa68b) and QUEUE... (Kenshi_Button1) on its right.
		craft_ = &body.add<Panel>(Look::kSlotBox, 6);
		craft_->halign = A::kStretch;
		craftName_ = &craft_->add<Label>("", "Kenshi_StandardFont_Medium");	// the left two thirds, centred
		craftName_->colour = style().colour("BuildingCrafting", ink);
		craftName_->fit = false;
		craftName_->align = TextAlign::kCenter;
		craftName_->halign = A::kStretch;
		craftName_->valign = A::kCenter;
		queue_ = &craft_->add<Button>("QUEUE...");
		queue_->halign = A::kEnd;
		queue_->valign = A::kCenter;
		queue_->onClicked = [this] { if (act_.queue) { auto f = act_.queue; f(last_.id); } };

		// InputOutputPanel: InputColumn | OutputColumn, each under its title (TitlesPanel's, Kenshi_PaintedTextFont_Medium).
		Box& cols = body.add<Box>(false, 6);
		Box& in = cols.add<Box>(true, 4);
		inCol_ = &in;
		inTitle_ = &in.add<Label>("INPUT", "Kenshi_PaintedTextFont_Medium");
		inTitle_->halign = A::kCenter;
		addSlot(in, kBuildingIn1);
		addSlot(in, kBuildingIn2);
		Box& out = cols.add<Box>(true, 4);
		outCol_ = &out;
		outTitle_ = &out.add<Label>("OUTPUT", "Kenshi_PaintedTextFont_Medium");
		outTitle_->halign = A::kCenter;
		addSlot(out, kBuildingOut);
		grids_[slot(kBuildingOut)]->dropIn = false;	// the game hands a drop on "out" back to its source, unless the host says (outputDropIn)

		// OutputDataPanel (the item furnace's): Total Metals, and Active OFF / ON with its warning.
		furnace_ = &out.add<Panel>(Look::kSlotBox, 8);
		furnace_->halign = A::kStretch;
		Box& fb = furnace_->add<Box>(true, 6);
		Box& metalsRow = fb.add<Box>(false, 6);
		Label& ml = metalsRow.add<Label>("Total Metals:", "Kenshi_StandardFont_Medium");
		ml.colour = ink;
		metals_ = &metalsRow.add<Label>("", "Kenshi_StandardFont_Medium");
		Box& activeRow = fb.add<Box>(false, 6);
		Label& al = activeRow.add<Label>("Active:", "Kenshi_StandardFont_Medium");
		al.colour = ink;
		al.valign = A::kCenter;
		active_ = &activeRow.add<Button>("OFF", Look::kButton, "Kenshi_StandardFont_Medium");	// Kenshi_Button2
		active_->tooltip = "{HudTag}Warning:\nSwitching on will incinerate the entire inventory!";
		active_->onClicked = [this] { if (last_.furnace && act_.active) { auto f = act_.active; f(last_.id, !last_.furnace->active); } };
	}

	BuildingWindow::~BuildingWindow()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	// Input1Panel / Input2Panel / OutputPanel (Kenshi_GenericTextBoxSkin): the name, the status line, the slot and the bar.
	Widget& BuildingWindow::addSlot(Widget& col, int g)
	{
		using A = Widget::Align;
		const int i = slot(g);
		Panel& p = col.add<Panel>(Look::kSlotBox, 8);
		p.halign = A::kStretch;
		panels_[i] = &p;
		Box& b = p.add<Box>(true, 4);
		b.halign = A::kStretch;
		const Color ink = style().textColour(Look::kField, State::kNormal, false);
		names_[i] = &b.add<Label>("", "Kenshi_StandardFont_Medium");
		names_[i]->colour = ink;
		names_[i]->halign = A::kCenter;
		status_[i] = &b.add<Label>("", "Kenshi_StandardFont_Small");
		status_[i]->halign = A::kCenter;
		ItemGrid& grid = b.add<ItemGrid>(0, 0);
		grids_[i] = &grid;
		grid.halign = A::kCenter;
		grid.tag = g;
		grid.clampDrop = true;	// the slot is the footprint: a drop anywhere over it lands where the item fits (kenshi-k16.2.122.1)
		grid.onDrop = [this, g](ItemGrid& from, const GridItem& it, int x, int y) { if (act_.move) { auto f = act_.move; f(last_.id, from.tag, it, g, x, y); } };
		grid.onRefused = [&grid](ItemGrid&, const GridItem& it) { message(grid.blocked || !grid.dropIn || !grid.takes(it) ? "Can't put that there." : "No room for that item."); };
		grid.onItemClick = [this, i, g](int k, Btn btn)
		{
			grids_[i]->selected = k;
			if (btn == Btn::kRight && act_.quickMove) { auto f = act_.quickMove; f(last_.id, g, grids_[i]->items[k]); }
		};
		grid.onDropOutside = [this, g](const GridItem& it, float, float) { if (act_.dropOutside) { auto f = act_.dropOutside; f(last_.id, g, it); } };
		bars_[i] = &b.add<ProgressStrip>();
		bars_[i]->halign = A::kStretch;
		return p;
	}

	bool BuildingWindow::shown() const { return win_->visible; }
	ItemGrid* BuildingWindow::grid(int g) const { return grids_[slot(g)]; }
	Widget* BuildingWindow::column(int g) const { return panels_[slot(g)]; }
	Label* BuildingWindow::nameText(int g) const { return names_[slot(g)]; }
	Label* BuildingWindow::statusText(int g) const { return status_[slot(g)]; }
	Rect BuildingWindow::bar(int g) const { return bars_[slot(g)]->screen(); }
	Rect BuildingWindow::barFill(int g) const { return bars_[slot(g)]->fill; }
	Widget* BuildingWindow::craftingPanel() const { return craft_; }
	Widget* BuildingWindow::furnacePanel() const { return furnace_; }

	void BuildingWindow::fill(int g, const BuildingSlot* s)
	{
		const int i = slot(g);
		panels_[i]->visible = s != nullptr;
		if (!s) { return; }
		const Color ink = style().textColour(Look::kField, State::kNormal, false);
		names_[i]->text = s->name;
		status_[i]->text = s->status;
		status_[i]->visible = !s->status.empty();
		status_[i]->colour = s->statusColour.empty() ? ink : style().colour(s->statusColour, ink);
		ItemGrid& grid = *grids_[i];
		if (grid.cols != s->cols || grid.rows != s->rows || grid.items != s->items) { grid.selected = -1; }
		grid.cols = s->cols;
		grid.rows = s->rows;
		grid.accepts = s->accepts;
		grid.items = s->items;
		bars_[i]->progress = s->bar;
	}

	void BuildingWindow::update(const BuildingWindowState& s)
	{
		if (s == last_) { return; }
		if (s.id != last_.id && s.valid) { win_->raise(); }
		last_ = s;
		win_->visible = s.valid;
		std::string title = s.name;
		for (char& c : title) { c = (char)toupper((unsigned char)c); }
		win_->caption().text = title;
		const size_t n = std::min<size_t>(s.inputs.size(), 2);
		inCol_->visible = n > 0;
		inTitle_->text = n > 1 ? "INPUTS" : "INPUT";
		fill(kBuildingIn1, n > 0 ? &s.inputs[0] : nullptr);
		fill(kBuildingIn2, n > 1 ? &s.inputs[1] : nullptr);
		outCol_->visible = s.output.has_value() || s.furnace.has_value();
		fill(kBuildingOut, s.output ? &*s.output : nullptr);
		grids_[slot(kBuildingOut)]->dropIn = s.outputDropIn;
		craft_->visible = s.crafting.has_value();
		if (s.crafting) { craftName_->text = s.crafting->name; }
		furnace_->visible = s.furnace.has_value();
		if (s.furnace)
		{
			const Color ink = style().textColour(Look::kField, State::kNormal, false);
			metals_->text = s.furnace->metals;
			metals_->colour = style().colour(s.furnace->ok ? "HudGood" : "HudBad", ink);
			active_->text = s.furnace->active ? "ON" : "OFF";
			active_->textColour = style().colour(s.furnace->active ? "HudGood" : "HudNeutral", ink);
		}
	}
}
