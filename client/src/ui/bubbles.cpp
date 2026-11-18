#include "ui/bubbles.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	class SpeechBubbles::Layer : public Widget
	{
	public:
		std::vector<SpeechBubble> bubbles;
		struct Drawn { Rect r; int lines = 0; float alpha = 0; };
		mutable std::unordered_map<uint64_t, Drawn> placed;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");
		const Font* large = fonts::get("Kenshi_StandardFont_Large");

	protected:
		void paint() const override
		{
			placed.clear();
			// Sizes from the original layout: 450 units at most (0.234 of 1920), text inset 31 units a side.
			const float s = scale_, side = 31 * s, padY = 5 * s, maxW = 450 * s - 2 * side;
			for (const SpeechBubble& b : bubbles)
			{
				const Font* f = b.large && large ? large : font;
				const float a = std::clamp(b.alpha, 0.0f, 1.0f);
				if (!f || b.text.empty() || a <= 0) { continue; }
				const std::vector<std::string_view> lines = f->wrap(b.text, maxW);
				float tw = 0;
				for (std::string_view l : lines) { tw = std::max(tw, f->width(l)); }
				const float w = std::round(tw + 2 * side), h = std::round(lines.size() * f->lineHeight() + 2 * padY);
				Rect r = { std::round(b.x - w / 2), std::round(b.y - h), w, h };
				if (b.important)
				{
					r.x = std::clamp(r.x, screen_.x, std::max(screen_.x, screen_.right() - w));
					r.y = std::clamp(r.y, screen_.y, std::max(screen_.y, screen_.bottom() - h));
				}
				placed[b.id] = { r, (int)lines.size(), a };
				style().draw(Look::kSpeech, r, s, State::kNormal, false, Color{}.alpha(a));
				float y = r.y + padY;
				const Color c = res::colour("Main").alpha(a), shadow = { 0, 0, 0, 0.75f * a };
				const float off = std::max(1.0f, std::round(s));
				for (std::string_view l : lines)
				{
					const float x = std::round(r.x + (r.w - f->width(l)) / 2);
					f->draw(x + off, y + off, l, shadow);
					f->draw(x, y, l, c);
					y += f->lineHeight();
				}
			}
		}
	};

	SpeechBubbles::SpeechBubbles()
	{
		layer_ = &root().add<Layer>();
		layer_->halign = layer_->valign = Widget::Align::kStretch;
		layer_->lower();	// under every window and the HUD
	}

	SpeechBubbles::~SpeechBubbles()
	{
		if (ready() && layer_) { root().remove(layer_); }
	}

	void SpeechBubbles::update(std::vector<SpeechBubble> bubbles) { layer_->bubbles = std::move(bubbles); }

	Rect SpeechBubbles::placed(uint64_t id) const { auto it = layer_->placed.find(id); return it != layer_->placed.end() ? it->second.r : Rect{}; }
	int SpeechBubbles::lines(uint64_t id) const { auto it = layer_->placed.find(id); return it != layer_->placed.end() ? it->second.lines : 0; }
	float SpeechBubbles::alpha(uint64_t id) const { auto it = layer_->placed.find(id); return it != layer_->placed.end() ? it->second.alpha : 0; }
}
