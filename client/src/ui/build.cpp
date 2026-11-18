#include "ui/build.hpp"

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const Rect kHammer = { 872, 763, 72, 52 };	// Kenshi_BuildButtonSkin's normal state, as the hammer's icon (our button draws the face)

		Label& heading(Widget& parent, const char* text, const Rect& r)	// Kenshi_TextboxPaintedText, centred
		{
			Label& l = parent.add<Label>(text, "Kenshi_PaintedTextFont_Medium");
			l.fit = false;
			l.align = TextAlign::kCenter;
			l.rect = r;
			return l;
		}

		// A list in an inset (Kenshi_ListBoxEmpty: parchment in the original), scrolling: plain rows in the inset's ink, no hover, the picked one
		// light grey (style BuildPicked).
		List& list(Widget& parent, const Rect& r)
		{
			Panel& box = parent.add<Panel>(Look::kInset, 6);
			box.rect = r;
			ScrollView& sv = box.add<ScrollView>();
			sv.halign = sv.valign = Widget::Align::kStretch;
			List& l = sv.body().add<List>();
			l.rect.w = r.w - 2 * 6 - 8;
			l.padY = 2;
			l.colour = l.hoverColour = style().textColour(Look::kInset, State::kNormal, false);
			l.selectedColour = style().colour("BuildPicked", l.selectedColour);
			return l;
		}
	}

	Build::Build(BuildActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		// MainPanel (Kenshi_FloatingPanelSkin), bottom left; positions: kinspect's live boxes (720 lines) x 1.5, from the panel's top-left.
		main_ = &root().add<Window>(Look::kPanel, 0);
		main_->valign = A::kEnd;
		main_->rect = { 0, 0, kW, kH };
		main_->visible = false;
		main_->onEscape = [this] { if (act_.close) { auto f = act_.close; f(); } };
		heading(*main_, "CATEGORIES", { 9, 7.5f, 178.5f, 22.5f });
		cats_ = &list(*main_, { 9, 30, 178.5f, 265.5f });
		cats_->onPick = [this](int i)
		{
			if (i < 0 || i >= (int)last_.categories.size()) { return; }
			sound("Building_Navigate");
			if (act_.category) { auto f = act_.category; f(last_.categories[i].id); }
		};
		heading(*main_, "BUILDINGS", { 199.5f, 7.5f, 339, 22.5f });
		list_ = &list(*main_, { 199.5f, 30, 339, 265.5f });
		list_->onPick = [this](int i)
		{
			if (i < 0 || i >= (int)last_.buildings.size()) { return; }
			// The original skips Building_Click without a select sound (1405504a0); then the list's Building_Navigate (deferred: k16.3.17).
			const std::string& s = last_.buildings[i].selectSound;
			if (s.empty()) { sound("Building_Navigate"); }
			else { sound("Building_Click", "Buildings_Click", s); }
			if (act_.pick) { auto f = act_.pick; f(last_.buildings[i].id); }
		};

		// BuildingTypePanel: < name >, the arrows only with variants.
		Widget& type = main_->add<Widget>();
		type.rect = { 549, 9, 508.5f, 39 };
		prev_ = &type.add<Button>("", Look::kArrowLeft);
		prev_->rect = { 0, 0, 28.5f, 39 };
		prev_->sound = "Building_Navigate";
		prev_->onClicked = [this] { if (act_.step) { act_.step(-1); } };
		next_ = &type.add<Button>("", Look::kArrowRight);
		next_->rect = { 477, 0, 28.5f, 39 };
		next_->sound = "Building_Navigate";
		next_->onClicked = [this] { if (act_.step) { act_.step(1); } };
		Panel& plate = type.add<Panel>(Look::kField, 0);
		plate.rect = { 30, 3, 439.5f, 33 };	// 293 x 17 at 720, taller for our font
		title_ = &plate.add<Label>("", "Kenshi_StandardFont_Medium");	// BuildingName
		title_->halign = title_->valign = A::kStretch;
		title_->fit = false;
		title_->align = TextAlign::kCenter;
		title_->colour = style().textColour(Look::kField, State::kNormal, false);

		Panel& pic = main_->add<Panel>(Look::kInset, 0);	// BuildingImage
		pic.rect = { 546, 49.5f, 133.5f, 133.5f };
		image_ = &pic.add<TextureImage>();
		image_->rect = { 9, 9, 112.5f, 112.5f };

		Panel& stats = main_->add<Panel>(Look::kInset, 0);	// StatsPanel; its lines 228 of 251 at 720, centred
		stats.rect = { 676.5f, 49.5f, 387, 160.5f };
		stats_ = &stats.add<Box>(true, 0);
		stats_->rect = { 25.5f, 6, 342, 151.5f };

		Panel& desc = main_->add<Panel>(Look::kInset, 9);	// DescriptionText
		desc.rect = { 546, 208.5f, 517.5f, 87 };
		infoScroll_ = &desc.add<ScrollView>();
		infoScroll_->halign = infoScroll_->valign = A::kStretch;
		info_ = &infoScroll_->body().add<RichText>();
		info_->rect.w = 517.5f - 2 * 9 - 8;
		info_->ink = style().textColour(Look::kInset, State::kNormal, false);

		// ButtonPanel, bottom right over the commands.
		buttons_ = &root().add<Panel>(Look::kPanel, 0);
		buttons_->halign = buttons_->valign = A::kEnd;
		buttons_->rect = { 0, 265.5f, 285, 73.5f };
		buttons_->solid = true;
		buttons_->visible = false;
		confirm_ = &buttons_->add<Button>("CONFIRM", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		confirm_->rect = { 9, 12, 103.5f, 48 };
		confirm_->sound = "Building_Confirm";
		confirm_->onClicked = [this] { if (act_.confirm) { act_.confirm(); } };
		undo_ = &buttons_->add<Button>("UNDO", Look::kButton, "Kenshi_PaintedTextFont_Medium");
		undo_->rect = { 124.5f, 19.5f, 69, 34.5f };
		undo_->sound = "Building_Undo";
		undo_->onClicked = [this] { if (act_.undo) { act_.undo(); } };
		build_ = &buttons_->add<Button>("", Look::kButton);
		build_->icon = kHammer;
		build_->rect = { 207, 10.5f, 72, 52.5f };
		build_->tooltip = "Build";
		build_->sound = "Building_Button";
		build_->onClicked = [this] { if (act_.build) { act_.build(); } };

		// CommandsPanel, bottom right.
		commandsBox_ = &root().add<Panel>(Look::kPanel, 0);
		commandsBox_->halign = commandsBox_->valign = A::kEnd;
		commandsBox_->rect = { 0, 0, 283.5f, 259.5f };
		commandsBox_->solid = true;
		commandsBox_->visible = false;
		heading(*commandsBox_, "COMMANDS", { 0, 7.5f, 283.5f, 22.5f });
		Panel& cmd = commandsBox_->add<Panel>(Look::kInset, 8);	// CommandsTextBox
		cmd.rect = { 4.5f, 28.5f, 274.5f, 223.5f };
		commands_ = &cmd.add<RichText>();
		commands_->rect.w = 274.5f - 2 * 8;
		commands_->ink = style().textColour(Look::kInset, State::kNormal, false);
	}

	Build::~Build()
	{
		if (!ready()) { return; }
		show(false);	// the message box back in its place
		for (Widget* w : { (Widget*)main_, (Widget*)buttons_, (Widget*)commandsBox_ }) { if (w) { root().remove(w); } }
	}

	Widget* Build::mainPanel() const { return main_; }
	Widget* Build::buttonPanel() const { return buttons_; }
	Widget* Build::commandsPanel() const { return commandsBox_; }
	Label* Build::statLabel(size_t i) const { return i < last_.stats.size() ? lines_[i].first : nullptr; }
	Label* Build::statValue(size_t i) const { return i < last_.stats.size() ? lines_[i].second : nullptr; }
	bool Build::shown() const { return shown_; }

	void Build::show(bool on)
	{
		if (shown_ == on) { return; }
		shown_ = on;
		if (HudMessage* m = messageBox())	// MessagePanel: the box the message showed in live, [4,461,458,44] at 720
		{
			if (on)
			{
				messageRect_ = m->rect;
				messageH_ = m->halign;
				messageV_ = m->valign;
				m->halign = Widget::Align::kStart;
				m->valign = Widget::Align::kEnd;
				m->rect = { 6, 322.5f, 687, 66 };
			}
			else
			{
				m->rect = messageRect_;
				m->halign = messageH_;
				m->valign = messageV_;
			}
		}
		refresh();
	}

	void Build::update(const BuildState& s)
	{
		if (s == last_) { return; }
		const bool newPick = s.picked != last_.picked;
		last_ = s;
		auto fill = [](List& l, const std::vector<BuildEntry>& entries, const std::string& sel)
		{
			l.items.clear();
			l.selected = -1;
			for (size_t i = 0; i < entries.size(); i++)
			{
				l.items.push_back(entries[i].name);
				if (entries[i].id == sel) { l.selected = (int)i; }
			}
		};
		fill(*cats_, s.categories, s.category);
		fill(*list_, s.buildings, s.picked);
		title_->text = s.title;
		prev_->visible = next_->visible = s.variants;
		undo_->enabled = s.canUndo;
		image_->set(s.image);
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);
		while (lines_.size() < s.stats.size())
		{
			Widget& line = stats_->add<Widget>();
			line.halign = Widget::Align::kStretch;
			line.rect.h = kLineH;
			Label& l = line.add<Label>("", "Kenshi_StandardFont_Medium");
			l.valign = Widget::Align::kCenter;
			Label& v = line.add<Label>("", "Kenshi_StandardFont_Medium");
			v.halign = Widget::Align::kEnd;
			v.valign = Widget::Align::kCenter;
			lines_.push_back({ &l, &v });
		}
		for (size_t i = 0; i < lines_.size(); i++)
		{
			const bool on = i < s.stats.size();
			lines_[i].first->parent()->visible = on;
			if (!on) { continue; }
			const Color c = s.stats[i].colour.a > 0 ? s.stats[i].colour : ink;
			lines_[i].first->text = s.stats[i].label;
			lines_[i].first->rect.x = s.stats[i].indent ? kIndent : 0;
			lines_[i].second->text = s.stats[i].value;
			lines_[i].first->colour = lines_[i].second->colour = c;
		}
		info_->text = s.description;
		if (newPick) { infoScroll_->offset = 0; }
		commands_->text = s.commands;
		refresh();
	}

	void Build::refresh()
	{
		const bool wasMain = main_->visible;
		main_->visible = shown_ && last_.chooser;
		buttons_->visible = commandsBox_->visible = shown_ && last_.placing;
		if (main_->visible && !wasMain)
		{
			main_->raise();
			focus(main_);
		}
		else if (!main_->visible && focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}
}
