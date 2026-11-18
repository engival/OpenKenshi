#include "ui/prospecting.hpp"

#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const float kBodyH = Prospecting::kH - CaptionWindow::kHeaderH;
	}

	// The map: the host's texture region in an inset box, the camera marker (Kenshi_MapMarkers "Camera", 0.107 of the map) rotated over it.
	class Prospecting::MapBox : public Widget
	{
	public:
		PortraitRef ref;
		float u = -1, v = 0, angle = 0;
		Rect camera;	// atlas texels

		MapBox()
		{
			solid = true;
			if (const ImageSet* is = res::images("Kenshi_MapMarkers"))
			{
				auto it = is->frames.find("Camera");
				if (it != is->frames.end()) { camera = it->second; }
			}
		}

		Rect marker() const
		{
			if (u < 0 || camera.w <= 0) { return {}; }
			const float s = screen_.w * 0.106667f;
			return { std::round(screen_.x + u * screen_.w - s / 2), std::round(screen_.y + v * screen_.h - s / 2), s, s };
		}

	protected:
		void paint() const override
		{
			style().draw(Look::kInset, screen_, scale_, State::kNormal, false);
			if (ref.tex) { gfx::textured(ref.tex, screen_, ref.uv[0], ref.uv[1], ref.uv[2], ref.uv[3], {}); }
			const Rect m = marker();
			if (m.w <= 0) { return; }
			gfx::pushClip(screen_);
			gfx::spriteRotated(m.x + m.w / 2, m.y + m.h / 2, m.w, m.h, angle, camera);
			gfx::popClip();
		}
	};

	// A resource line (Kenshi_ProspectingWindowResourceLine; live 20 px at 720): CheckboxButton (Kenshi_TickButton2, 171) with the name,
	// ValueText (Kenshi_GenericTextBoxFlatSmall) at 187.5, 70.5 wide.
	struct Prospecting::Row : public Widget
	{
		Button* tick = nullptr;
		Label* value = nullptr;

		Row()
		{
			rect = { 0, 0, 261, kRowH };
			tick = &add<Button>("", Look::kToggle, "Kenshi_StandardFont_Medium");
			tick->rect = { 0, 0, 171, kRowH };
			Panel& box = add<Panel>(Look::kField, 0);
			box.rect = { 187.5f, 0, 70.5f, kRowH };
			value = &box.add<Label>("", "Kenshi_StandardFont_MediumSmall");
			value->halign = value->valign = Align::kStretch;
			value->fit = false;
			value->align = TextAlign::kCenter;
		}
	};

	Prospecting::Prospecting(ProspectActions actions) : act_(std::move(actions))
	{
		using A = Widget::Align;
		win_ = &root().add<CaptionWindow>("PROSPECTING RESULTS", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->halign = win_->valign = A::kCenter;
		win_->rect = { 0, 0, kW, kH };
		win_->visible = false;
		Panel& body = win_->body();
		body.rect.h = kBodyH;

		// Positions: kinspect's live boxes (720 lines) x 1.5, from the body's top-left (594 x 512).
		Label& over = body.add<Label>("OVERVIEW", "Kenshi_PaintedTextFont_Medium");	// lbOverview
		over.fit = false;
		over.align = TextAlign::kCenter;
		over.rect = { 10, 2, 571, 25 };
		Panel& box = body.add<Panel>(Look::kInset, 0);	// Kenshi_GenericTextBoxSkin
		box.rect = { 7.5f, 27, 574.5f, 148.5f };
		for (int c = 0; c < 2; c++)	// OverviewDatapanel1 / 2
		{
			cols_[c] = &box.add<Box>(true, 0);
			cols_[c]->rect = { c == 0 ? 9.0f : 294.0f, 9, 264, 127.5f };
		}

		Label& resHead = body.add<Label>("RESOURCES", "Kenshi_PaintedTextFont_Medium");	// lbResources
		resHead.fit = false;
		resHead.align = TextAlign::kCenter;
		resHead.rect = { 12, 187, 261, 24 };
		list_ = &body.add<ScrollView>();	// ResourcesList
		list_->rect = { 12, 211, 261, 276 };
		static_cast<Box&>(list_->body()).spacing = 2;

		map_ = &body.add<MapBox>();	// Map
		map_->rect = { 282, 187, 298.5f, 298.5f };
	}

	Prospecting::~Prospecting()
	{
		if (ready() && win_) { root().remove(win_); }
	}

	void Prospecting::show(bool on)
	{
		if (win_->visible == on) { return; }
		if (on) { sound("Prospect_Results"); }
		win_->visible = on;
		if (on)
		{
			win_->rect.x = win_->rect.y = 0;	// re-centred on every show, as the original
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool Prospecting::shown() const { return win_->visible; }
	Widget* Prospecting::window() const { return win_; }
	Button* Prospecting::tick(size_t i) const { return i < last_.resources.size() ? rows_[i]->tick : nullptr; }
	Label* Prospecting::value(size_t i) const { return i < last_.resources.size() ? rows_[i]->value : nullptr; }
	Label* Prospecting::overviewLabel(int c, size_t i) const { return c >= 0 && c < 2 && i < last_.overview[c].size() ? lines_[c][i].first : nullptr; }
	Label* Prospecting::overviewValue(int c, size_t i) const { return c >= 0 && c < 2 && i < last_.overview[c].size() ? lines_[c][i].second : nullptr; }
	Widget* Prospecting::map() const { return map_; }
	Rect Prospecting::marker() const { return map_->marker(); }

	void Prospecting::update(const ProspectState& s)
	{
		if (s == last_) { return; }
		last_ = s;
		const Color ink = style().textColour(Look::kInset, State::kNormal, false);
		for (int c = 0; c < 2; c++)
		{
			std::vector<std::pair<Label*, Label*>>& lines = lines_[c];
			while (lines.size() < s.overview[c].size())
			{
				Widget& line = cols_[c]->add<Widget>();
				line.halign = Widget::Align::kStretch;
				line.rect.h = 20;	// 13-14 px at 720
				Label& l = line.add<Label>("", "Kenshi_StandardFont_Medium");
				l.valign = Widget::Align::kCenter;
				Label& v = line.add<Label>("", "Kenshi_StandardFont_Medium");
				v.halign = Widget::Align::kEnd;
				v.valign = Widget::Align::kCenter;
				lines.push_back({ &l, &v });
			}
			for (size_t i = 0; i < lines.size(); i++)
			{
				const bool on = i < s.overview[c].size();
				lines[i].first->parent()->visible = on;
				if (!on) { continue; }
				const ProspectLine& p = s.overview[c][i];
				lines[i].first->text = p.label;
				lines[i].second->text = p.value;
				lines[i].first->colour = p.labelColour.a > 0 ? p.labelColour : ink;
				lines[i].second->colour = p.valueColour.a > 0 ? p.valueColour : ink;
			}
		}
		while (rows_.size() < s.resources.size())
		{
			Row& r = list_->body().add<Row>();
			r.tick->onClicked = [this, i = rows_.size()]
			{
				if (i < last_.resources.size() && last_.resources[i].id != last_.selected && act_.select) { auto f = act_.select; f(last_.resources[i].id); }
			};
			rows_.push_back(&r);
		}
		for (size_t i = 0; i < rows_.size(); i++)
		{
			rows_[i]->visible = i < s.resources.size();
			if (i >= s.resources.size()) { continue; }
			rows_[i]->tick->text = s.resources[i].name;
			rows_[i]->tick->checked = s.resources[i].id == s.selected;
			rows_[i]->value->text = std::to_string(s.resources[i].value);
			const int v = s.resources[i].value;
			rows_[i]->value->colour = s.resources[i].colour.a > 0 ? s.resources[i].colour
				: style().colour(v < 50 ? "ProspectLow" : v <= 75 ? "ProspectMid" : "ProspectHigh", style().textColour(Look::kField, State::kNormal, false));
		}
		map_->ref = s.map;
		map_->u = s.markerU;
		map_->v = s.markerV;
		map_->angle = s.markerAngle;
	}
}
