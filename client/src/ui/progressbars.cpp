#include "ui/progressbars.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "ui/skin.hpp"
#include "ui/ui.hpp"
#include "ui/xml.hpp"

namespace ui
{
	namespace
	{
		constexpr float kW = 215, kH = 33;	// the panel, units
		// The template's (Kenshi_ProgressBarFill, 150 x 20) track and fill: children aligned Stretch, so MyGUI keeps their edge offsets fixed
		// (left, top, right, bottom; units here) as the bar stretches. Measured on the original's 2560 x 1440 shots (kenshi-h2v.16.4): the
		// fill starts 11 px into the bar and is the bar's height less 7, in vanilla and Dark UI alike.
		const Rect kTrack = { 10, 2, 9, 3 }, kFill = { 11, 3, 10, 4 };

		Rect part(const Rect& r, const Rect& f) { return { std::round(r.x + f.x * r.w), std::round(r.y + f.y * r.h), std::round(f.w * r.w), std::round(f.h * r.h) }; }
		Rect edges(const Rect& r, const Rect& e, float s)
		{
			const float x = std::round(r.x + e.x * s), y = std::round(r.y + e.y * s);
			return { x, y, std::round(r.right() - e.w * s) - x, std::round(r.bottom() - e.h * s) - y };
		}

		// The panel layout's ProgressBar and Text (position_real fractions of the panel), from the themed Kenshi_ProgressBarPanel.layout: Dark
		// UI ships its own (a thinner bar, edge to edge). Vanilla's values when the file or a widget is missing.
		struct PanelLayout { Rect bar = { 0.0139535f, 0.0909091f, 0.976744f, 0.848485f }, text = { 0, 0, 1, 0.909091f }; };
		const PanelLayout& panelLayout()
		{
			static const PanelLayout pl = []
			{
				PanelLayout p;
				std::string err;
				const auto root = xml::load(res::file("layout/Kenshi_ProgressBarPanel.layout"), err);
				const xml::Node* panel = root ? root->first("Widget") : nullptr;
				if (!panel) { fprintf(stderr, "ui: Kenshi_ProgressBarPanel.layout: %s; vanilla geometry\n", err.empty() ? "no root widget" : err.c_str()); return p; }
				for (const xml::Node* w : panel->all("Widget"))
				{
					Rect r;
					if (sscanf(w->attr("position_real").c_str(), "%f %f %f %f", &r.x, &r.y, &r.w, &r.h) != 4) { continue; }
					if (w->attr("name") == "ProgressBar") { p.bar = r; }
					else if (w->attr("name") == "Text") { p.text = r; }
				}
				return p;
			}();
			return pl;
		}
	}

	Rect progressBarPlace(const Rect& panel) { return part(panel, panelLayout().bar); }

	Rect drawProgressBar(const Rect& bar, float progress, float scale)
	{
		const Rect place = edges(bar, kFill, scale), fill = { place.x, place.y, std::round(place.w * std::clamp(progress, 0.0f, 1.0f)), place.h };
		style().draw(Look::kProgressTrack, edges(bar, kTrack, scale), scale, State::kNormal, false);
		if (fill.w > 0) { style().draw(Look::kProgressFill, fill, scale, State::kNormal, false); }
		style().draw(Look::kProgressFrame, bar, scale, State::kNormal, false);
		return fill;
	}

	class ProgressBars::Layer : public Widget
	{
	public:
		std::vector<WorldProgress> bars;
		std::vector<WorldFloatText> floats;
		mutable std::unordered_map<uint64_t, Rect> placed, filled;
		mutable std::unordered_map<uint64_t, std::pair<Rect, float>> texts;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");
		const Font* floaters[3] = { fonts::get("Kenshi_FloaterFont_Small"), fonts::get("Kenshi_FloaterFont_Medium"), fonts::get("Kenshi_FloaterFont_Large") };

	protected:
		void paint() const override
		{
			placed.clear();
			filled.clear();
			const float s = scale_, w = std::round(kW * s), h = std::round(kH * s);
			const Color ink = style().colour("ProgressText", { 0.871f, 0.845f, 0.81f, 1 }), shadow = { 0, 0, 0, 1 };
			const float off = std::max(1.0f, std::round(s));
			for (const WorldProgress& pb : bars)
			{
				const WorldProgress* b = &pb;
				const Rect r = { std::round(b->x - w / 2), std::round(b->y), w, h };	// top centre at the point; bars may overlap, as in the game
				placed[b->id] = r;
				style().draw(Look::kPanelThin, r, s, State::kNormal, false);
				filled[b->id] = drawProgressBar(progressBarPlace(r), b->progress, s);
				if (font && !b->text.empty())	// the Text: centred in the layout's Text box (vanilla: the panel's upper 0.909)
				{
					const Rect tb = part(r, panelLayout().text);
					const float tx = std::round(tb.x + (tb.w - font->width(b->text)) / 2), ty = std::round(tb.y + (tb.h - font->lineHeight()) / 2);
					gfx::pushClip(r);
					font->draw(tx + off, ty + off, b->text, shadow);
					font->draw(tx, ty, b->text, ink);
					gfx::popClip();
				}
			}
			texts.clear();
			for (const WorldFloatText& t : floats)	// top left at the point (Kenshi_TextboxStandardText, no shadow); fades out from age 8, gone at 12
			{
				const Font* f = floaters[std::clamp(t.font, 0, 2)];
				if (!f) { f = font; }
				if (!f || t.age >= 12) { continue; }
				const float a = t.age > 8 ? 1 - (t.age - 8) * 0.25f : 1;
				const Rect r = { std::round(t.x), std::round(t.y), f->width(t.text), f->lineHeight() };
				texts[t.id] = { r, t.colour.a * a };
				f->draw(r.x, r.y, t.text, t.colour.alpha(a));	// x the fade only: Color::alpha multiplies the host's own a (0.7 for "Failed!"), so a2 halved it (k16.2.275.1)
			}
		}
	};

	ProgressBars::ProgressBars()
	{
		layer_ = &root().add<Layer>();
		layer_->halign = layer_->valign = Widget::Align::kStretch;
		layer_->lower();	// under every window and the HUD
	}

	ProgressBars::~ProgressBars()
	{
		if (ready() && layer_) { root().remove(layer_); }
	}

	void ProgressBars::update(std::vector<WorldProgress> bars) { layer_->bars = std::move(bars); }
	void ProgressBars::floatTexts(std::vector<WorldFloatText> texts) { layer_->floats = std::move(texts); }

	Rect ProgressBars::floatPlaced(uint64_t id) const
	{
		auto it = layer_->texts.find(id);
		return it != layer_->texts.end() ? it->second.first : Rect{};
	}

	float ProgressBars::floatAlpha(uint64_t id) const
	{
		auto it = layer_->texts.find(id);
		return it != layer_->texts.end() ? it->second.second : 0;
	}

	Rect ProgressBars::placed(uint64_t id) const
	{
		auto it = layer_->placed.find(id);
		return it != layer_->placed.end() ? it->second : Rect{};
	}

	Rect ProgressBars::filled(uint64_t id) const
	{
		auto it = layer_->filled.find(id);
		return it != layer_->filled.end() ? it->second : Rect{};
	}
}
