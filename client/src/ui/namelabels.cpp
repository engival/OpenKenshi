#include "ui/namelabels.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Kenshi_CharacterNamePanel.layout at 1080 lines: the panel 0.0296296 of the screen high; StealthImage 38 x 27 on the left, vertically
		// centred; LightLevelImage 32 x 32 on the right; the panel as wide as the text, the shown icons and 10 (the exe's UI-scale x 10).
		constexpr float kH = 32, kStealthW = 38, kStealthH = 27, kLightW = 32, kPad = 10;
		constexpr int kLightFrames = 7;	// kenshi_images.xml: LightLevel's frames 32 apart along the atlas row (the image set keeps the first)
		const char* const kStealth[] = { "NameTagHidden", "NameTagAlmostSeen", "NameTagSeen" };
	}

	class NameLabels::Layer : public Widget
	{
	public:
		std::vector<NameLabel> labels;
		bool names = true;
		struct Drawn { Rect panel, text; std::string caption; Color colour; };
		mutable std::unordered_map<uint64_t, Drawn> drawn;
		const Font* font = fonts::get("Kenshi_StandardFont_Large");

	protected:
		void paint() const override
		{
			drawn.clear();
			const float s = scale_;
			const ImageSet* is = res::images("Kenshi_CharacterNameTags");
			const auto frame = [&](const char* name)
			{
				if (!is) { return Rect{}; }
				const auto it = is->frames.find(name);
				return it == is->frames.end() ? Rect{} : it->second;
			};
			const Rect stealthArt = frame("Stealth"), lightArt = frame("LightLevel");
			const Color shadow = style().colour("NameTagShadow", { 0.2807f, 0.2807f, 0.2807f, 1 });
			const float off = std::max(1.0f, std::round(s));
			for (const NameLabel& l : labels)
			{
				const bool icons = l.stealth > 0, text = names && font && !l.name.empty();
				const std::string caption = l.selected ? "[" + l.name + "]" : l.name;
				const float tw = text ? font->width(caption) : 0;
				const float w = tw + (icons ? (kStealthW + kLightW) * s : 0) + kPad * s, h = std::round(kH * s);
				if (!text && !icons) { continue; }
				Drawn& d = drawn[l.id];
				d.panel = { std::round(l.x - w / 2), std::round(l.y - h / 2), std::round(w), h };
				d.caption = text ? caption : "";
				d.colour = style().colour(l.selected ? "NameTagSelected" : "NameTagUnselected", l.selected ? Color{ 1, 1, 1, 1 } : Color{ 0.702f, 0.702f, 0.702f, 1 });
				if (icons)
				{
					const Rect st = { d.panel.x, std::round(d.panel.y + (h - kStealthH * s) / 2), std::round(kStealthW * s), std::round(kStealthH * s) };
					gfx::sprite(st, stealthArt, style().colour(kStealth[std::clamp(l.stealth, 1, 3) - 1], {}));
					Rect li = lightArt;
					li.x += li.w * std::round((1 - std::clamp(l.light, 0.0f, 1.0f)) * (kLightFrames - 1));
					gfx::sprite({ std::round(d.panel.right() - kLightW * s), d.panel.y, std::round(kLightW * s), h }, li, style().colour("NameTagLight", {}));
				}
				if (text)
				{
					const float tx = std::round(d.panel.x + (d.panel.w - tw) / 2), ty = std::round(d.panel.y + (h - font->lineHeight()) / 2);
					d.text = { tx, ty, tw, font->lineHeight() };
					font->draw(tx + off, ty + off, caption, shadow);
					font->draw(tx, ty, caption, d.colour);
				}
			}
		}
	};

	NameLabels::NameLabels()
	{
		layer_ = &root().add<Layer>();
		layer_->halign = layer_->valign = Widget::Align::kStretch;
		layer_->lower();	// under every window and the HUD (the layout's layer Back)
	}

	NameLabels::~NameLabels()
	{
		if (ready() && layer_) { root().remove(layer_); }
	}

	void NameLabels::update(std::vector<NameLabel> labels, bool names)
	{
		layer_->labels = std::move(labels);
		layer_->names = names;
	}

	Rect NameLabels::placed(uint64_t id) const { const auto i = layer_->drawn.find(id); return i == layer_->drawn.end() ? Rect{} : i->second.panel; }
	Rect NameLabels::textBox(uint64_t id) const { const auto i = layer_->drawn.find(id); return i == layer_->drawn.end() ? Rect{} : i->second.text; }
	std::string NameLabels::text(uint64_t id) const { const auto i = layer_->drawn.find(id); return i == layer_->drawn.end() ? "" : i->second.caption; }
	Color NameLabels::colour(uint64_t id) const { const auto i = layer_->drawn.find(id); return i == layer_->drawn.end() ? Color{} : i->second.colour; }
}
