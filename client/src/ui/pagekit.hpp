#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/ui.hpp"

// Pieces shared by the overview's pages after Kenshi_OverviewWindow.layout (Research, Crafting): the layout's position_real fractions, the
// painted headings and the rows of buttons their lists are made of.
namespace ui
{
	// A fraction of an outer fraction-rect (a panel inside an inset), as a fraction of the page.
	inline Rect inside(const Rect& outer, const Rect& f) { return { outer.x + f.x * outer.w, outer.y + f.y * outer.h, f.w * outer.w, f.h * outer.h }; }

	inline Label& paintedHeading(Widget& parent, const char* text, TextAlign align)	// Kenshi_TextboxPaintedText
	{
		Label& l = parent.add<Label>(text, "Kenshi_PaintedTextFont_Medium");
		l.fit = false;
		l.align = align;
		return l;
	}

	// The page: each child placed by its fraction of the page (the layout's position_real), so the boxes keep the original's proportions.
	class FractionPage : public Widget
	{
	public:
		std::vector<std::pair<Widget*, Rect>> fractions;

		template <class T> T& at(const Rect& f, T& w)
		{
			fractions.push_back({ &w, f });
			return w;
		}

		void place(const Rect& px, float s) override
		{
			for (auto& [w, f] : fractions)
			{
				w->halign = w->valign = Align::kStart;
				w->rect = { f.x * px.w / s, f.y * px.h / s, f.w * px.w / s, f.h * px.h / s };
			}
			Widget::place(px, s);
		}
	};

	// Rows of Kenshi_Button1 buttons, the name left and a short text right (blueprints, recipes, queues): hover, a click, a right-click;
	// draggable rows reorder (the drop target framed). The progress is a Kenshi_LifeBar behind the text: a blueprint's progress widget (2, 4 of
	// a 185 x 20 button, 12 high), or a removable row's Progress. Removable rows are Kenshi_ItemListItem.layout's: the button over 0..0.9 of the
	// row, a red "X" (RemoveButton, another Kenshi_Button1) over the rest.
	class ButtonRows : public Widget
	{
	public:
		struct Row
		{
			std::string id = {}, name = {}, right = {}, tip = {};
			Color ink = { 0, 0, 0, 0 };	// alpha 0: the button's text colour
			float progress = 0;	// 0..1: the bar; 0 none
		};
		std::vector<Row> rows;
		float rowH = 34.5f, buttonH = 27;	// a row's pitch, its button
		bool draggable = false, removable = false;
		std::function<void(int)> onPick, onRight, onRemove;	// onRemove: a removable row's X
		std::function<void(int from, int to)> onMove;
		const Font* font = fonts::get("Kenshi_StandardFont_Medium");

		ButtonRows() { solid = true; }

		Size preferred() const override { return { 0, rows.size() * rowH }; }

		Rect button(size_t i) const
		{
			const float s = scale_;
			return { std::round(screen_.x), std::round(screen_.y + (i * rowH + (rowH - buttonH) / 2) * s), std::round(screen_.w), std::round(buttonH * s) };
		}

		Rect removeButton(size_t i) const	// w 0 unless removable
		{
			const Rect r = button(i);
			const float x = std::round(r.x + r.w * 0.9f);
			return removable ? Rect{ x, r.y, r.right() - x, r.h } : Rect{ r.right(), r.y, 0, r.h };
		}

		Rect progressBar(size_t i) const	// w 0: none
		{
			const Rect r = button(i);
			const float p = std::clamp(rows[i].progress, 0.0f, 1.0f);
			if (removable) { return { std::round(r.x + r.w * 0.015f), std::round(r.y + r.h * 0.216667f), std::round(r.w * 0.86f * p), std::round(r.h * 0.5f) }; }
			const float x = std::round(r.x + r.w * 2 / 185);
			return { x, std::round(r.y + r.h * 4 / 20), std::round((r.right() - 2 * (x - r.x) - r.x) * p), std::round(r.h * 12 / 20) };
		}

		int at(float x, float y) const
		{
			for (size_t i = 0; i < rows.size(); i++) { if (button(i).contains(x, y)) { return (int)i; } }
			return -1;
		}

