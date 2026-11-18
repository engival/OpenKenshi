#include "ui/statgrid.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		// Icon geometry in a unit box, -0.5 .. 0.5, y down.
		struct Pen
		{
			Rect r;
			Color c;
			float x(float u) const { return r.x + r.w * (0.5f + u); }
			float y(float v) const { return r.y + r.h * (0.5f + v); }

			void line(float x0, float y0, float x1, float y1, float t = 0.09f) const
			{
				const float ax = x(x0), ay = y(y0), bx = x(x1), by = y(y1), dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
				if (len <= 0) { return; }
				const float nx = -dy / len * t * r.w / 2, ny = dx / len * t * r.w / 2;
				gfx::triangle(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, c);
				gfx::triangle(ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
			}

			void tri(float x0, float y0, float x1, float y1, float x2, float y2) const { gfx::triangle(x(x0), y(y0), x(x1), y(y1), x(x2), y(y2), c); }
			void box(float x0, float y0, float x1, float y1) const { gfx::fill({ std::round(x(x0)), std::round(y(y0)), std::round(x(x1) - x(x0)), std::round(y(y1) - y(y0)) }, c); }

			void ring(float cx, float cy, float rad, float t = 0.08f) const
			{
				for (int i = 0; i < 20; i++)
				{
					const float a0 = i * 6.2831853f / 20, a1 = (i + 1) * 6.2831853f / 20;
					line(cx + rad * std::cos(a0), cy + rad * std::sin(a0), cx + rad * std::cos(a1), cy + rad * std::sin(a1), t);
				}
			}

			void disc(float cx, float cy, float rad) const
			{
				for (int i = 0; i < 16; i++)
				{
					const float a0 = i * 6.2831853f / 16, a1 = (i + 1) * 6.2831853f / 16;
					tri(cx, cy, cx + rad * std::cos(a0), cy + rad * std::sin(a0), cx + rad * std::cos(a1), cy + rad * std::sin(a1));
				}
			}
		};

		void runner(const Pen& p)
		{
			p.box(0.08f, -0.44f, 0.24f, -0.28f);
			p.line(0.12f, -0.24f, -0.02f, 0.06f);
			p.line(0.1f, -0.2f, 0.22f, -0.06f);
			p.line(0.22f, -0.06f, 0.34f, -0.14f);
			p.line(0.1f, -0.2f, -0.08f, -0.12f);
			p.line(-0.08f, -0.12f, -0.16f, -0.24f);
			p.line(-0.02f, 0.06f, 0.18f, 0.2f);
			p.line(0.18f, 0.2f, 0.14f, 0.46f);
			p.line(-0.02f, 0.06f, -0.12f, 0.28f);
			p.line(-0.12f, 0.28f, -0.36f, 0.3f);
		}
	}

	void drawStatIcon(StatIcon icon, const Rect& px, const Color& ink)
	{
		const Pen p{ px, ink };
		switch (icon)
		{
			case StatIcon::kFist:	// four knuckles over the closed hand, the thumb across
				for (int i = 0; i < 4; i++) { p.box(-0.32f + 0.165f * i, -0.32f, -0.19f + 0.165f * i, -0.1f); }
				p.box(-0.32f, -0.04f, 0.34f, 0.36f);
				p.box(0.22f, -0.2f, 0.4f, 0.1f);
				break;
			case StatIcon::kSword:	// a blade on the diagonal, the guard across it, the grip and pommel
				p.line(-0.16f, 0.16f, 0.36f, -0.36f, 0.12f);
				p.tri(0.36f, -0.36f, 0.22f, -0.3f, 0.3f, -0.22f);
				p.line(-0.32f, 0.0f, 0.0f, 0.32f, 0.1f);
				p.line(-0.16f, 0.16f, -0.34f, 0.34f, 0.1f);
				p.box(-0.44f, 0.32f, -0.3f, 0.46f);
				break;
			case StatIcon::kShield:	// a heater shield's outline, a boss in the middle
				p.line(-0.32f, -0.36f, 0.32f, -0.36f);
				p.line(-0.32f, -0.36f, -0.32f, 0.04f);
				p.line(0.32f, -0.36f, 0.32f, 0.04f);
				p.line(-0.32f, 0.04f, 0.0f, 0.42f);
				p.line(0.32f, 0.04f, 0.0f, 0.42f);
				p.disc(0, -0.04f, 0.1f);
				break;
			case StatIcon::kCrossbow:	// the prod across, the string under it, the stock down
				p.line(-0.4f, -0.08f, 0.0f, -0.32f);
				p.line(0.0f, -0.32f, 0.4f, -0.08f);
				p.line(-0.4f, -0.08f, 0.4f, -0.08f, 0.04f);
				p.line(0.0f, -0.4f, 0.0f, 0.42f, 0.11f);
				p.box(-0.1f, 0.12f, 0.0f, 0.24f);
				break;
			case StatIcon::kCrosshair:
				p.ring(0, 0, 0.3f);
				p.line(0, -0.44f, 0, -0.14f, 0.07f);
				p.line(0, 0.14f, 0, 0.44f, 0.07f);
				p.line(-0.44f, 0, -0.14f, 0, 0.07f);
				p.line(0.14f, 0, 0.44f, 0, 0.07f);
				break;
			case StatIcon::kEye:	// two lids, the iris
				p.line(-0.44f, 0, -0.2f, -0.2f);
				p.line(-0.2f, -0.2f, 0.2f, -0.2f);
				p.line(0.2f, -0.2f, 0.44f, 0);
				p.line(-0.44f, 0, -0.2f, 0.2f);
				p.line(-0.2f, 0.2f, 0.2f, 0.2f);
				p.line(0.2f, 0.2f, 0.44f, 0);
				p.disc(0, 0, 0.14f);
				break;
			case StatIcon::kArm:	// a flexed arm: the upper arm level, the forearm up to the fist, the biceps a bulge on top
				p.line(-0.44f, 0.3f, 0.2f, 0.3f, 0.2f);
				p.line(0.2f, 0.3f, 0.2f, -0.18f, 0.18f);
				p.disc(-0.1f, 0.18f, 0.2f);
				p.disc(0.2f, -0.28f, 0.15f);
				break;
			case StatIcon::kBolt:	// dexterity: a lightning bolt
				p.tri(0.14f, -0.44f, -0.24f, 0.06f, 0.06f, 0.06f);
				p.tri(-0.06f, -0.06f, 0.24f, -0.06f, -0.14f, 0.44f);
				break;
			case StatIcon::kBricks:	// toughness: a wall, two courses offset
				p.box(-0.42f, -0.3f, -0.04f, -0.04f);
				p.box(0.04f, -0.3f, 0.42f, -0.04f);
				p.box(-0.42f, 0.04f, -0.24f, 0.3f);
				p.box(-0.16f, 0.04f, 0.22f, 0.3f);
				p.box(0.3f, 0.04f, 0.42f, 0.3f);
				break;
			case StatIcon::kDodge:	// a chevron stepping aside from three speed lines
				p.line(-0.04f, -0.36f, 0.28f, 0.0f, 0.12f);
				p.line(0.28f, 0.0f, -0.04f, 0.36f, 0.12f);
				p.line(-0.44f, -0.16f, -0.2f, -0.16f, 0.06f);
				p.line(-0.48f, 0.0f, -0.16f, 0.0f, 0.06f);
				p.line(-0.44f, 0.16f, -0.2f, 0.16f, 0.06f);
				break;
			case StatIcon::kRun:
				runner(p);
				break;
			case StatIcon::kKnockout:	// a big Z and a small one
				p.line(-0.36f, -0.3f, 0.08f, -0.3f);
				p.line(0.08f, -0.3f, -0.36f, 0.2f);
				p.line(-0.36f, 0.2f, 0.08f, 0.2f);
				p.line(0.16f, -0.02f, 0.4f, -0.02f, 0.07f);
				p.line(0.4f, -0.02f, 0.16f, 0.28f, 0.07f);
				p.line(0.16f, 0.28f, 0.4f, 0.28f, 0.07f);
				break;
			case StatIcon::kHunger:	// a bowl, steam over it
				for (int i = 0; i < 8; i++)
				{
					const float a0 = i * 3.14159265f / 8, a1 = (i + 1) * 3.14159265f / 8;
					p.tri(0, 0.02f, 0.4f * std::cos(a0), 0.02f + 0.36f * std::sin(a0), 0.4f * std::cos(a1), 0.02f + 0.36f * std::sin(a1));
				}
				p.line(-0.14f, -0.4f, -0.08f, -0.1f, 0.06f);
				p.line(0.06f, -0.4f, 0.12f, -0.1f, 0.06f);
				break;
		}
	}

	StatGrid::StatGrid()
	{
		solid = true;	// for the tooltips
	}

	Size StatGrid::preferred() const
	{
		return { rect.w, cellH * rows.size() };
	}

	const StatCell* StatGrid::cellAt(float x, float y) const
	{
		if (!screen_.contains(x, y) || rows.empty()) { return nullptr; }
		const size_t r = std::min(rows.size() - 1, (size_t)((y - screen_.y) / (cellH * scale_)));
		const std::vector<StatCell>& row = rows[r];
		if (row.empty()) { return nullptr; }
		const size_t c = std::min(row.size() - 1, (size_t)((x - screen_.x) / (screen_.w / row.size())));
		return &row[c];
	}

	std::string StatGrid::tip() const
	{
		float x, y;
		mouse(x, y);
		const StatCell* c = cellAt(x, y);
		return c ? c->tip : tooltip;
	}

	void StatGrid::paint() const
	{
		const Font* value = fonts::get("Kenshi_StandardFont_Medium");
		const Font* note = fonts::get("Kenshi_StandardFont_MediumSmall");
		const Color ink = res::colour("Main"), faint = ink.alpha(0.6f), rule = res::colour("Border");
		const float s = scale_, h = cellH * s, icon = std::round(26 * s);
		for (size_t r = 0; r < rows.size(); r++)
		{
			const std::vector<StatCell>& row = rows[r];
			const float y = screen_.y + r * h, w = row.empty() ? 0 : screen_.w / row.size();
			if (r > 0) { gfx::fill({ screen_.x + 4 * s, std::round(y), screen_.w - 8 * s, std::max(1.0f, std::round(s)) }, rule); }
			for (size_t c = 0; c < row.size(); c++)
			{
				const StatCell& k = row[c];
				const float x = screen_.x + c * w, cx = x + w / 2;
				if (c > 0) { gfx::fill({ std::round(x), y + 8 * s, std::max(1.0f, std::round(s)), h - 16 * s }, rule.alpha(0.5f)); }
				if (labels) { if (value) { value->draw(std::round(cx - value->width(k.label) / 2), std::round(y + 12 * s), k.label, faint); } }
				else { drawStatIcon(k.icon, { std::round(cx - icon / 2), std::round(y + 6 * s), icon, icon }, faint); }
				if (value) { value->draw(std::round(cx - value->width(k.value) / 2), std::round(y + 35 * s), k.value, ink); }
				if (note && !k.note.empty())
				{
					const Color nc = k.noteColour.empty() ? faint : res::colour(k.noteColour, faint);
					note->draw(std::round(cx - note->width(k.note) / 2), std::round(y + 53 * s), k.note, nc);
				}
				if (k.xp >= 0)
				{
					const Rect strip{ std::round(x + 6 * s), std::round(y + h - 5 * s), std::round(w - 12 * s), std::max(2.0f, std::round(2 * s)) };
					gfx::fill(strip, rule);
					gfx::fill({ strip.x, strip.y, std::round(strip.w * std::clamp(k.xp, 0.0f, 1.0f)), strip.h }, res::colour("BarFill"));
				}
			}
		}
	}
}
