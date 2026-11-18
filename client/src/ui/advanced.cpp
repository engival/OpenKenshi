#include "ui/advanced.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const float kBodyH = AdvancedOptions::kH - CaptionWindow::kHeaderH;
		const float kOverlap = 2.5f;	// the window's top over the panel's bottom (401 vs 36 + 366.7 at 720)
	}

	// Kept under its anchor (re-placed every frame, so a resize or a moved panel takes it along); the hovered name's tip, polled per frame.
	class AdvancedOptions::Frame : public CaptionWindow
	{
	public:
		const Widget* below = nullptr;
		std::function<void()> onPlace;
		Frame() : CaptionWindow("ADVANCED OPTIONS", {}, 0) {}

		void place(const Rect& px, float s) override
		{
			if (onPlace) { onPlace(); }
			CaptionWindow::place(px, s);
		}
	};

	AdvancedOptions::AdvancedOptions(AdvancedActions actions) : act_(std::move(actions))
	{
		Frame& win = root().add<Frame>();
		win_ = &win;
		win.movable = false;	// kept under its anchor, as the original's (Movable false)
		win.rect = { 0, 0, kW, kH };
		win.visible = false;
		win.onPlace = [this]
		{
			if (win_->below && win_->below->shown())
			{
				const Rect& p = win_->below->screen();
				win_->halign = win_->valign = Widget::Align::kStart;
				win_->rect.x = p.x / scale();
				win_->rect.y = p.bottom() / scale() - kOverlap;
			}
			hover();
		};
		Panel& body = win.body();
		body.rect.h = kBodyH;

		// Positions: kinspect's live boxes (720 lines) x 1.5, from the body's top-left.
		Widget& data = body.add<Widget>();	// Datapanel
		data.rect = { 12, 11.5f, 546, 312 };
		ScrollView& sv = data.add<ScrollView>();
		sv.halign = sv.valign = Widget::Align::kStretch;
		sv.rect = { 0, 6, 0, 0 };	// the rows (304.5) fill it: no scrollbar, as live
		list_ = static_cast<Box*>(&sv.body());
		list_->spacing = 0;

		Panel& tipBox = body.add<Panel>(Look::kInset, 9);	// TooltipPanel
		tipBox.rect = { 12, 353.5f, 546, 75 };
		tipBox.clipChildren = true;
		tip_ = &tipBox.add<RichText>();
		tip_->rect.w = 546 - 2 * 9;
		tip_->ink = style().textColour(Look::kInset, State::kNormal, false);
	}

	AdvancedOptions::~AdvancedOptions()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	std::string AdvancedOptions::number(double v)
	{
		char b[32];
		for (int p = 1; p <= 6; p++)
		{
			snprintf(b, sizeof b, "%.*g", p, v);
			if (std::strtod(b, nullptr) == v) { return b; }
		}
		return b;
	}

	void AdvancedOptions::openBelow(const Widget& panel)
	{
		win_->below = &panel;
		show(true);
	}

	void AdvancedOptions::show(bool on)
	{
		if (win_->visible == on) { return; }
		win_->visible = on;
		if (on) { win_->raise(); }	// not the keys: the panel above keeps Enter / Left / Right
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool AdvancedOptions::shown() const { return win_->visible; }
	Widget* AdvancedOptions::window() const { return win_; }
	Widget* AdvancedOptions::name(size_t i) const { return i < rows_.size() ? rows_[i].name : nullptr; }
	Slider* AdvancedOptions::slider(size_t i) const { return i < rows_.size() ? rows_[i].slider : nullptr; }
	EditBox* AdvancedOptions::valueBox(size_t i) const { return i < rows_.size() ? rows_[i].box : nullptr; }
	Button* AdvancedOptions::tick(size_t i) const { return i < rows_.size() ? rows_[i].tick : nullptr; }

	void AdvancedOptions::hover()
	{
		for (size_t i = 0; i < rows_.size() && i < last_.size(); i++)
		{
			if (hovered(rows_[i].name) && !last_[i].tip.empty()) { tip_->text = last_[i].tip; }
		}
	}

	void AdvancedOptions::update(const std::vector<AdvOption>& rows)
	{
		if (rows == last_) { return; }
		bool same = rows.size() == last_.size();
		for (size_t i = 0; same && i < rows.size(); i++)
		{
			same = rows[i].id == last_[i].id && rows[i].label == last_[i].label && rows[i].kind == last_[i].kind && rows[i].min == last_[i].min
				&& rows[i].max == last_[i].max && rows[i].step == last_[i].step;
		}
		last_ = rows;
		if (!same) { rebuild(); }
		for (size_t i = 0; i < rows_.size(); i++)
		{
			const Row& r = rows_[i];
			if (r.slider)
			{
				r.slider->value = std::clamp(last_[i].value, last_[i].min, last_[i].max);
				if (!focused(r.box)) { r.box->text = number(last_[i].value); }
			}
			else { r.tick->checked = last_[i].value != 0; }
		}
	}

	// A row per option. Slider row: the name [10.5, 163.5], the slider [174, 273], the value box [447, 87]; tick row: the name, the tick at 483.
	void AdvancedOptions::rebuild()
	{
		list_->clear();
		rows_.clear();
		for (const AdvOption& o : last_)
		{
			const bool slider = o.kind == AdvOption::Kind::kSlider;
			const float h = slider ? kSliderH : kTickH;
			Widget& line = list_->add<Widget>();
			line.rect = { 0, 0, 534, h };
			Widget& name = line.add<Widget>();	// a read-only box in the original: the text clipped
			name.rect = { 10.5f, 0, slider ? 163.5f : 472.5f - 10.5f, h };
			name.solid = name.clipChildren = true;
			Label& l = name.add<Label>(o.label, "Kenshi_StandardFont_Medium");
			l.valign = Widget::Align::kCenter;
			Row r = { &name, nullptr, nullptr, nullptr };
			const std::string id = o.id;
			if (slider)
			{
				Slider& sl = line.add<Slider>(o.min, o.max, o.step);
				sl.rect = { 174, 0, 273, h };
				EditBox& box = line.add<EditBox>();
				box.rect = { 447, 0, 87, h };
				box.maxBytes = 12;
				sl.onChange = [this, id, &box](double v)
				{
					box.text = number(v);
					if (act_.change) { act_.change(id, v); }
				};
				box.onCommit = [this, id, &sl, &box](const std::string& t)	// Enter or blur: clamped, not stepped
				{
					char* end = nullptr;
					const double v = std::strtod(t.c_str(), &end);
					if (end != t.c_str())
					{
						sl.value = std::clamp(v, sl.min, sl.max);
						if (act_.change) { act_.change(id, sl.value); }
					}
					box.text = number(sl.value);
				};
				r.slider = &sl;
				r.box = &box;
			}
			else
			{
				TickBox& tb = line.add<TickBox>();
				tb.rect = { 483, 0, kTickH, kTickH };
				tb.onToggle = [this, id](bool on) { if (act_.change) { act_.change(id, on ? 1 : 0); } };
				r.tick = &tb;
			}
			rows_.push_back(r);
		}
	}
}
