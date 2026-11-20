#include "ui/statuspopup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/font.hpp"
#include "ui/style.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const char* const kFont = "Kenshi_StandardFont_Medium";
		// Units: the panel's padding, the cue's dots and gaps, the gap before the seconds; a row is the font's line x kRowK.
		const float kPadX = 12, kPadY = 8, kDot = 6, kDotGap = 4, kCueGap = 8, kSecGap = 16, kRowK = 1.3f;

		std::string secondsText(float s)
		{
			char b[32];
			const int n = (int)std::floor(s);
			if (n < 60) { snprintf(b, sizeof b, "%d s", n); }
			else { snprintf(b, sizeof b, "%d m %02d s", n / 60, n % 60); }
			return b;
		}

		Color levelColour(StatusLine::Level l)
		{
			switch (l)
			{
				case StatusLine::Level::kWarn: return style().colour("Accent", Color{ 0.85f, 0.64f, 0.26f, 1 });
				case StatusLine::Level::kError: return style().colour("BadBright", Color{ 0.93f, 0.46f, 0.37f, 1 });
				default: return style().colour("Main", Color{ 0.91f, 0.89f, 0.85f, 1 });
			}
		}
	}

	// One line: the cue (busy), the text in its level's colour, the seconds at the right. Born / gone: ui::now() of its fade in / out
	// (gone < 0: still in the host's list).
	class StatusPopup::Row : public Widget
	{
	public:
		const StatusPopupActions* act = nullptr;
		const Font* font = fonts::get(kFont);
		StatusLine line;
		double born = 0, gone = -1;
		float indent = 0;	// units: the cue's column (set by the view when any row is busy)

		float alpha() const
		{
			const double t = now();
			const double in = std::clamp((t - born) / kFade, 0.0, 1.0);
			return (float)(gone < 0 ? in : std::min(in, std::clamp((gone + kFade - t) / kFade, 0.0, 1.0)));
		}

		bool expired() const { return gone >= 0 && now() >= gone + kFade; }

		// Pixels at the current scale: the text's column and the seconds' (the latter at least "00 s" wide, so the panel does not jitter as it counts).
		float width(float s) const
		{
			if (!font) { return 0; }
			float w = indent * s + font->width(line.text);
			if (line.seconds >= 0) { w += kSecGap * s + std::max(font->width(secondsText(line.seconds)), font->width("00 s")); }
			return w;
		}

		void onClick(Btn b) override
		{
			if (b == Btn::kLeft && gone < 0 && act && act->clicked) { act->clicked(line.key); }
		}

	protected:
		void paint() const override
		{
			const float a = alpha();
			if (a <= 0 || !font) { return; }
			const float s = scale_;
			if (solid && state() != State::kNormal) { gfx::fill(screen_, Color{ 1, 1, 1, 0.06f * a }); }
			const Color ink = levelColour(line.level).alpha(a);
			if (line.busy)
			{
				const int on = (int)std::fmod(now() * 3, 3.0);	// the main menu's cue: a step a third of a second
				const float d = std::round(kDot * s), y = std::round(screen_.y + (screen_.h - d) / 2);
				for (int i = 0; i < 3; i++) { gfx::fill({ std::round(screen_.x + i * (kDot + kDotGap) * s), y, d, d }, i == on ? ink : ink.alpha(0.3f)); }
			}
			const float ty = std::round(screen_.y + (screen_.h - font->lineHeight()) / 2), sh = std::max(1.0f, std::round(s));
			const auto text = [&](float x, std::string_view t, const Color& c)
			{
				font->draw(x + sh, ty + sh, t, { 0, 0, 0, 0.75f * c.a });
				font->draw(x, ty, t, c);
			};
			float right = screen_.right();
			if (line.seconds >= 0)
			{
				const std::string st = secondsText(line.seconds);
				right -= font->width(st);
				text(std::round(right), st, levelColour(StatusLine::Level::kInfo).alpha(0.65f * a));	// dimmed text (the original's Secondary is dark: unreadable on the panel)
				right -= kSecGap * s;
			}
			gfx::pushClip({ screen_.x, screen_.y, std::max(0.0f, right - screen_.x), screen_.h });	// a text past kMaxW is cut, never over the seconds
			text(std::round(screen_.x + indent * s), line.text, ink);
			gfx::popClip();
		}
	};

	// The panel: the rows top down, sized to the widest, at the top right or beside the avoid rect. Drops the rows whose fade out ended at
	// each layout, and hides itself with the last.
	class StatusPopup::View : public Widget
	{
	public:
		Rect avoid;
		std::vector<Row*> rows;	// top down, fading ones included

		float alpha() const
		{
			float a = 0;
			for (const Row* r : rows) { a = std::max(a, r->alpha()); }
			return a;
		}

		void place(const Rect&, float s) override
		{
			for (size_t i = 0; i < rows.size();)
			{
				if (rows[i]->expired())
				{
					remove(rows[i]);
					rows.erase(rows.begin() + i);
				}
				else { i++; }
			}
			if (rows.empty())
			{
				visible = false;
				return;
			}
			const Font* font = fonts::get(kFont);
			const float rowH = std::round((font ? font->lineHeight() : 20 * s) * kRowK);
			const bool busy = std::any_of(rows.begin(), rows.end(), [](const Row* r) { return r->line.busy; });
			float w = 0;
			for (Row* r : rows)
			{
				r->indent = busy ? 3 * kDot + 2 * kDotGap + kCueGap : 0;
				w = std::max(w, r->width(s));
			}
			w = std::round(std::clamp(w + 2 * kPadX * s, kMinW * s, kMaxW * s));
			const float h = std::round(rows.size() * rowH + 2 * kPadY * s);
			const Rect a = parent() ? parent()->content() : screen_;
			float x, y;
			if (avoid.w > 0 && avoid.h > 0)
			{
				x = avoid.x - kGap * s - w;
				y = avoid.y;
			}
			else
			{
				x = a.right() - kMargin * s - w;
				y = a.y + kTop * s;
			}
			x = std::round(std::clamp(x, a.x, std::max(a.x, a.right() - w)));
			y = std::round(std::clamp(y, a.y, std::max(a.y, a.bottom() - h)));
			screen_ = { x, y, w, h };
			scale_ = s;
			float ry = y + std::round(kPadY * s);
			for (Row* r : rows)
			{
				r->place({ x + std::round(kPadX * s), ry, w - 2 * std::round(kPadX * s), rowH }, s);
				ry += rowH;
			}
		}

	protected:
		void paint() const override
		{
			const float a = alpha();
			if (a > 0) { style().draw(Look::kPanel, screen_, scale_, State::kNormal, false, Color{}.alpha(a)); }
		}
	};

	StatusPopup::StatusPopup(StatusPopupActions actions)
	{
		act_ = std::move(actions);
		view_ = &root().add<View>();
		view_->visible = false;
	}

	StatusPopup::~StatusPopup()
	{
		if (ready() && view_) { root().remove(view_); }
	}

	void StatusPopup::update(const StatusPopupState& in)
	{
		if (in == last_) { return; }
		last_ = in;
		const double t = now();
		std::vector<StatusLine> lines = in.lines;
		if (lines.size() > kMaxLines) { lines.resize(kMaxLines); }
		View& v = *view_;
		v.avoid = in.avoid;
		const auto find = [&](const std::string& key) -> Row*
		{
			for (Row* r : v.rows) { if (r->line.key == key) { return r; } }
			return nullptr;
		};
		std::vector<Row*> order;	// the host's lines, then the fading ones put back near their old place
		for (const StatusLine& l : lines)
		{
			if (std::any_of(order.begin(), order.end(), [&](const Row* r) { return r->line.key == l.key; })) { continue; }	// a repeated key: the first wins
			Row* r = find(l.key);
			if (!r)
			{
				r = &v.add<Row>();
				r->act = &act_;
				r->born = t;
			}
			else if (r->gone >= 0)	// back while fading out: fades in again from where it is
			{
				r->born = t - r->alpha() * kFade;
				r->gone = -1;
			}
			r->line = l;
			r->solid = (bool)act_.clicked;
			order.push_back(r);
		}
		for (size_t i = 0; i < v.rows.size(); i++)
		{
			Row* r = v.rows[i];
			if (std::find(order.begin(), order.end(), r) != order.end()) { continue; }
			if (r->gone < 0) { r->gone = t; }
			r->solid = false;
			order.insert(order.begin() + std::min(i, order.size()), r);
		}
		v.rows = order;
		for (Row* r : order) { r->raise(); }	// children in the rows' order
		if (!order.empty()) { v.visible = true; }
	}

	bool StatusPopup::shown() const { return view_->visible; }
	Widget* StatusPopup::window() const { return view_; }

	Widget* StatusPopup::row(const std::string& key) const
	{
		for (Row* r : view_->rows) { if (r->line.key == key) { return r; } }
		return nullptr;
	}

	float StatusPopup::alpha(const std::string& key) const
	{
		for (Row* r : view_->rows) { if (r->line.key == key) { return r->alpha(); } }
		return 0;
	}
}
