#include "ui/diamond.hpp"

#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	Diamond::Diamond(bool vertical, std::string fontName) : vertical(vertical), font(fonts::get(fontName))
	{
		solid = true;
	}

	int Diamond::side(float x, float y) const
	{
		const float hw = screen_.w / 2, hh = screen_.h / 2, dx = x - (screen_.x + hw), dy = y - (screen_.y + hh);
		if (hw <= 0 || hh <= 0 || std::abs(dx) / hw + std::abs(dy) / hh > 1) { return 0; }
		return (vertical ? dy : dx) < 0 ? -1 : 1;
	}

	Widget* Diamond::hit(float x, float y)
	{
		return side(x, y) != 0 ? Widget::hit(x, y) : nullptr;
	}

	bool Diamond::onPress(Btn, float x, float y)
	{
		pressed_ = side(x, y);
		return true;
	}

	void Diamond::onRelease(Btn, float x, float y)
	{
		released_ = side(x, y);
	}

	void Diamond::onClick(Btn b)
	{
		const bool on = pressed_ < 0 ? firstEnabled : secondEnabled;
		if (b == Btn::kLeft && pressed_ != 0 && released_ == pressed_ && on && onStep) { onStep(pressed_); }
	}

	std::string Diamond::tip() const
	{
		float x, y;
		mouse(x, y);
		const int s = side(x, y);
		const std::string& half = s < 0 ? firstTip : secondTip;
		return s != 0 && !half.empty() ? half : tooltip;
	}

	// In the diamond's own axes: a along the arrows (negative: the first tip), p across them; k = how far in from the rect, pixels.
	void Diamond::paint() const
	{
		const float b = scale_, cx = screen_.x + screen_.w / 2, cy = screen_.y + screen_.h / 2;
		const float ha = (vertical ? screen_.h : screen_.w) / 2, hp = (vertical ? screen_.w : screen_.h) / 2;
		auto px = [&](float a, float p) { return vertical ? cx + p : cx + a; };
		auto py = [&](float a, float p) { return vertical ? cy + a : cy + p; };
		auto tri = [&](float a0, float p0, float a1, float p1, float a2, float p2, const Color& c)
		{
			gfx::triangle(px(a0, p0), py(a0, p0), px(a1, p1), py(a1, p1), px(a2, p2), py(a2, p2), c);
		};
		auto diamond = [&](float k, const Color& c)
		{
			const float a = ha - k, p = hp - k;
			tri(-a, 0, 0, p, a, 0, c);
			tri(-a, 0, a, 0, 0, -p, c);
		};
		// Active: a gold rim 7 units wide instead of 5 and the face washed with it, so the mode reads at a glance in either style (k16.3.32).
		const Color accent = res::colour("Accent");
		diamond(0, active ? accent : res::colour("Edge"));
		diamond(4 * b, active ? accent : res::colour("Border"));
		diamond((active ? 7 : 5) * b, res::colour("Window"));
		if (active) { diamond(7 * b, accent.alpha(0.22f)); }

		// The arrow tabs: the inner diamond's tips past 62% of the way out, a triangle on each pointing out.
		float mx, my;
		mouse(mx, my);
		const int over = hovered(this) ? side(mx, my) : 0, held = pressed(this) ? pressed_ : 0;
		const float ia = ha - 5 * b, ip = hp - 5 * b, k = 0.62f, cut = ip * (1 - k), arrow = 0.09f * (vertical ? screen_.h : screen_.w);
		for (int s : { -1, 1 })
		{
			const bool on = enabled && (s < 0 ? firstEnabled : secondEnabled);
			const char* face = !on ? "Window" : held == s ? "ButtonDown" : over == s ? "ButtonHover" : "Button";
			const char* ink = !on ? "ButtonText" : held == s ? "ButtonTextDown" : over == s ? "ButtonTextHover" : "ButtonText";
			tri(s * ia, 0, s * ia * k, -cut, s * ia * k, cut, res::colour(face));
			const float at = s * ia * 0.8f;
			tri(at + s * arrow * 0.6f, 0, at - s * arrow * 0.6f, -arrow, at - s * arrow * 0.6f, arrow, on ? res::colour(ink) : res::colour(ink).alpha(0.35f));
		}

		const Color ink = enabled ? res::colour("Main") : res::colour("Main").alpha(0.5f);	// as the clock's: light in both styles
		if (font && !text.empty())
		{
			const float w = font->width(text);
			font->draw(std::round(cx - w / 2), std::round(cy - font->lineHeight() / 2), text, ink);
		}
		if (picture)
		{
			const float s = std::round(0.36f * std::min(screen_.w, screen_.h));
			picture({ cx - s / 2, cy - s / 2, s, s }, ink);
		}
	}
}