		int hot() const
		{
			float x, y;
			mouse(x, y);
			return hovered(this) ? at(x, y) : -1;
		}

		std::string tip() const override
		{
			const int i = hot();
			return i >= 0 ? rows[i].tip : "";
		}

		bool onPress(Btn b, float x, float y) override
		{
			press_ = at(x, y);
			px_ = x;
			py_ = y;
			dragging_ = false;
			return b != Btn::kMiddle;
		}

		void onDrag(float x, float y) override
		{
			if (!draggable || press_ < 0 || dragging_ || std::hypot(x - px_, y - py_) < 5 * scale_) { return; }
			dragging_ = true;
			const Rect r = button(press_);
			const float ox = px_ - r.x, oy = py_ - r.y, s = scale_;
			setDragImage([=](float x, float y) { style().draw(Look::kButton, { std::round(x - ox), std::round(y - oy), r.w, r.h }, s, State::kNormal, false, Color{ 1, 1, 1, 0.5f }); });
		}

		void onRelease(Btn, float x, float y) override
		{
			if (!dragging_) { return; }
			dragging_ = false;
			setDragImage({});
			const int to = root().hit(x, y) == this ? at(x, y) : -1;
			if (to >= 0 && to != press_ && onMove) { auto f = onMove; f(press_, to); }
			press_ = -1;	// no click follows a drag
		}

		void onClick(Btn b) override
		{
			float x, y;
			mouse(x, y);
			const int i = at(x, y);
			if (i < 0 || i != press_) { return; }
			if (b == Btn::kLeft && removeButton(i).contains(x, y))
			{
				if (onRemove) { auto f = onRemove; f(i); }
				return;
			}
			if (b == Btn::kLeft && onPick) { auto f = onPick; f(i); }
			if (b == Btn::kRight && onRight) { auto f = onRight; f(i); }
		}

	protected:
		void paint() const override
		{
			if (!font) { return; }
			const int h = hot();
			float mx, my;
			mouse(mx, my);
			const int drop = dragging_ ? at(mx, my) : -1;
			for (size_t i = 0; i < rows.size(); i++)
			{
				const Row& row = rows[i];
				const Rect r = button(i), x = removeButton(i), face = { r.x, r.y, x.x - r.x, r.h };
				const bool onX = x.contains(mx, my);
				const auto state = [&](bool part) { return (int)i == h && part ? (pressed(this) && press_ == (int)i ? State::kPushed : State::kHighlighted) : State::kNormal; };
				const State st = state(!onX);
				style().draw(Look::kButton, face, scale_, st, false);
				const Rect bar = progressBar(i);
				if (bar.w > 0) { style().draw(Look::kBarFill, bar, scale_, State::kNormal, false); }
				const Rect a = style().textArea(Look::kButton, face, scale_, st, false);
				const Color ink = row.ink.a > 0 ? row.ink : removable ? style().colour("QueueText", style().textColour(Look::kButton, st, false)) : style().textColour(Look::kButton, st, false);
				const float pad = std::round(4 * scale_), ty = std::round(a.y + (a.h - font->lineHeight()) / 2), rw = font->width(row.right);
				gfx::pushClip(a);
				font->draw(std::round(a.x + pad), ty, row.name, ink);
				font->draw(std::round(a.right() - pad - rw), ty, row.right, ink);
				gfx::popClip();
				if (removable)
				{
					const State xs = state(onX);
					style().draw(Look::kButton, x, scale_, xs, false);
					const Rect xa = style().textArea(Look::kButton, x, scale_, xs, false);
					font->draw(std::round(xa.x + (xa.w - font->width("X")) / 2), std::round(xa.y + (xa.h - font->lineHeight()) / 2), "X", style().colour("QueueRemove", { 0.8f, 0.266f, 0.266f, 1 }));
				}
				if ((int)i == drop && drop != press_) { gfx::frame(r, std::max(1.0f, std::round(2 * scale_)), res::colour("Accent")); }
			}
		}

	private:
		int press_ = -1;
		float px_ = 0, py_ = 0;
		bool dragging_ = false;
	};
}
