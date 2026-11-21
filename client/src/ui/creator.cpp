#include "ui/creator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Column widths (units); the preview fills the screen between them, 20 from each.
		const float kLeftW = 460, kRightW = 580, kMargin = 40;
		const float kCaptionW = 110;
		const float kRowH = 36;
		const float kBar = 6;	// a scroll view's bar

		const char* kTabs[3] = { "BODY", "FACE", "HAIR" };	// CreatorPage order

		Label& caption(Box& row, const std::string& text, float w = kCaptionW, const char* font = "Kenshi_StandardFont_Medium")
		{
			Label& l = row.add<Label>(text, font);
			l.fit = false;
			l.rect = { 0, 0, w, kRowH };
			l.colour = res::colour("Main").alpha(0.7f);
			return l;
		}

		Label& heading(Box& col, const std::string& text)
		{
			Label& l = col.add<Label>(text, "Kenshi_PaintedTextFont_Medium");
			l.colour = res::colour("Main").alpha(0.7f);
			return l;
		}

		Box& pageBody(Widget* page) { return static_cast<Box&>(static_cast<ScrollView*>(page)->body()); }

		// The sliders' layout key: rows are rebuilt only when it changes, never for a value change (a slider may be held).
		bool sameRows(const std::vector<CreatorSlider>& a, const std::vector<CreatorSlider>& b)
		{
			if (a.size() != b.size()) { return false; }
			for (size_t i = 0; i < a.size(); i++)
			{
				if (a[i].category != b[i].category || a[i].name != b[i].name || a[i].min != b[i].min || a[i].max != b[i].max || a[i].step != b[i].step) { return false; }
			}
			return true;
		}

		// Whole numbers for an integer choice or a wide range (skin tone 0..100), else two decimals.
		std::string valueText(const Slider& sl)
		{
			char b[32];
			if (sl.step >= 1 || sl.max - sl.min >= 10) { snprintf(b, sizeof b, "%d", (int)std::lround(sl.value)); }
			else { snprintf(b, sizeof b, "%.2f", sl.value); }
			return b;
		}
	}

	// The area between the columns: the host draws the character there and reads its press / drag / wheel itself before ui::event
	// (k16.2.433.1); solid so a click there doesn't reach the layer behind.
	class Creator::Preview : public Widget
	{
	public:
		Preview() { solid = true; }
	};

	// A column; without a cancel action Escape is not ours: it goes on to the host (the new game's editor: the game menu).
	class Creator::Side : public Window
	{
	public:
		bool onKey(int key, bool down) override { return onEscape && Window::onKey(key, down); }
	};

	Creator::Creator(CreatorActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		Widget& sc = root().add<Widget>();
		screen_ = &sc;
		sc.halign = sc.valign = A::kStretch;
		sc.visible = false;
		preview_ = &sc.add<Preview>();
		preview_->halign = preview_->valign = A::kStretch;
		preview_->rect = { kMargin + kLeftW + 20, 0, kMargin + kRightW + 20, 0 };

		// Left: who it is.
		Window& l = sc.add<Side>();
		left_ = &l;
		l.valign = A::kStretch;
		l.rect = { kMargin, kMargin, kLeftW, kMargin };
		Label& title = l.add<Label>("CHARACTER", "Kenshi_PaintedTextFont_Large");
		title.shadow = true;
		title.halign = A::kCenter;
		empty_ = &l.add<Label>("No character to edit yet");
		empty_->halign = empty_->valign = A::kCenter;
		cancel_ = &l.add<Button>("CANCEL");
		cancel_->valign = A::kEnd;
		cancel_->onClicked = [this] { if (act_.cancel) { auto f = act_.cancel; f(); } };
		leftBody_ = &l.add<Widget>();
		leftBody_->halign = leftBody_->valign = A::kStretch;
		leftBody_->rect = { 0, 56, 0, 0 };
		done_ = &l.add<Button>("CONFIRM");
		done_->halign = done_->valign = A::kEnd;
		done_->tooltip = "Start with this squad";
		done_->sound = "Create_Character";
		done_->onClicked = [this] { if (act_.done) { act_.done(); } };

		const float leftIn = kLeftW - 48;	// the window's content width
		Box& top = leftBody_->add<Box>(true, 8);
		leftTop_ = &top;
		Box& nameRow = top.add<Box>(false, 6);
		caption(nameRow, "NAME");
		name_ = &nameRow.add<EditBox>();
		name_->rect = { 0, 0, leftIn - kCaptionW - 6, kRowH };
		name_->maxBytes = 64;
		name_->onCommit = [this](const std::string& t) { if (act_.setName) { act_.setName(t); } };
		addPicker(top, "RACE", &CreatorActions::setGroup);
		addPicker(top, "SUBRACE", &CreatorActions::setRace);
		addPicker(top, "GENDER", &CreatorActions::setSex);
		Box& files = top.add<Box>(false, 6);
		import_ = &files.add<Button>("IMPORT");
		import_->tooltip = "Load a saved character";
		import_->onClicked = [this] { if (last_.editable && act_.importCharacter) { act_.importCharacter(); } };
		export_ = &files.add<Button>("EXPORT");
		export_->tooltip = "Save this character";
		export_->onClicked = [this] { if (last_.editable && act_.exportCharacter) { act_.exportCharacter(); } };
		clothes_ = &files.add<Button>("CLOTHES", Look::kToggle);
		clothes_->checkable = true;
		clothes_->tooltip = "Show the starting clothes";
		clothes_->onToggle = [this](bool on) { if (last_.editable && act_.setClothes) { act_.setClothes(on); } };
		addPicker(top, "CHARACTER", &CreatorActions::setCharacter);

		ScrollView& info = leftBody_->add<ScrollView>();
		info_ = &info;
		info.halign = info.valign = A::kStretch;
		info.rect = { 0, 0, 0, 52 };
		Box& ib = static_cast<Box&>(info.body());
		ib.spacing = 6;
		descHead_ = &heading(ib, "RACE DESCRIPTION");
		desc_ = &ib.add<Label>("", "Kenshi_StandardFont_Medium");
		desc_->wrap = true;
		desc_->rect.w = leftIn - kBar - 4;
		statHead_ = &heading(ib, "RACE STATS");
		statCol_ = &ib.add<Box>(true, 2);

		// Right: how it looks.
		Window& r = sc.add<Side>();
		right_ = &r;
		r.halign = A::kEnd;
		r.valign = A::kStretch;
		r.rect = { kMargin, kMargin, kRightW, kMargin };
		rightBody_ = &r.add<Widget>();
		rightBody_->halign = rightBody_->valign = A::kStretch;
		const float rightIn = kRightW - 48;
		Box& tabs = rightBody_->add<Box>(false, 6);
		tabRow_ = &tabs;
		for (int i = 0; i < 3; i++)
		{
			Button& t = tabs.add<Button>(kTabs[i]);
			t.rect.w = (rightIn - 12) / 3;
			t.onClicked = [this, i] { showPage(i); };
			tabs_[i] = &t;
		}
		for (int i = 0; i < 3; i++)	// update places their top under the tabs
		{
			ScrollView& sv = rightBody_->add<ScrollView>();
			sv.halign = sv.valign = A::kStretch;
			sv.rect = { 0, 0, 0, 52 };
			pages_[i] = &sv;
			pageBody(&sv).spacing = 6;
		}
		rowCols_[0] = &pageBody(pages_[kCreatorBody]).add<Box>(true, 6);
		Box& face = pageBody(pages_[kCreatorFace]);
		addPicker(face, "HEAD", &CreatorActions::setHead);
		rowCols_[1] = &face.add<Box>(true, 6);
		Box& hair = pageBody(pages_[kCreatorHair]);
		addPicker(hair, "HAIR", &CreatorActions::setHair);
		addPicker(hair, "BEARD", &CreatorActions::setBeard);
		addPicker(hair, "COLOUR", &CreatorActions::setHairColour);

		Box& acts = rightBody_->add<Box>(false, 6);
		acts.valign = A::kEnd;
		randomAll_ = &acts.add<Button>("RANDOMISE ALL");
		randomAll_->tooltip = "A random look: body, face and hair";
		randomAll_->onClicked = [this] { if (last_.editable && act_.randomizeAll) { act_.randomizeAll(); } };
		random_ = &acts.add<Button>("RANDOMISE");
		random_->tooltip = "A random look for this tab only";
		random_->onClicked = [this] { if (last_.editable && act_.randomize) { act_.randomize(page_); } };
		reset_ = &acts.add<Button>("RESET");
		reset_->tooltip = "The race's default look";
		reset_->onClicked = [this] { if (last_.editable && act_.resetDefaults) { act_.resetDefaults(); } };
		showPage(kCreatorBody);
		setCancel(std::move(act_.cancel));
		refresh(true, true);
	}

	Creator::~Creator()
	{
		if (ready() && screen_) { root().remove(screen_); }
	}

	bool Creator::shown() const { return screen_->visible; }

	void Creator::setCancel(std::function<void()> cancel)
	{
		act_.cancel = std::move(cancel);
		std::function<void()> esc;
		if (act_.cancel) { esc = [this] { if (act_.cancel) { auto f = act_.cancel; f(); } }; }
		left_->onEscape = right_->onEscape = esc;
		cancel_->visible = act_.cancel != nullptr;	// the new game's editor has none: CONFIRM is the way out
	}

	void Creator::show(bool on)
	{
		screen_->visible = on;
		if (on)
		{
			screen_->raise();
			focus(left_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	Rect Creator::previewRect() const { return shown() ? preview_->screen() : Rect{}; }

	Widget* Creator::nameBox() const { return name_; }
	Widget* Creator::preview() const { return preview_; }
	Widget* Creator::description() const { return desc_; }

	Widget* Creator::slider(const std::string& name) const
	{
		for (const Row& r : rows_) { if (r.name == name) { return r.slider; } }
		return nullptr;
	}

	Widget* Creator::sliderBox(const std::string& name) const
	{
		for (const Row& r : rows_) { if (r.name == name) { return r.box; } }
		return nullptr;
	}

	void Creator::showPage(int i)
	{
		page_ = i;
		for (int k = 0; k < 3; k++)
		{
			tabs_[k]->checked = k == i;
			pages_[k]->visible = k == i;
		}
	}

	// caption  <  value  >, as wide as its column (the right column's pages leave room for the scroll bar)
	void Creator::addPicker(Box& col, const char* text, std::function<void(int)> CreatorActions::* send)
	{
		const bool right = send == &CreatorActions::setHead || send == &CreatorActions::setHair || send == &CreatorActions::setBeard
			|| send == &CreatorActions::setHairColour;
		const float w = right ? kRightW - 48 - kBar : kLeftW - 48;
		Box& row = col.add<Box>(false, 6);
		caption(row, text);
		Picker p;
		p.send = send;
		p.prev = &row.add<Button>("<");
		p.prev->rect.w = 40;
		p.value = &row.add<Label>();
		p.value->fit = false;
		p.value->align = TextAlign::kCenter;
		p.valueW = w - kCaptionW - 6 * 3 - 80;
		p.value->rect = { 0, 0, p.valueW, kRowH };
		p.next = &row.add<Button>(">");
		p.next->rect.w = 40;
		const size_t i = pickers_.size();
		p.prev->onClicked = [this, i] { step(pickers_[i], -1); };
		p.next->onClicked = [this, i] { step(pickers_[i], 1); };
		pickers_.push_back(p);
	}

	// Wraps around; the next step starts from the new index, the label waits for the host's answer.
	void Creator::step(Picker& p, int d)
	{
		if (p.count < 2) { return; }
		p.index = ((p.index + d) % p.count + p.count) % p.count;
		if (const auto& f = act_.*p.send) { f(p.index); }
	}

	void Creator::setPicker(Picker& p, int index, int count, std::string text, bool usable)
	{
		p.index = index;
		p.count = count;
		p.value->text = std::move(text);
		p.value->colour = res::colour("Main").alpha(usable ? 1.0f : 0.35f);
		p.prev->enabled = p.next->enabled = usable;
	}

	void Creator::setPicker(Picker& p, int index, const std::vector<std::string>& names)
	{
		const int n = (int)names.size();
		std::string text = "-";
		if (index >= 0 && index < n) { text = !names[index].empty() ? names[index] : index == 0 ? "NONE" : "#" + std::to_string(index); }
		setPicker(p, index, n, text, n >= 2);
	}

	// One row per slider (name, slider, value box): Face on FACE; the rest on BODY, "Body" first in the state's order, other categories under
	// their heading.
	void Creator::rebuildSliders()
	{
		for (Widget* c : rowCols_) { c->clear(); }
		rows_.clear();
		auto addRow = [this](Widget* col, const CreatorSlider& cs)
		{
			Box& row = static_cast<Box&>(*col).add<Box>(false, 6);
			caption(row, cs.name, 170, "Kenshi_StandardFont_Medium").tooltip = cs.name;
			Slider& sl = row.add<Slider>(cs.min, cs.max, cs.step);
			sl.rect = { 0, 0, 270, 32 };
			sl.value = cs.value;
			EditBox& box = row.add<EditBox>(valueText(sl));
			box.rect = { 0, 0, 70, 32 };
			box.maxBytes = 12;
			const std::string name = cs.name;
			sl.onChange = [this, name, &sl, &box](double v)
			{
				box.text = valueText(sl);
				if (act_.setSlider) { act_.setSlider(name, (float)v); }
			};
			box.onCommit = [&sl, &box](const std::string& t)
			{
				char* end = nullptr;
				const double v = std::strtod(t.c_str(), &end);
				if (end != t.c_str()) { sl.setValue(v); }	// clamped and stepped; onChange sends it
				box.text = valueText(sl);
			};
			rows_.push_back({ cs.name, &sl, &box });
		};
		for (const CreatorSlider& cs : last_.sliders) { if (cs.category == "Face") { addRow(rowCols_[1], cs); } }
		for (const CreatorSlider& cs : last_.sliders) { if (cs.category == "Body") { addRow(rowCols_[0], cs); } }
		std::vector<std::string> done = { "Face", "Body" };
		for (const CreatorSlider& c : last_.sliders)
		{
			if (std::find(done.begin(), done.end(), c.category) != done.end()) { continue; }
			done.push_back(c.category);
			heading(static_cast<Box&>(*rowCols_[0]), c.category.empty() ? "OTHER" : c.category);
			for (const CreatorSlider& cs : last_.sliders) { if (cs.category == c.category) { addRow(rowCols_[0], cs); } }
		}
	}

	// "Hit Points ........ 100"
	void Creator::rebuildStats()
	{
		statCol_->clear();
		stats_.clear();
		for (const CreatorStat& st : last_.stats)
		{
			Box& row = static_cast<Box&>(*statCol_).add<Box>(false, 6);
			caption(row, st.label, 260, "Kenshi_StandardFont_Medium").rect.h = 24;
			Label& v = row.add<Label>(st.value, "Kenshi_StandardFont_Medium");
			v.fit = false;
			v.align = TextAlign::kEnd;
			if (!st.value.empty() && (st.value[0] == '+' || st.value[0] == '-')) { v.colour = res::colour(st.value[0] == '+' ? "StatGood" : "StatBad"); }	// a skill's +N% / -N%
			v.rect = { 0, 0, kLeftW - 48 - kBar - 260 - 6 - 4, 24 };
			stats_.push_back(&v);
		}
	}

	void Creator::update(const CreatorState& s)
	{
		if (!(s == last_))
		{
			const bool rows = !sameRows(s.sliders, last_.sliders);
			const bool stats = s.stats != last_.stats;
			last_ = s;
			refresh(rows, stats);
		}
		// The style decides the rows' heights (the original's buttons are taller), and the state which show: the scrolling parts start under them.
		info_->rect.y = leftTop_->preferred().h + 16;
		const float pagesY = tabRow_->preferred().h + 12;
		for (Widget* p : pages_) { p->rect.y = pagesY; }
	}

	void Creator::refresh(bool rows, bool stats)
	{
		const CreatorState& s = last_;
		empty_->visible = !s.valid;
		leftBody_->visible = rightBody_->visible = done_->visible = s.valid;
		if (!focused(name_)) { name_->text = s.name; }
		std::vector<std::string> groups;
		for (const CreatorRaceGroup& g : s.groups) { groups.push_back(g.name); }
		setPicker(pickers_[kGroup], s.group, groups);
		static const std::vector<std::string> kNoRaces;
		setPicker(pickers_[kRace], s.race, s.group >= 0 && s.group < (int)s.groups.size() ? s.groups[s.group].races : kNoRaces);
		setPicker(pickers_[kSex], s.sex, 2, s.sex == 1 ? "FEMALE" : "MALE", s.bothSexes);
		// an animal in the squad names its own race (no group entry, no subrace, no sex): the state's text, else the list's
		if (!s.raceText.empty()) { pickers_[kGroup].value->text = s.raceText; }
		if (!s.subraceText.empty()) { pickers_[kRace].value->text = s.subraceText; }
		if (!s.genderText.empty()) { pickers_[kSex].value->text = s.genderText; }
		else if (!s.editable) { pickers_[kSex].value->text = "-"; }
		setPicker(pickers_[kCharacter], s.character, s.characters, std::to_string(s.character + 1) + "/" + std::to_string(std::max(s.characters, 1)), s.characters >= 2);
		pickers_[kCharacter].prev->parent()->visible = s.characters >= 2;	// one character: no CHANGE CHARACTER at all
		setPicker(pickers_[kHead], s.head, s.heads, s.heads > 0 ? std::to_string(s.head + 1) + " / " + std::to_string(s.heads) : "-", s.heads >= 2);
		setPicker(pickers_[kHair], s.hair, s.hairs);
		setPicker(pickers_[kBeard], s.beard, s.beards);
		setPicker(pickers_[kHairColour], s.hairColour, s.hairColours);
		clothes_->checked = s.clothes;
		// An animal in the squad, as the original draws it (kenshi-inspect/findings/kenshi-k16.2.472/original_creator_bonedog_fido.jpg):
		// RACE / SUBRACE / GENDER without arrows (the value keeps its place), IMPORT / EXPORT / CLOTHES, the tabs and RANDOMISE ALL /
		// RANDOMISE / RESET drawn as usual but sending nothing, the tab pages empty, RACE DESCRIPTION with an empty body.
		for (int i = 0; i < (int)pickers_.size(); i++)
		{
			if (i == kCharacter) { continue; }
			Picker& p = pickers_[i];
			p.prev->visible = p.next->visible = s.editable;
			p.value->rect.w = p.valueW + (s.editable ? 0 : 2 * (p.prev->rect.w + 6));
			if (!s.editable)
			{
				p.prev->enabled = p.next->enabled = false;
				p.value->colour = res::colour("Main");
			}
		}
		clothes_->checkable = s.editable;	// a click on an animal's CLOTHES doesn't tick it
		for (Widget* p : pages_) { pageBody(p).visible = s.editable; }
		desc_->text = s.description;
		descHead_->visible = desc_->visible = !s.description.empty() || !s.editable;	// no empty headings while the host has nothing; an animal's stays
		statHead_->visible = !s.stats.empty();
		if (stats) { rebuildStats(); }
		if (rows) { rebuildSliders(); }
		for (Row& r : rows_) { r.slider->enabled = r.box->enabled = s.editable; }
		if (rows) { return; }
		for (const CreatorSlider& cs : s.sliders)	// the rows are in page order, not the state's
		{
			for (Row& r : rows_)
			{
				if (r.name != cs.name || pressed(r.slider)) { continue; }
				r.slider->value = cs.value;
				if (!focused(r.box)) { r.box->text = valueText(*r.slider); }
			}
		}
	}
}
