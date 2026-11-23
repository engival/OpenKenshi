#include "ui/widget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "app/config.hpp"
#include "sokol_app.h"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		Rect rounded(float x, float y, float w, float h)
		{
			const float x0 = std::round(x), y0 = std::round(y);
			return { x0, y0, std::round(x + w) - x0, std::round(y + h) - y0 };
		}

		float mouseY()
		{
			float x, y;
			mouse(x, y);
			return y;
		}

		void textLine(const Font& f, float x, float y, std::string_view t, const Color& c, bool shadow)
		{
			if (shadow) { f.draw(x + std::max(1.0f, std::round(scale())), y + std::max(1.0f, std::round(scale())), t, { 0, 0, 0, 0.75f * c.a }); }
			f.draw(x, y, t, c);
		}
	}

	// ---- Widget ----

	Widget::~Widget()
	{
		children_.clear();
		detail::forget(this);
	}

	Widget& Widget::adopt(std::unique_ptr<Widget> w)
	{
		Widget& r = *w;
		w->parent_ = this;
		children_.push_back(std::move(w));
		return r;
	}

	std::unique_ptr<Widget> Widget::take(Widget* child)
	{
		auto it = std::find_if(children_.begin(), children_.end(), [&](const auto& c) { return c.get() == child; });
		if (it == children_.end()) { return nullptr; }
		std::unique_ptr<Widget> w = std::move(*it);
		children_.erase(it);
		w->parent_ = nullptr;
		return w;
	}

	void Widget::remove(Widget* child)
	{
		auto it = std::find_if(children_.begin(), children_.end(), [&](const auto& c) { return c.get() == child; });
		if (it != children_.end()) { children_.erase(it); }
	}

	void Widget::raise()
	{
		if (!parent_) { return; }
		auto& sib = parent_->children_;
		auto it = std::find_if(sib.begin(), sib.end(), [&](const auto& c) { return c.get() == this; });
		if (it != sib.end()) { std::rotate(it, it + 1, sib.end()); }
	}

	void Widget::lower()
	{
		if (!parent_) { return; }
		auto& sib = parent_->children_;
		auto it = std::find_if(sib.begin(), sib.end(), [&](const auto& c) { return c.get() == this; });
		if (it != sib.end()) { std::rotate(sib.begin(), it, it + 1); }
	}

	void Widget::clear() { children_.clear(); }

	bool Widget::shown() const
	{
		for (const Widget* w = this; w; w = w->parent_) { if (!w->visible) { return false; } }
		return true;
	}

	void Widget::layout(const Rect& a, float s)
	{
		const Size p = preferred();
		float x, y, w = p.w * s, h = p.h * s;
		switch (halign)
		{
			case Align::kStart: x = a.x + rect.x * s; break;
			case Align::kEnd: x = a.right() - (rect.x + p.w) * s; break;
			case Align::kCenter: x = a.x + a.w / 2 + (rect.x - p.w / 2) * s; break;
			default: x = a.x + rect.x * s; w = a.w - (rect.x + rect.w) * s; break;
		}
		switch (valign)
		{
			case Align::kStart: y = a.y + rect.y * s; break;
			case Align::kEnd: y = a.bottom() - (rect.y + p.h) * s; break;
			case Align::kCenter: y = a.y + a.h / 2 + (rect.y - p.h / 2) * s; break;
			default: y = a.y + rect.y * s; h = a.h - (rect.y + rect.h) * s; break;
		}
		place(rounded(x, y, std::max(0.0f, w), std::max(0.0f, h)), s);
	}

	void Widget::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		const Rect c = content();
		for (const auto& ch : children_) { if (ch->visible) { ch->layout(c, s); } }
	}

	void Widget::draw() const
	{
		if (!visible) { return; }
		paint();
		if (clipChildren) { gfx::pushClip(screen_); }
		for (const auto& c : children_) { c->draw(); }
		if (clipChildren) { gfx::popClip(); }
		paintOver();
	}

	Widget* Widget::hit(float x, float y)
	{
		if (!visible) { return nullptr; }
		const bool inside = screen_.contains(x, y);
		if (clipChildren && !inside) { return nullptr; }
		for (auto it = children_.rbegin(); it != children_.rend(); ++it)
		{
			if (Widget* w = (*it)->hit(x, y)) { return w; }
		}
		return solid && inside ? this : nullptr;
	}

	State Widget::state() const
	{
		if (!enabled) { return State::kDisabled; }
		if (pressed(this)) { return hovered(this) ? State::kPushed : State::kHighlighted; }
		return hovered(this) ? State::kHighlighted : State::kNormal;
	}

	// ---- Panel ----

	Panel::Panel(Look look, float padding) : look(look), padding(padding) { solid = true; }

	Size Panel::preferred() const
	{
		Size p = { rect.w, rect.h };
		if (p.w > 0 && p.h > 0) { return p; }
		float w = 0, h = 0;
		for (const auto& c : children())
		{
			if (!c->visible) { continue; }
			const Size q = c->preferred();
			w = std::max(w, c->rect.x + (c->halign == Align::kStretch ? q.w + c->rect.w : q.w));
			h = std::max(h, c->rect.y + (c->valign == Align::kStretch ? q.h + c->rect.h : q.h));
		}
		if (p.w <= 0) { p.w = w + 2 * padding; }
		if (p.h <= 0) { p.h = h + 2 * padding; }
		return p;
	}

	Rect Panel::content() const { return screen_.inset(std::round(padding * scale_)); }

	void Panel::paint() const
	{
		style().draw(look, screen_, scale_, enabled ? State::kNormal : State::kDisabled, false, tint);
	}

	// ---- Window ----

	bool Window::onKey(int key, bool down)
	{
		if (keyHook && keyHook(key, down)) { return true; }
		if (key != SAPP_KEYCODE_ESCAPE) { return false; }
		if (takesEscape && !takesEscape()) { return false; }
		if (down && onEscape) { auto f = onEscape; f(); }
		return true;
	}

	// ---- CaptionWindow ----

	namespace
	{
		const cfg::Bool kMovable{ cfg::Group{ "Windows" }, "ui.windows_movable", true, "Windows move by dragging their title bar (the message box and the advanced options stay put)" };
		constexpr float kSnap = 10;	// units: MyGUI's Snap distance

		// Where windows are kept and snap: the UI's area (the host's frame, or the panel it gives the UI), never the OS window or screen.
		const Rect& area() { return root().screen(); }

		// r inside area; within snap of an edge, on it (snap 0: only clamped). Wider than the area: its left / top edge.
		Rect keepOn(Rect r, const Rect& area, float snap)
		{
			if (area.right() - r.right() < snap) { r.x = area.right() - r.w; }
			if (r.x - area.x < snap) { r.x = area.x; }
			if (area.bottom() - r.bottom() < snap) { r.y = area.bottom() - r.h; }
			if (r.y - area.y < snap) { r.y = area.y; }
			return r;
		}
	}

	class CaptionWindow::Header : public Panel
	{
	public:
		explicit Header(CaptionWindow& w) : Panel(Look::kHeader), win_(w) {}

		bool onPress(Btn b, float x, float y) override
		{
			CaptionWindow& a = win_.anchor();
			grab_ = b == Btn::kLeft && a.movable && kMovable.get();
			if (grab_)
			{
				win_.raiseGroup();
				gx_ = x - a.moveX * scale_;
				gy_ = y - a.moveY * scale_;
			}
			return true;
		}
		void onDrag(float x, float y) override { if (grab_) { win_.anchor().moveTo((x - gx_) / scale_, (y - gy_) / scale_); } }
		void onRelease(Btn, float, float) override { grab_ = false; }

	private:
		CaptionWindow& win_;
		bool grab_ = false;
		float gx_ = 0, gy_ = 0;	// the pointer at the window's laid-out spot, pixels
	};

	class CaptionWindow::Grip : public Widget
	{
	public:
		explicit Grip(CaptionWindow& w) : win_(w) { solid = true; tooltip = "Drag to resize"; }

		bool onPress(Btn b, float x, float y) override
		{
			grab_ = b == Btn::kLeft;
			if (grab_)
			{
				win_.raise();
				gx_ = x;
				gy_ = y;
				w0_ = win_.screen().w / scale_;
				h0_ = win_.screen().h / scale_;
			}
			return true;
		}
		void onDrag(float x, float y) override { if (grab_) { win_.resizeTo(w0_ + (x - gx_) / scale_, h0_ + (y - gy_) / scale_); } }
		void onRelease(Btn, float, float) override { grab_ = false; }

	protected:
		void paint() const override	// three diagonal ticks in the corner
		{
			const Color c = grab_ || hovered(this) ? res::colour("Accent") : res::colour("Main").alpha(0.75f);
			const float t = std::max(1.0f, std::round(scale_)), r = screen_.right() - 3 * scale_, b = screen_.bottom() - 3 * scale_;
			for (int i = 1; i <= 3; i++)
			{
				const float d = i * 4 * scale_;
				for (float k = 0; k < d; k += t) { gfx::fill(rounded(r - d + k, b - k - t, t, t), c); }
			}
		}

	private:
		CaptionWindow& win_;
		bool grab_ = false;
		float gx_ = 0, gy_ = 0, w0_ = 0, h0_ = 0;	// the pointer, pixels; the window's size at the press, units
	};

	CaptionWindow::CaptionWindow(std::string text, std::function<void()> onClose, float padding) : Window(Look::kNone, 0)
	{
		header_ = &add<Header>(*this);
		header_->halign = Align::kStretch;
		header_->rect = { 0, 0, 0, kHeaderH };
		caption_ = &header_->add<Label>(std::move(text), "Kenshi_PaintedTextFont_Medium");
		caption_->shadow = true;
		caption_->halign = caption_->valign = Align::kCenter;
		close_ = &header_->add<Button>("", Look::kClose);
		close_->rect = { 4, 0, 31, 32 };
		close_->halign = Align::kEnd;
		close_->valign = Align::kCenter;
		close_->tooltip = "Close";
		close_->visible = (bool)onClose;
		close_->onClicked = onClose;
		pin_ = &header_->add<Button>("", Look::kPin);	// beside close, as in the original's Kenshi_WindowCXP
		pin_->rect = { 36, 0, 31, 32 };
		pin_->halign = Align::kEnd;
		pin_->valign = Align::kCenter;
		pin_->tooltip = "Pin";
		pin_->checkable = true;
		pin_->visible = false;
		target_ = &header_->add<Button>("", Look::kTarget);	// ours: outside the pin (placed in preferred())
		target_->rect = { 36, 0, 31, 32 };
		target_->halign = Align::kEnd;
		target_->valign = Align::kCenter;
		target_->tooltip = "Quick right-click target: items right-clicked in the other windows go here";
		target_->checkable = true;
		target_->visible = false;
		if (onClose) { onEscape = [this, f = std::move(onClose)] { if (!pinned()) { f(); } }; }
		body_ = &add<Panel>(Look::kWindow, padding);
		body_->halign = Align::kStretch;
		body_->rect.y = kHeaderH;
		grip_ = &add<Grip>(*this);
		grip_->visible = false;
	}

	Size CaptionWindow::preferred() const
	{
		const Size b = body_->preferred();
		target_->rect.x = pin_->visible ? 68 : 36;
		const Button& outer = target_->visible ? *target_ : pin_->visible ? *pin_ : *close_;	// the caption stays centred clear of the buttons on both sides
		const float captionW = caption_->preferred().w + 2 * (outer.rect.x + outer.rect.w + close_->rect.x);
		return { rect.w > 0 ? rect.w : std::max(b.w, captionW), rect.h > 0 ? rect.h : kHeaderH + b.h };
	}

	void CaptionWindow::follow(CaptionWindow& lead)
	{
		unfollow();
		lead_ = &lead;
		lead.group_.push_back(this);
	}

	void CaptionWindow::raiseGroup()
	{
		CaptionWindow& a = anchor();
		a.raise();
		for (CaptionWindow* w : a.group_) { w->raise(); }
		raise();
	}

	void CaptionWindow::unfollow()
	{
		if (!lead_) { return; }
		std::erase(lead_->group_, this);
		lead_ = nullptr;
	}

	CaptionWindow::~CaptionWindow()
	{
		unfollow();
		for (CaptionWindow* w : group_) { w->lead_ = nullptr; }
	}

	Rect CaptionWindow::extent() const
	{
		Rect box = visible ? base_ : Rect{};
		for (const CaptionWindow* w : group_)
		{
			if (!w->visible || w->base_.w <= 0) { continue; }	// not laid out yet
			if (box.w <= 0) { box = w->base_; continue; }
			const float x = std::min(box.x, w->base_.x), y = std::min(box.y, w->base_.y);
			box = { x, y, std::max(box.right(), w->base_.right()) - x, std::max(box.bottom(), w->base_.bottom()) - y };
		}
		return box;
	}

	Rect CaptionWindow::shift(const Rect& box, float x, float y, float snap) const
	{
		const float s = scale();	// not scale_: a hidden lead was never placed
		return keepOn({ box.x + x * s, box.y + y * s, box.w, box.h }, area(), snap * s);
	}

	void CaptionWindow::place(const Rect& laid, float s)
	{
		laid_ = { laid.w / s, laid.h / s };
		Rect px = laid;
		if (resizable)	// from the top-left corner
		{
			const Rect& u = area();
			const float minW = minSize.w > 0 ? minSize.w : laid_.w, minH = minSize.h > 0 ? minSize.h : laid_.h;
			px.w = std::round(std::clamp(laid_.w + sizeW, minW, std::max(minW, u.w / s)) * s);
			px.h = std::round(std::clamp(laid_.h + sizeH, minH, std::max(minH, u.h / s)) * s);
		}
		base_ = px;
		const CaptionWindow& a = lead_ ? *lead_ : *this;
		if (a.movable && kMovable.get() && (a.moveX != 0 || a.moveY != 0))
		{
			const Rect box = a.extent(), moved = shift(box, a.moveX, a.moveY, 0);	// the others' rects from their last layout
			px = rounded(px.x + moved.x - box.x, px.y + moved.y - box.y, px.w, px.h);
		}
		Window::place(px, s);
		grip_->visible = resizable;
		if (!resizable) { return; }
		const float hh = kHeaderH * s, g = std::round(18 * s);
		body_->place(rounded(px.x, px.y + hh, px.w, px.h - hh), s);	// the body fills the resized window
		grip_->place(rounded(px.right() - g, px.bottom() - g, g, g), s);
	}

	void CaptionWindow::resizeTo(float w, float h)
	{
		const Rect& u = area();
		const float s = scale_, snap = kSnap;
		const Rect& at = screen();
		if ((u.right() - (at.x + w * s)) / s < snap) { w = (u.right() - at.x) / s; }	// within the snap of the far edges: on them
		if ((u.bottom() - (at.y + h * s)) / s < snap) { h = (u.bottom() - at.y) / s; }
		const float minW = minSize.w > 0 ? minSize.w : laid_.w, minH = minSize.h > 0 ? minSize.h : laid_.h;
		sizeW = std::clamp(w, minW, std::max(minW, (u.right() - at.x) / s)) - laid_.w;
		sizeH = std::clamp(h, minH, std::max(minH, (u.bottom() - at.y) / s)) - laid_.h;
	}

	void CaptionWindow::moveTo(float x, float y)
	{
		const Rect box = extent(), moved = shift(box, x, y, kSnap);
		moveX = (moved.x - box.x) / scale();
		moveY = (moved.y - box.y) / scale();
	}

	// ---- Label ----

	Label::Label(std::string t, std::string fontName) : text(std::move(t)), font(fonts::get(fontName)), colour(res::colour("Main")) {}

	Size Label::preferred() const
	{
		if (!fit || !font) { return { rect.w, rect.h }; }
		const float s = scale();
		if (wrap)
		{
			const float w = halign == Align::kStretch && screen_.w > 0 ? screen_.w : rect.w * s;
			return { rect.w, font->wrap(text, w).size() * font->lineHeight() / s };
		}
		float w = 0;
		size_t n = 0;
		for (std::string_view l : font->wrap(text, 1e9f)) { w = std::max(w, font->width(l)); n++; }
		return { std::ceil(w / s) + (shadow ? 1 : 0), n * font->lineHeight() / s };
	}

	void Label::paint() const
	{
		if (!font || text.empty()) { return; }
		const auto lines = wrap ? font->wrap(text, screen_.w) : font->wrap(text, 1e9f);
		const float lh = font->lineHeight(), total = lines.size() * lh;
		float y = vAlign == TextAlign::kStart ? screen_.y : vAlign == TextAlign::kEnd ? screen_.bottom() - total : screen_.y + (screen_.h - total) / 2;
		const Color c = enabled ? colour : colour.alpha(0.5f);
		for (std::string_view l : lines)
		{
			const float w = font->width(l);
			const float x = align == TextAlign::kStart ? screen_.x : align == TextAlign::kEnd ? screen_.right() - w : screen_.x + (screen_.w - w) / 2;
			textLine(*font, x, y, l, c, shadow);
			y += lh;
		}
	}

	// ---- Rich text ----

	namespace
	{
		// "{Name}text": the palette colour Name, else def.
		std::string_view inkPrefix(std::string_view s, const Color& def, Color& out)
		{
			out = def;
			const size_t end = s.find('}');
			if (s.empty() || s[0] != '{' || end == std::string_view::npos) { return s; }
			out = res::colour(std::string(s.substr(1, end - 1)), def);
			return s.substr(end + 1);
		}

		// A value's spans: a "{Colour}" (letters and digits) anywhere in it switches the ink for the rest ("Tech 1, {ResearchGood}Tech 2").
		template <class F> void valueSpans(std::string_view v, Color ink, F&& span)
		{
			while (!v.empty())
			{
				size_t at = 0, end = 0;
				for (at = v.find('{'); at != std::string_view::npos; at = v.find('{', at + 1))
				{
					end = v.find('}', at);
					if (end != std::string_view::npos && end > at + 1 && std::all_of(v.begin() + at + 1, v.begin() + end, [](char c) { return std::isalnum((unsigned char)c); })) { break; }
				}
				if (at == std::string_view::npos)
				{
					span(v, ink);
					return;
				}
				if (at > 0) { span(v.substr(0, at), ink); }
				ink = res::colour(std::string(v.substr(at + 1, end - at - 1)), ink);
				v = v.substr(end + 1);
			}
		}

		float valueWidth(std::string_view v, const Font& f)
		{
			float w = 0;
			valueSpans(v, {}, [&](std::string_view s, const Color&) { w += f.width(s); });
			return w;
		}
	}

	RichLayout layoutRich(std::string_view text, const Font& f, float wrapW, float rowH, float gap, const Color& ink)
	{
		RichLayout t;
		std::string_view rest = text;
		while (!rest.empty())
		{
			const size_t nl = rest.find('\n');
			std::string_view line = rest.substr(0, nl);
			rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
			RichRow r;
			line = inkPrefix(line, ink, r.ink);
			const size_t tab = line.find('\t');
			if (tab != std::string_view::npos)
			{
				r.text = line.substr(0, tab);
				r.value = inkPrefix(line.substr(tab + 1), r.ink, r.valueInk);
				t.w = std::max(t.w, f.width(r.text) + gap + valueWidth(r.value, f));
				r.h = rowH;
				t.rows.push_back(r);
				continue;
			}
			const auto parts = f.wrap(line, wrapW);
			r.h = parts.size() > 1 ? f.lineHeight() : rowH;
			if (parts.empty()) { t.rows.push_back(r); }
			for (std::string_view p : parts)
			{
				r.text = p;
				t.w = std::max(t.w, f.width(p));
				t.rows.push_back(r);
			}
		}
		for (const RichRow& r : t.rows) { t.h += r.h; }
		return t;
	}

	void drawRich(const RichLayout& t, const Font& f, float x, float y, float w)
	{
		for (const RichRow& r : t.rows)
		{
			const float ty = std::round(y + (r.h - f.lineHeight()) / 2);
			f.draw(x, ty, r.text, r.ink);
			float vx = x + w - valueWidth(r.value, f);
			valueSpans(r.value, r.valueInk, [&](std::string_view s, const Color& c)
			{
				f.draw(vx, ty, s, c);
				vx += f.width(s);
			});
			y += r.h;
		}
	}

	RichText::RichText(std::string t, std::string fontName) : text(std::move(t)), font(fonts::get(fontName)), ink(res::colour("Main")) {}

	RichLayout RichText::layout(float w) const
	{
		const float s = scale();
		return layoutRich(text, *font, w, std::max(font->lineHeight(), std::round(rowH * s)), gap * s, ink);
	}

	Size RichText::preferred() const
	{
		if (!font) { return { rect.w, rect.h }; }
		const float w = halign == Align::kStretch && screen_.w > 0 ? screen_.w : rect.w * scale();
		return { rect.w, layout(w).h / scale() };
	}

	void RichText::paint() const
	{
		if (font) { drawRich(layout(screen_.w), *font, screen_.x, screen_.y, screen_.w); }
	}

	// ---- Image ----

	Image::Image(const std::string& set, const std::string& index)
	{
		if (const ImageSet* is = res::images(set))
		{
			auto it = is->frames.find(index);
			if (it != is->frames.end()) { texels = it->second; }
		}
		rect.w = texels.w;
		rect.h = texels.h;
	}

	Rect portraitMarkArt(PortraitMark m)
	{
		static const char* const kIndex[] = { "", "Prison", "Slave", "Eaten" };
		const ImageSet* is = m == PortraitMark::kNone ? nullptr : res::images("Kenshi_PortraitImage");
		if (!is) { return {}; }
		const auto it = is->frames.find(kIndex[static_cast<int>(m)]);
		return it == is->frames.end() ? Rect{} : it->second;
	}

	Rect portraitBackArt(PortraitBack b)
	{
		static const char* const kIndex[] = { "", "Combat", "Hunger" };
		const ImageSet* is = b == PortraitBack::kNone ? nullptr : res::images("Kenshi_PortraitImage");
		if (!is) { return {}; }
		const auto it = is->frames.find(kIndex[static_cast<int>(b)]);
		return it == is->frames.end() ? Rect{} : it->second;
	}

	PortraitFace::PortraitFace()
	{
		halign = valign = Align::kStretch;
		back = &add<Image>();
		face = &add<TextureImage>();
		mark = &add<Image>();
		for (Widget* w : { static_cast<Widget*>(back), static_cast<Widget*>(face), static_cast<Widget*>(mark) }) { w->halign = w->valign = Align::kStretch; }
		back->keepAspect = mark->keepAspect = false;
		if (const ImageSet* is = res::images("Kenshi_PortraitImage"))
		{
			const auto it = is->frames.find("Hurt");
			if (it != is->frames.end()) { hurtArt_ = it->second; }
		}
	}

	void PortraitFace::set(const PortraitLayers& l, bool down)
	{
		layers_ = l;
		back->texels = portraitBackArt(l.back);
		back->visible = l.back != PortraitBack::kNone;
		mark->texels = portraitMarkArt(l.mark);
		mark->visible = l.mark != PortraitMark::kNone;
		face->tint = (down ? Color{ 0.45f, 0.45f, 0.45f, 1 } : Color{}).alpha(l.dim ? 0.25f : 1);
	}

	float PortraitFace::hurtAlpha() const
	{
		if (!layers_.hurt) { return 0; }
		const double p = std::fmod(now() * 2, 2.0);	// the original's step: 2.0 x frame dt, turning at 0 and 1
		return static_cast<float>(p < 1 ? p : 2 - p);
	}

	void PortraitFace::paint() const
	{
		const float a = hurtAlpha();
		if (a > 0 && hurtArt_.w > 0) { gfx::sprite(screen_, hurtArt_, Color{}.alpha(a)); }
	}

	void Image::paint() const
	{
		if (texels.w <= 0 || texels.h <= 0) { return; }
		Rect r = screen_;
		if (keepAspect)
		{
			const float k = std::min(r.w / texels.w, r.h / texels.h);
			r = rounded(r.x + (r.w - texels.w * k) / 2, r.y + (r.h - texels.h * k) / 2, texels.w * k, texels.h * k);
		}
		gfx::sprite(r, texels, enabled ? tint : tint.alpha(0.5f));
	}

	// ---- Button ----

	Button::Button(std::string t, Look look, std::string fontName) : look(look), text(std::move(t)), font(fonts::get(fontName))
	{
		solid = true;
	}

	Size Button::preferred() const
	{
		Size p = { rect.w, rect.h };
		const Size n = style().natural(look);
		const float lead = look == Look::kToggle ? n.h : 0;	// the light
		if (p.w <= 0) { p.w = std::max(n.w, font && !text.empty() ? std::ceil(font->width(text) / scale()) + 24 + lead : 0.0f); }
		if (p.h <= 0) { p.h = n.h > 0 ? n.h : font ? font->lineHeight() / scale() + 8 : 24; }
		return p;
	}

	void Button::onClick(Btn b)
	{
		if (b != Btn::kLeft)
		{
			if (auto f = onOtherClick) { f(b); }
			return;
		}
		auto toggled = onToggle;
		auto clicked = onClicked;	// copies: a callback may destroy this button
		if (clicked || toggled) { ui::sound(sound.empty() ? "General_Click" : sound); }
		if (checkable)
		{
			checked = !checked;
			if (toggled) { toggled(checked); }
		}
		if (clicked) { clicked(); }
	}

	void Button::onDoubleClick(Btn b)
	{
		if (b == Btn::kLeft && onDoubleClicked) { auto f = onDoubleClicked; f(); return; }
		onClick(b);
	}

	void Button::ownHints(std::vector<HintLine>& out) const
	{
		if (!clickHint.empty()) { out.push_back({ "Click", clickHint, enabled, {} }); }
		if (!doubleClickHint.empty()) { out.push_back({ "Double click", doubleClickHint, enabled, {} }); }
	}

	void Button::paint() const
	{
		const State s = held && enabled ? State::kPushed : state();
		style().draw(look, screen_, scale_, s, checked);
		const Rect area = style().textArea(look, screen_, scale_, s, checked);
		// Held (a click the host holds) keeps the pushed frame but not the pushed caption, which reads as greyed: the hover caption, brighter
		// (kenshi-k16.2.210.5: a held button must read as active).
		const Color lit = style().textColour(look, State::kHighlighted, checked);
		const Color colour = textColour.a > 0 ? (enabled ? textColour : textColour.alpha(0.5f))
			: s == State::kPushed && held ? Color{ std::min(1.0f, lit.r + 0.2f), std::min(1.0f, lit.g + 0.2f), std::min(1.0f, lit.b + 0.2f), lit.a }
			: style().textColour(look, s, checked);
		if (icon.w > 0)
		{
			const float k = std::min({ scale_, area.w / icon.w, area.h / icon.h });
			gfx::sprite(rounded(area.x + (area.w - icon.w * k) / 2, area.y + (area.h - icon.h * k) / 2, icon.w * k, icon.h * k), icon, enabled ? iconTint : iconTint.alpha(0.5f));
		}
		if (font && !text.empty())	// too wide: from the left, clipped (the HUD's shrunk squad tabs)
		{
			const float w = font->width(text), y = area.y + (area.h - font->lineHeight()) / 2;
			if (w <= area.w) { font->draw(align == TextAlign::kStart ? area.x : align == TextAlign::kEnd ? area.right() - w : area.x + (area.w - w) / 2, y, text, colour); }
			else
			{
				gfx::pushClip(area);
				font->draw(area.x, y, text, colour);
				gfx::popClip();
			}
		}
	}

	// ---- Dropdown ----

	Dropdown::Dropdown(std::string ph, std::string fontName) : Button({}, Look::kInput, std::move(fontName)), placeholder(std::move(ph))
	{
		align = TextAlign::kStart;
	}

	Size Dropdown::preferred() const
	{
		float w = 0;
		if (font) { for (const std::string& i : items) { w = std::max(w, font->width(i)); } w = std::max(w, font->width(placeholder)); }
		return { rect.w > 0 ? rect.w : std::ceil(w / scale()) + 40, rect.h > 0 ? rect.h : (font ? font->lineHeight() / scale() + 8 : 24) };
	}

	void Dropdown::onClick(Btn b)
	{
		if (b != Btn::kLeft || items.empty()) { return; }
		std::vector<Menu::Item> rows;
		for (size_t i = 0; i < items.size(); i++)
		{
			rows.push_back({ items[i], [this, i]
			{
				selected = (int)i;
				if (onPick) { onPick((int)i); }
			} });
		}
		auto m = std::make_unique<Menu>(std::move(rows), font ? font->name() : std::string("Kenshi_StandardFont_Medium"));
		m->rect.w = screen_.w / scale_;
		menu_ = &openMenu(std::move(m), screen_.x, screen_.bottom());
	}

	void Dropdown::paint() const
	{
		const State s = state();
		style().draw(look, screen_, scale_, s, false);
		const float a = std::round(5 * scale_), cx = screen_.right() - std::round(12 * scale_), cy = screen_.y + screen_.h / 2;
		const Color main = res::colour("Main");
		gfx::triangle(cx - a, cy - a * 0.5f, cx + a, cy - a * 0.5f, cx, cy + a * 0.6f, hovered(this) ? res::colour("Accent") : main.alpha(enabled ? 0.8f : 0.4f));
		const bool pick = selected >= 0 && selected < (int)items.size();
		const std::string& t = pick ? items[selected] : placeholder;
		if (!font || t.empty()) { return; }
		const Rect area = { screen_.x + std::round(6 * scale_), screen_.y, screen_.w - std::round(30 * scale_), screen_.h };
		gfx::pushClip(area);
		font->draw(area.x, area.y + (area.h - font->lineHeight()) / 2, t, pick && enabled ? main : main.alpha(0.5f));
		gfx::popClip();
	}

	// ---- TickBox ----

	TickBox::TickBox(std::string t, std::string fontName) : Button(std::move(t), Look::kTick, std::move(fontName))
	{
		checkable = true;
	}

	Size TickBox::preferred() const
	{
		const Size n = style().natural(look);
		const float sw = n.w > 0 ? n.w : 24, sh = n.h > 0 ? n.h : 24;
		const float tw = font && !text.empty() ? 6 + std::ceil(font->width(text) / scale()) : 0;
		return { rect.w > 0 ? rect.w : sw + tw, rect.h > 0 ? rect.h : std::max(sh, font ? font->lineHeight() / scale() : 0) };
	}

	void TickBox::paint() const
	{
		const Size n = style().natural(look);
		const float sw = (n.w > 0 ? n.w : 24) * scale_, sh = (n.h > 0 ? n.h : 24) * scale_;
		const Rect box = rounded(screen_.x, screen_.y + (screen_.h - sh) / 2, sw, sh);
		const State s = state();
		style().draw(look, box, scale_, s, checked);
		if (font && !text.empty())
		{
			Color c = res::colour("Main");
			if (s == State::kHighlighted || s == State::kPushed) { c = { std::min(1.0f, c.r * 1.2f), std::min(1.0f, c.g * 1.2f), std::min(1.0f, c.b * 1.2f), 1 }; }
			if (!enabled) { c = c.alpha(0.5f); }
			font->draw(box.right() + 6 * scale_, screen_.y + (screen_.h - font->lineHeight()) / 2, text, c);
		}
	}

	// ---- Slider ----

	Slider::Slider(double mn, double mx, double st) : min(mn), max(mx), step(st), value(mn)
	{
		solid = true;
	}

	// Part widths from the game's 51-unit-high layout: arrows 29 / 31, thumb 27 x 57, track 30 high.
	float Slider::arrowW(bool right) const { return screen_.h * (right ? 31.0f : 29.0f) / 51; }
	float Slider::thumbW() const { return screen_.h * 27 / 51; }

	Rect Slider::trackPx() const
	{
		const float l = screen_.x + arrowW(false), r = screen_.right() - arrowW(true);
		return { l, screen_.y, std::max(0.0f, r - l - thumbW()), screen_.h };
	}

	Size Slider::preferred() const { return { rect.w > 0 ? rect.w : 260, rect.h > 0 ? rect.h : 40 }; }

	void Slider::setValue(double v)
	{
		if (step > 0) { v = min + std::round((v - min) / step) * step; }
		v = std::clamp(v, min, max);
		if (v == value) { return; }
		value = v;
		if (auto f = onChange) { f(v); }
	}

	void Slider::userSet(double v)
	{
		const double before = value;
		setValue(v);
		if (value != before && now() - lastSound_ >= kSoundGap)
		{
			lastSound_ = now();
			ui::sound("Slider");
		}
	}

	bool Slider::onPress(Btn b, float x, float y)
	{
		if (b != Btn::kLeft) { return false; }
		const double st = step > 0 ? step : (max - min) / 100;
		if (x < screen_.x + arrowW(false)) { arrow_ = -1; userSet(value - st); return true; }
		if (x >= screen_.right() - arrowW(true)) { arrow_ = 1; userSet(value + st); return true; }
		held_ = dragged_ = false;	// a click goes where it lands
		onDrag(x, y);
		return true;
	}

	void Slider::onDrag(float x, float)
	{
		if (arrow_ != 0) { return; }
		const Rect t = trackPx();
		if (t.w <= 0) { return; }
		const double raw = min + (max - min) * std::clamp((x - t.x - thumbW() / 2) / t.w, 0.0f, 1.0f);
		double v = raw;
		if (held_ && std::fabs(x - heldX_) <= kNotchPull * scale_) { v = heldNotch_; }
		else
		{
			held_ = false;
			for (double n : notches)
			{
				if (!dragged_ || lastRaw_ == n || (lastRaw_ - n) * (raw - n) > 0) { continue; }	// not crossed (or onto) since the last move
				held_ = true;
				heldNotch_ = v = n;
				heldX_ = x;
				break;
			}
		}
		lastRaw_ = raw;
		dragged_ = true;
		userSet(v);
	}

	bool Slider::onScroll(float dy)
	{
		const double st = step > 0 ? step : (max - min) / 100;
		userSet(value + (dy > 0 ? st : dy < 0 ? -st : 0));
		return true;
	}

	// ---- ScrollBar ----

	ScrollBar::ScrollBar(bool v) : vertical(v) { solid = true; }

	Rect ScrollBar::arrow(bool far) const
	{
		const float a = std::round(vertical ? screen_.w : screen_.h);
		if (vertical) { return { screen_.x, far ? screen_.bottom() - a : screen_.y, screen_.w, a }; }
		return { far ? screen_.right() - a : screen_.x, screen_.y, a, screen_.h };
	}

	Rect ScrollBar::track() const
	{
		const Rect lo = arrow(false), hi = arrow(true);
		return vertical ? Rect{ screen_.x, lo.bottom(), screen_.w, hi.y - lo.bottom() } : Rect{ lo.right(), screen_.y, hi.x - lo.right(), screen_.h };
	}

	Rect ScrollBar::thumb() const
	{
		const Rect t = track();
		const double range = std::max(0.0, max - min);
		const float len = vertical ? t.h : t.w;
		const float tl = std::round(std::clamp((float)(len * page / std::max(1e-9, range + page)), std::min(len, 12 * scale_), len));
		const float at = std::round((range > 0 ? (float)((value - min) / range) : 0.0f) * (len - tl));
		return vertical ? Rect{ t.x, t.y + at, t.w, tl } : Rect{ t.x + at, t.y, tl, t.h };
	}

	void ScrollBar::set(double v)
	{
		v = std::clamp(v, min, std::max(min, max));
		if (v == value) { return; }
		value = v;
		if (onChange) { auto f = onChange; f(v); }
	}

	bool ScrollBar::onPress(Btn b, float x, float y)
	{
		if (b != Btn::kLeft) { return true; }
		const Rect th = thumb();
		if (arrow(false).contains(x, y)) { set(value - step); }
		else if (arrow(true).contains(x, y)) { set(value + step); }
		else if (th.contains(x, y)) { grab_ = vertical ? y - th.y : x - th.x; }
		else { set(value + ((vertical ? y < th.y : x < th.x) ? -page : page)); }
		return true;
	}

	void ScrollBar::onDrag(float x, float y)
	{
		if (grab_ < 0) { return; }
		const Rect t = track(), th = thumb();
		const float travel = vertical ? t.h - th.h : t.w - th.w;
		if (travel <= 0) { return; }
		const float at = (vertical ? y - t.y : x - t.x) - grab_;
		set(min + (max - min) * std::clamp(at / travel, 0.0f, 1.0f));
	}

	void ScrollBar::paint() const
	{
		float mx, my;
		mouse(mx, my);
		const bool hot = hovered(this);
		auto look = [&](const Rect& r) { return hot && r.contains(mx, my) ? State::kHighlighted : State::kNormal; };
		const Rect lo = arrow(false), hi = arrow(true), th = thumb();
		style().draw(Look::kScrollTrack, track(), scale_, State::kNormal, false);
		style().draw(vertical ? Look::kScrollUp : Look::kScrollLeft, lo, scale_, look(lo), false);
		style().draw(vertical ? Look::kScrollDown : Look::kScrollRight, hi, scale_, look(hi), false);
		style().draw(Look::kScrollThumb, th, scale_, grab_ >= 0 ? State::kPushed : look(th), false);
	}

	void Slider::paint() const
	{
		const State s = state();
		const float h = screen_.h, lw = arrowW(false), rw = arrowW(true);
		float mx, my;
		mouse(mx, my);
		// Each arrow lights up on its own: the widget's state is for the whole bar.
		auto part = [&](int which, bool over)
		{
			if (s == State::kDisabled) { return State::kDisabled; }
			if (arrow_ == which && pressed(this)) { return State::kPushed; }
			return hovered(this) && over ? State::kHighlighted : State::kNormal;
		};
		const Style& st = style();
		const float th = h * 30 / 51;
		st.draw(Look::kTrack, rounded(screen_.x + lw - 1, screen_.y + (h - th) / 2, screen_.w - lw - rw + 2, th), scale_, s == State::kDisabled ? s : State::kNormal, false);
		st.draw(Look::kArrowLeft, rounded(screen_.x, screen_.y, lw, h), scale_, part(-1, mx < screen_.x + lw), false);
		st.draw(Look::kArrowRight, rounded(screen_.right() - rw, screen_.y, rw, h), scale_, part(1, mx >= screen_.right() - rw), false);
		const Rect t = trackPx();
		const float tickW = std::max(1.0f, std::round(scale_)), tickH = std::round(4 * scale_);
		for (double n : notches)	// a small tick under the track at each notch
		{
			if (max <= min || n < min || n > max) { continue; }
			const float nx = t.x + t.w * (float)((n - min) / (max - min)) + thumbW() / 2;
			gfx::fill(rounded(nx - tickW / 2, screen_.y + (h + th) / 2 + std::round(2 * scale_), tickW, tickH), res::colour("Main").alpha(0.5f));
		}
		const float f = max > min ? (float)((value - min) / (max - min)) : 0;
		const float tw = thumbW(), tH = h * 57 / 51;
		const bool overThumb = std::fabs(mx - (t.x + t.w * f + tw / 2)) <= tw / 2;
		st.draw(Look::kThumb, rounded(t.x + t.w * f, screen_.y + (h - tH) / 2, tw, tH), scale_, part(0, overThumb), false);
	}

	// ---- EditBox ----

	EditBox::EditBox(std::string t, std::string fontName) : text(std::move(t)), font(fonts::get(fontName))
	{
		solid = true;
		caret_ = anchor_ = text.size();
	}

	Size EditBox::preferred() const
	{
		return { rect.w > 0 ? rect.w : 200, rect.h > 0 ? rect.h : std::max(32.0f, font ? font->lineHeight() / scale() + 12 : 32) };
	}

	Rect EditBox::textArea() const { return screen_.inset(std::round(10 * scale_)); }

	void EditBox::begin()
	{
		if (editing_) { return; }
		editing_ = true;
		start_ = text;
		caret_ = std::min(caret_, text.size());
		anchor_ = std::min(anchor_, text.size());
	}

	void EditBox::finish(bool keep)
	{
		if (!editing_) { return; }
		editing_ = false;
		if (!keep)
		{
			if (text != start_)
			{
				text = start_;
				changed();
			}
			caret_ = anchor_ = text.size();
			return;
		}
		if (text != start_ && onCommit) { auto f = onCommit; f(text); }
	}

	void EditBox::onBlur() { finish(true); }

	void EditBox::changed()
	{
		if (onChange) { auto f = onChange; f(text); }
	}

	std::string EditBox::selected() const
	{
		const size_t lo = std::min(caret_, anchor_), hi = std::min(std::max(caret_, anchor_), text.size());
		return lo < hi ? text.substr(lo, hi - lo) : std::string();
	}

	void EditBox::select(size_t from, size_t to)
	{
		anchor_ = std::min(from, text.size());
		caret_ = std::min(to, text.size());
	}

	bool EditBox::eraseSelection()
	{
		if (caret_ == anchor_) { return false; }
		const size_t lo = std::min(caret_, anchor_), hi = std::max(caret_, anchor_);
		text.erase(lo, hi - lo);
		caret_ = anchor_ = lo;
		return true;
	}

	void EditBox::insert(std::string_view s)
	{
		std::string clean;
		for (char c : s) { if ((unsigned char)c >= 32 && c != 127) { clean += c; } }	// one line, no control characters
		const size_t sel = std::max(caret_, anchor_) - std::min(caret_, anchor_);
		if (clean.empty() || text.size() - sel + clean.size() > maxBytes) { return; }
		eraseSelection();
		text.insert(caret_, clean);
		caret_ += clean.size();
		anchor_ = caret_;
		changed();
	}

	size_t EditBox::at(float x) const
	{
		if (!font) { return caret_; }
		const float x0 = textArea().x - scroll_;
		size_t best = 0;
		float bestD = std::fabs(x - x0);
		for (size_t i = 0; i < text.size();)
		{
			utf8Next(text, i);
			const float d = std::fabs(x - (x0 + font->width(std::string_view(text).substr(0, i))));
			if (d < bestD) { bestD = d; best = i; }
		}
		return best;
	}

	bool EditBox::onPress(Btn b, float x, float)
	{
		begin();
		kp_.reset();
		if (b != Btn::kLeft || !font) { return true; }
		caret_ = at(x);	// the caret goes to the code point boundary nearest the click; Shift + click extends the selection
		if (!(mods() & SAPP_MODIFIER_SHIFT)) { anchor_ = caret_; }
		return true;
	}

	void EditBox::onDrag(float x, float) { caret_ = at(x); }

	void EditBox::onDoubleClick(Btn b)
	{
		if (b == Btn::kLeft) { select(0, text.size()); }
	}

	bool EditBox::onChar(unsigned c)
	{
		begin();
		if (kp_ && kp_->at == now() && ((c >= '0' && c <= '9') || c == '.' || c == ','))	// NumLock on: the keypad key typed after all; undo its navigation
		{
			text = kp_->text;
			caret_ = kp_->caret;
			anchor_ = kp_->anchor;
			kp_.reset();
		}
		kp_.reset();
		if (c < 32 || c == 127 || (mods() & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER))) { return true; }
		char u[4];
		size_t n;
		if (c < 0x80) { u[0] = (char)c; n = 1; }
		else if (c < 0x800) { u[0] = (char)(0xC0 | c >> 6); u[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
		else if (c < 0x10000) { u[0] = (char)(0xE0 | c >> 12); u[1] = (char)(0x80 | (c >> 6 & 0x3F)); u[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
		else { u[0] = (char)(0xF0 | c >> 18); u[1] = (char)(0x80 | (c >> 12 & 0x3F)); u[2] = (char)(0x80 | (c >> 6 & 0x3F)); u[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
		insert(std::string_view(u, n));
		return true;
	}

	bool EditBox::onKey(int key, bool down)
	{
		if (key >= SAPP_KEYCODE_F1 && key <= SAPP_KEYCODE_F25) { return false; }
		if (!down) { return true; }
		begin();
		kp_.reset();
		// The keypad (X11 reports its keys the same whatever NumLock): navigation now; a digit char right after puts this back (onChar).
		static constexpr struct { int kp, nav; } kKeypad[] = {
			{ SAPP_KEYCODE_KP_4, SAPP_KEYCODE_LEFT }, { SAPP_KEYCODE_KP_6, SAPP_KEYCODE_RIGHT }, { SAPP_KEYCODE_KP_7, SAPP_KEYCODE_HOME },
			{ SAPP_KEYCODE_KP_1, SAPP_KEYCODE_END }, { SAPP_KEYCODE_KP_DECIMAL, SAPP_KEYCODE_DELETE }, { SAPP_KEYCODE_KP_0, 0 },
			{ SAPP_KEYCODE_KP_2, 0 }, { SAPP_KEYCODE_KP_3, 0 }, { SAPP_KEYCODE_KP_5, 0 }, { SAPP_KEYCODE_KP_8, 0 }, { SAPP_KEYCODE_KP_9, 0 } };
		for (const auto& k : kKeypad)
		{
			if (key != k.kp) { continue; }
			const Kp was{ text, caret_, anchor_, now() };
			const bool r = k.nav ? onKey(k.nav, true) : true;
			kp_ = was;
			return r;
		}
		const uint32_t m = mods();
		const bool shift = m & SAPP_MODIFIER_SHIFT, ctrl = m & SAPP_MODIFIER_CTRL;
		auto prev = [&](size_t i) { do { i--; } while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80); return i; };
		auto space = [&](size_t i) { return text[i] == ' '; };
		auto wordLeft = [&](size_t i) { while (i > 0 && space(i - 1)) { i--; } while (i > 0 && !space(i - 1)) { i--; } return i; };
		auto wordRight = [&](size_t i) { while (i < text.size() && !space(i)) { i++; } while (i < text.size() && space(i)) { i++; } return i; };
		auto move = [&](size_t to) { caret_ = to; if (!shift) { anchor_ = caret_; } };
		const size_t lo = std::min(caret_, anchor_), hi = std::max(caret_, anchor_);
		auto copy = [&]
		{
			const std::string s = selected();
			if (s.empty()) { return; }
			if (clipboardSet) { clipboardSet(s); }
		};
		switch (key)
		{
			case SAPP_KEYCODE_LEFT:
				if (ctrl) { move(wordLeft(caret_)); }
				else if (!shift && lo != hi) { caret_ = anchor_ = lo; }	// a plain arrow collapses the selection to its near end
				else if (caret_ > 0) { move(prev(caret_)); }
				break;
			case SAPP_KEYCODE_RIGHT:
				if (ctrl) { move(wordRight(caret_)); }
				else if (!shift && lo != hi) { caret_ = anchor_ = hi; }
				else if (caret_ < text.size()) { size_t n = caret_; utf8Next(text, n); move(n); }
				break;
			case SAPP_KEYCODE_HOME: move(0); break;
			case SAPP_KEYCODE_END: move(text.size()); break;
			case SAPP_KEYCODE_BACKSPACE:
				if (eraseSelection()) { changed(); }
				else if (caret_ > 0)
				{
					const size_t p = ctrl ? wordLeft(caret_) : prev(caret_);
					text.erase(p, caret_ - p);
					caret_ = anchor_ = p;
					changed();
				}
				break;
			case SAPP_KEYCODE_DELETE:
				if (eraseSelection()) { changed(); }
				else if (caret_ < text.size())
				{
					size_t n = caret_;
					utf8Next(text, n);
					text.erase(caret_, n - caret_);
					changed();
				}
				break;
			case SAPP_KEYCODE_A:
				if (ctrl) { select(0, text.size()); }
				break;
			case SAPP_KEYCODE_C:
				if (ctrl) { copy(); }
				break;
			case SAPP_KEYCODE_X:
				if (ctrl && lo != hi)
				{
					copy();
					eraseSelection();
					changed();
				}
				break;
			case SAPP_KEYCODE_V:
				if (ctrl)
				{
					const std::string clip = clipboardGet ? clipboardGet() : "";
					insert(std::string_view(clip).substr(0, clip.find_first_of("\r\n")));
				}
				break;
			case SAPP_KEYCODE_ENTER:
			case SAPP_KEYCODE_KP_ENTER:
			case SAPP_KEYCODE_ESCAPE:
			{
				if (key == SAPP_KEYCODE_ESCAPE && onEscape)
				{
					auto g = onEscape;
					if (g())
					{
						editing_ = false;
						break;
					}
				}
				if (key != SAPP_KEYCODE_ESCAPE && keepsKeyboard)
				{
					if (onEnter) { auto g = onEnter; g(); }
					break;
				}
				finish(key != SAPP_KEYCODE_ESCAPE);
				Widget* f = parent();	// the keyboard back to the enclosing window
				while (f && !f->focusable()) { f = f->parent(); }
				focus(f);
				if (key != SAPP_KEYCODE_ESCAPE && onEnter) { auto g = onEnter; g(); }
				break;
			}
			default: break;
		}
		return true;
	}

	void EditBox::paint() const
	{
		const bool on = focused(this);
		style().draw(Look::kInput, screen_, scale_, enabled ? (on || hovered(this) ? State::kHighlighted : State::kNormal) : State::kDisabled, false);
		if (!font) { return; }
		const Rect a = textArea();
		const float caretX = font->width(std::string_view(text).substr(0, std::min(caret_, text.size())));
		// Keep the caret inside the area, and no empty space at the end while the text is longer than the area.
		scroll_ = std::clamp(scroll_, caretX - a.w + 2 * scale_, caretX);
		scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, font->width(text) - a.w + 2 * scale_));
		const Color c = style().textColour(Look::kInput, enabled ? State::kNormal : State::kDisabled, false);
		const float y = a.y + (a.h - font->lineHeight()) / 2;
		gfx::pushClip(screen_.inset(std::round(4 * scale_)));
		if (on && anchor_ != caret_)
		{
			const std::string_view t(text);
			const float x0 = font->width(t.substr(0, std::min(std::min(caret_, anchor_), text.size()))), x1 = font->width(t.substr(0, std::min(std::max(caret_, anchor_), text.size())));
			gfx::fill(rounded(a.x - scroll_ + x0, y, x1 - x0, font->lineHeight()), res::colour("TextSelect"));
		}
		font->draw(std::round(a.x - scroll_), std::round(y), text, c);
		if (text.empty() && !hint.empty()) { font->draw(std::round(a.x), std::round(y), hint, c.alpha(0.45f)); }
		if (on && std::fmod(now(), 1.0) < 0.6) { gfx::fill(rounded(a.x - scroll_ + caretX, y, std::max(1.0f, std::round(scale_ * 1.5f)), font->lineHeight()), c); }
		gfx::popClip();
	}

	// ---- TabView ----

	TabView::TabView(float buttonW, float headerH) : buttonW(buttonW), headerH(headerH)
	{
		header_ = &Widget::add<Box>(false, gap);
	}

	Widget& TabView::add(const std::string& caption)
	{
		Button& b = header_->add<Button>(caption, Look::kButton);
		if (buttonW > 0) { b.rect = { 0, 0, buttonW, headerH }; }
		const size_t i = tabs_.size();
		b.sound = "Tab";
		b.onClicked = [this, i] { select(i); };
		b.checked = tabs_.empty();
		tabs_.push_back(&b);
		Widget& p = Widget::add<Widget>();
		p.halign = p.valign = Align::kStretch;
		p.visible = pages_.empty();
		pages_.push_back(&p);
		return p;
	}

	void TabView::clearTabs()
	{
		header_->clear();
		for (Widget* p : pages_) { remove(p); }
		tabs_.clear();
		pages_.clear();
		selected_ = 0;
	}

	void TabView::select(size_t i)
	{
		if (i >= tabs_.size()) { return; }
		selected_ = i;
		for (size_t t = 0; t < tabs_.size(); t++)
		{
			tabs_[t]->checked = t == i;
			pages_[t]->visible = t == i;
		}
		if (onSelect) { onSelect(i); }
	}

	Size TabView::preferred() const
	{
		const Size h = header_->preferred();
		Size s = { std::max(rect.w, h.w), rect.h };
		for (const Widget* p : pages_)
		{
			const Size ps = p->preferred();
			s.w = std::max(s.w, ps.w);
			s.h = std::max(s.h, headerH + gap + ps.h);
		}
		return s;
	}

	void TabView::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		header_->spacing = gap;
		header_->halign = tabAlign;
		if (buttonW > 0 && !tabs_.empty())	// too many for the header: they shrink to share it (captions clip)
		{
			const float fit = (px.w / s - gap * (tabs_.size() - 1)) / tabs_.size();
			for (Button* b : tabs_) { b->rect.w = std::floor(std::min(buttonW, fit)); }
		}
		header_->layout({ px.x, px.y, px.w, headerH * s }, s);
		const float top = (headerH + gap) * s;
		for (Widget* p : pages_) { if (p->visible) { p->layout({ px.x, px.y + top, px.w, std::max(0.0f, px.h - top) }, s); } }
	}

	// ---- Box ----

	Size Box::preferred() const
	{
		if (!fit) { return { rect.w, rect.h }; }
		float main = 0, cross = 0;
		int n = 0;
		for (const auto& c : children())
		{
			if (!c->visible) { continue; }
			const Size p = c->preferred();
			main += vertical ? p.h : p.w;
			cross = std::max(cross, vertical ? p.w : p.h);
			n++;
		}
		main += 2 * padding + (n > 1 ? (n - 1) * spacing : 0);
		cross += 2 * padding;
		return vertical ? Size{ std::max(rect.w, cross), main } : Size{ main, std::max(rect.h, cross) };
	}

	void Box::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		const float pad = padding * s;
		float at = (vertical ? px.y : px.x) + pad;
		const float c0 = (vertical ? px.x : px.y) + pad, cLen = (vertical ? px.w : px.h) - 2 * pad;
		for (const auto& c : children())
		{
			if (!c->visible) { continue; }
			const Size p = c->preferred();
			const float m = (vertical ? p.h : p.w) * s;
			float cl = (vertical ? p.w : p.h) * s, cp = c0;
			switch (vertical ? c->halign : c->valign)
			{
				case Align::kEnd: cp = c0 + cLen - cl; break;
				case Align::kCenter: cp = c0 + (cLen - cl) / 2; break;
				case Align::kStretch: cl = cLen; break;
				default: break;
			}
			c->place(vertical ? rounded(cp, at, cl, m) : rounded(at, cp, m, cl), s);
			at += m + spacing * s;
		}
	}

	// ---- Tabs ----

	Tabs::Tabs(Side side, std::string font) : side(side), font_(std::move(font))
	{
		solid = true;
		if (side == Side::kLeft)
		{
			list_ = &Widget::add<ScrollView>();
			static_cast<Box&>(list_->body()).spacing = gap;
			return;
		}
		prev_ = &Widget::add<Button>("", Look::kScrollLeft);
		next_ = &Widget::add<Button>("", Look::kScrollRight);
		prev_->onClicked = [this] { if (first_ > 0) { first_--; } };
		next_->onClicked = [this] { if (!tabs_.empty() && tabs_.back()->visible == false) { first_++; } };
	}

	size_t Tabs::add(const std::string& caption, const std::string& tip)
	{
		const size_t i = tabs_.size();
		Widget& parent = list_ ? list_->body() : *this;
		Button& b = parent.add<Button>(caption, list_ ? Look::kTabSide : Look::kTabTop, font_);
		b.rect.h = tabH;
		b.tooltip = tip;
		if (list_)
		{
			b.halign = Align::kStretch;
			b.align = TextAlign::kStart;
		}
		b.checked = i == selected_;
		b.sound = kSilent;	// Tab, only for another tab
		b.onClicked = [this, i] { if (i != selected_) { ui::sound("Tab"); select(i); } };
		tabs_.push_back(&b);
		return i;
	}

	void Tabs::heading(const std::string& text)
	{
		if (!list_) { return; }
		Widget& row = list_->body().add<Widget>();
		row.rect.h = tabH;
		Label& l = row.add<Label>(text, "Kenshi_StandardFont_MediumSmall");
		l.colour = res::colour("Accent").alpha(0.75f);
		l.rect = { 14, 4, 0, 0 };
		l.valign = Align::kEnd;
	}

	void Tabs::clear()
	{
		if (list_) { list_->body().clear(); }
		else { for (Button* b : tabs_) { remove(b); } }
		tabs_.clear();
		selected_ = first_ = 0;
	}

	void Tabs::select(size_t i)
	{
		if (i >= tabs_.size()) { return; }
		selected_ = i;
		for (size_t t = 0; t < tabs_.size(); t++) { tabs_[t]->checked = t == i; }
		reveal_ = true;
		if (list_)	// kLeft: keep the row in view (headings are rows too)
		{
			const auto& rows = list_->body().children();
			for (size_t r = 0; r < rows.size(); r++) { if (rows[r].get() == tabs_[i]) { list_->reveal(r); } }
		}
		if (onSelect) { onSelect(i); }
	}

	float Tabs::width(size_t from, size_t to) const
	{
		float w = 0;
		for (size_t i = from; i < to; i++) { w += tabs_[i]->preferred().w + (i > from ? gap : 0); }
		return w;
	}

	Size Tabs::preferred() const
	{
		if (list_)
		{
			const Size b = list_->body().preferred();
			return { std::max(rect.w, b.w + 28), std::max(rect.h, b.h) };
		}
		return { std::max(rect.w, width(0, tabs_.size())), std::max(rect.h, tabH) };
	}

	void Tabs::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		if (list_)
		{
			list_->place(px, s);
			return;
		}
		const size_t n = tabs_.size();
		const float avail = px.w / s, aw = std::round(tabH * 0.75f);
		const bool over = width(0, n) > avail;
		const float stripW = over ? avail - 2 * aw - 2 * gap : avail;
		if (!over) { first_ = 0; }
		else
		{
			first_ = std::min(first_, n - 1);
			if (reveal_)
			{
				if (selected_ < first_) { first_ = selected_; }
				while (first_ < selected_ && width(first_, selected_ + 1) > stripW) { first_++; }
			}
			while (first_ > 0 && width(first_ - 1, n) <= stripW) { first_--; }	// no empty space at the end
		}
		reveal_ = false;
		float x = px.x;
		bool room = true;
		for (size_t i = 0; i < n; i++)
		{
			Button& b = *tabs_[i];
			const float w = b.preferred().w * s;
			room = room && (i < first_ || x + w <= px.x + stripW * s + 0.5f);
			b.visible = i >= first_ && room;
			if (!b.visible) { continue; }
			b.place(rounded(x, px.y, w, tabH * s), s);
			x += w + gap * s;
		}
		prev_->visible = next_->visible = over;
		if (over)
		{
			const float y = px.y + (tabH - aw) * s / 2;
			next_->place(rounded(px.right() - aw * s, y, aw * s, aw * s), s);
			prev_->place(rounded(px.right() - (2 * aw + gap) * s, y, aw * s, aw * s), s);
			prev_->enabled = first_ > 0;
			next_->enabled = !tabs_.back()->visible;
		}
	}

	bool Tabs::onScroll(float dy)
	{
		if (list_ || !prev_->visible) { return false; }
		if (dy > 0 && prev_->enabled) { first_--; }
		else if (dy < 0 && next_->enabled) { first_++; }
		return true;
	}

	void Tabs::paint() const
	{
		const float b = std::max(1.0f, std::round(scale_));
		const Color hair = res::colour("Border");
		if (list_) { gfx::fill({ screen_.right() - b, screen_.y, b, screen_.h }, hair); }
		else { gfx::fill({ screen_.x, screen_.y + std::round(tabH * scale_) - b, screen_.w, b }, hair); }
	}

	// ---- Flex ----

	void Flex::set(const Widget& child, FlexItem it)
	{
		for (auto& [w, i] : items_) { if (w == &child) { i = it; return; } }
		items_.push_back({ &child, it });
	}

	FlexItem Flex::item(const Widget& child) const
	{
		for (const auto& [w, i] : items_) { if (w == &child) { return i; } }
		return {};
	}

	float Flex::start(const Widget& c, const FlexItem& it) const
	{
		if (it.basis >= 0) { return it.basis; }
		if (it.fill > 0) { return 0; }
		const Size p = c.preferred();
		return vertical ? p.h : p.w;
	}

	float Flex::base(const Widget& c, const FlexItem& it) const { return std::clamp(start(c, it), it.min, std::max(it.min, it.max)); }

	Size Flex::preferred() const
	{
		float main = 0, cross = 0;
		int n = 0;
		for (const auto& c : children())
		{
			if (!c->visible) { continue; }
			const Size p = c->preferred();
			main += base(*c, item(*c));
			cross = std::max(cross, vertical ? p.w : p.h);
			n++;
		}
		main += 2 * padding + (n > 1 ? (n - 1) * gap : 0);
		cross += 2 * padding;
		return vertical ? Size{ std::max(rect.w, cross), main } : Size{ main, std::max(rect.h, cross) };
	}

	void Flex::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		std::erase_if(items_, [this](const auto& e)	// children removed since
		{
			for (const auto& c : children()) { if (c.get() == e.first) { return false; } }
			return true;
		});
		struct Slot { Widget* w; FlexItem it; float len; bool frozen; };
		std::vector<Slot> slots;
		for (const auto& c : children()) { if (c->visible) { slots.push_back({ c.get(), item(*c), 0, false }); } }
		const float len = (vertical ? px.h : px.w) / s - 2 * padding - (slots.size() > 1 ? (slots.size() - 1) * gap : 0);
		for (Slot& sl : slots)
		{
			sl.len = base(*sl.w, sl.it);
			sl.frozen = sl.it.fill <= 0;
		}
		// Share what is left by weight; a child that hits its min / max keeps it and the rest is shared again.
		for (bool again = true; again;)
		{
			again = false;
			float free = len, weight = 0;
			for (const Slot& sl : slots) { free -= sl.frozen ? sl.len : start(*sl.w, sl.it); if (!sl.frozen) { weight += sl.it.fill; } }
			if (weight <= 0) { break; }
			for (Slot& sl : slots)
			{
				if (sl.frozen) { continue; }
				const float want = start(*sl.w, sl.it) + std::max(0.0f, free) * sl.it.fill / weight;
				sl.len = std::clamp(want, sl.it.min, std::max(sl.it.min, sl.it.max));
				if (sl.len != want) { sl.frozen = again = true; }
			}
		}
		float used = 0;
		for (const Slot& sl : slots) { used += sl.len; }
		const float spare = std::max(0.0f, len - used) * s;
		const float pad = padding * s;
		float at = (vertical ? px.y : px.x) + pad + (justify == Align::kEnd ? spare : justify == Align::kCenter ? spare / 2 : 0);
		const float c0 = (vertical ? px.x : px.y) + pad, cLen = std::max(0.0f, (vertical ? px.w : px.h) - 2 * pad);
		for (const Slot& sl : slots)
		{
			const Size p = sl.w->preferred();
			const float m = sl.len * s;
			float cl = std::min(cLen, (vertical ? p.w : p.h) * s), cp = c0;
			switch (vertical ? sl.w->halign : sl.w->valign)
			{
				case Align::kEnd: cp = c0 + cLen - cl; break;
				case Align::kCenter: cp = c0 + (cLen - cl) / 2; break;
				case Align::kStretch: cl = cLen; break;
				default: break;
			}
			sl.w->place(vertical ? rounded(cp, at, cl, m) : rounded(at, cp, m, cl), s);
			at += m + gap * s;
		}
	}

	// ---- ScrollView ----

	ScrollView::ScrollView()
	{
		solid = true;
		clipChildren = true;
		Box& b = add<Box>(true, 2);
		b.halign = Align::kStretch;
	}

	void ScrollView::place(const Rect& px, float s)
	{
		screen_ = px;
		scale_ = s;
		const float bar = (6 + 8) * s;	// the bar's column and a fixed gap before it, so text never touches the bar (user 2026-11-05)
		Widget& b = body();
		contentH_ = b.preferred().h;
		const float maxOff = std::max(0.0f, contentH_ - px.h / s);
		if (toEnd_ || (stickToEnd && atEnd_)) { offset = maxOff; }
		toEnd_ = false;
		offset = std::clamp(offset, 0.0f, maxOff);
		atEnd_ = offset >= maxOff - 0.5f;
		b.place(rounded(px.x, px.y - offset * s, px.w - (maxOff > 0 ? bar : 0), contentH_ * s), s);
		if (reveal_ >= 0 && (size_t)reveal_ < b.children().size())
		{
			const Rect& c = b.children()[reveal_]->screen();
			const float top = (c.y - b.screen().y) / s, h = c.h / s, viewH = px.h / s;
			const float to = std::clamp(top < offset ? top : top + h > offset + viewH ? top + h - viewH : offset, 0.0f, maxOff);
			if (to != offset)
			{
				offset = to;
				atEnd_ = offset >= maxOff - 0.5f;
				b.place(rounded(px.x, px.y - offset * s, px.w - (maxOff > 0 ? bar : 0), contentH_ * s), s);
			}
		}
	}

	bool ScrollView::onScroll(float dy)
	{
		reveal_ = -1;	// the user's scroll wins
		offset -= dy * 40;
		atEnd_ = false;	// re-evaluated at the next layout
		return true;
	}

	Rect ScrollView::thumb() const
	{
		if (!overflows()) { return {}; }
		const float viewH = screen_.h / scale_;
		const float h = std::max(12 * scale_, screen_.h * viewH / contentH_);
		const float y = screen_.y + (screen_.h - h) * (offset / (contentH_ - viewH));
		return rounded(screen_.right() - 4 * scale_, y, 3 * scale_, h);
	}

	bool ScrollView::onPress(Btn b, float x, float y)
	{
		if (b != Btn::kLeft || !overflows() || x < screen_.right() - 6 * scale_) { return true; }	// the bar's column only
		reveal_ = -1;	// the user's scroll wins
		const Rect th = thumb();
		if (y < th.y) { offset -= screen_.h / scale_; }
		else if (y >= th.bottom()) { offset += screen_.h / scale_; }
		else { grab_ = y - th.y; }
		atEnd_ = false;
		return true;
	}

	void ScrollView::onDrag(float, float y)
	{
		const float travel = screen_.h - thumb().h;
		if (grab_ < 0 || travel <= 0) { return; }
		offset = (contentH_ - screen_.h / scale_) * std::clamp((y - screen_.y - grab_) / travel, 0.0f, 1.0f);
		atEnd_ = false;
	}

	void ScrollView::paintOver() const
	{
		if (!overflows()) { return; }
		float mx, my;
		mouse(mx, my);
		const bool hot = grab_ >= 0 || (hovered(this) && mx >= screen_.right() - 6 * scale_);
		const Color main = res::colour("Main");
		gfx::fill({ screen_.right() - 4 * scale_, screen_.y, 3 * scale_, screen_.h }, main.alpha(0.15f));
		gfx::fill(thumb(), main.alpha(hot ? 1.0f : 0.7f));
	}

	// ---- List ----

	List::List(std::string fontName) : font(fonts::get(fontName))
	{
		solid = true;
		colour = res::colour("Main");
		hoverColour = res::colour("Special");
		selectedColour = res::colour("Good");
	}

	void List::build(float widthPx) const
	{
		rows_.resize(items.size());
		const float s = scale(), px = padX * s;
		float y = 0;
		for (size_t i = 0; i < items.size(); i++)
		{
			Row& r = rows_[i];
			r.label = numbered ? std::to_string(i + 1) + ". " + items[i] : items[i];
		}
		for (Row& r : rows_)
		{
			r.lines = font ? font->wrap(r.label, std::max(1.0f, widthPx - 2 * px)) : std::vector<std::string_view>{};
			r.y = y;
			r.h = std::max(rowMin * s, r.lines.size() * (font ? font->lineHeight() : 0) + 2 * padY * s);
			y += r.h;
		}
	}

	Size List::preferred() const
	{
		const float s = scale();
		build(halign == Align::kStretch && screen_.w > 0 ? screen_.w : rect.w * s);
		float h = 0;
		for (const Row& r : rows_) { h += r.h; }
		return { rect.w, std::max(rect.h, h / s) };
	}

	int List::rowAt(float y) const
	{
		for (size_t i = 0; i < rows_.size(); i++)
		{
			const float y0 = screen_.y + rows_[i].y;
			if (y >= y0 && y < y0 + rows_[i].h) { return (int)i; }
		}
		return -1;
	}

	bool List::onPress(Btn, float, float y)
	{
		pressRow_ = rowAt(y);
		return true;
	}

	void List::onClick(Btn b)
	{
		const int i = rowAt(mouseY());
		if (b != Btn::kLeft || i < 0 || i != pressRow_) { return; }
		selected = i;
		if (auto f = onPick) { f(i); }
	}

	void List::onDoubleClick(Btn b)
	{
		const int i = rowAt(mouseY());
		if (b != Btn::kLeft || i < 0) { return; }
		selected = i;
		if (auto f = onActivate) { f(i); }
		else if (auto g = onPick) { g(i); }
	}

	std::string List::tip() const
	{
		const int i = hovered(this) ? rowAt(mouseY()) : -1;
		return i >= 0 && i < (int)itemTips.size() && !itemTips[i].empty() ? itemTips[i] : tooltip;
	}

	bool List::onKey(int key, bool down)
	{
		const int i = key - SAPP_KEYCODE_1;
		if (!numbered || i < 0 || i > 8 || i >= (int)items.size()) { return false; }
		if (down)
		{
			selected = i;
			if (auto f = onPick) { f(i); }
		}
		return true;
	}

	void List::paint() const
	{
		if (!font) { return; }
		build(screen_.w);
		const int hot = hovered(this) ? rowAt(mouseY()) : -1;
		for (size_t i = 0; i < rows_.size(); i++)
		{
			const Row& r = rows_[i];
			const Rect box = { screen_.x, screen_.y + r.y, screen_.w, r.h };
			if ((int)i == selected) { gfx::fill(box, selectedColour.alpha(0.18f)); }
			if ((int)i == hot) { gfx::fill(box, hoverColour.alpha(0.10f)); }
			const bool own = i < itemColours.size() && itemColours[i].a > 0;
			const Color c = own ? itemColours[i] : (int)i == hot ? hoverColour : (int)i == selected ? selectedColour : colour;
			float y = std::round(box.y + (r.h - r.lines.size() * font->lineHeight()) / 2);
			for (std::string_view l : r.lines)
			{
				font->draw(box.x + padX * scale_, y, l, enabled ? c : c.alpha(0.5f));
				y += font->lineHeight();
			}
		}
	}

	// ---- Table ----

	Table::Table(std::vector<Column> cols, std::string fontName) : columns(std::move(cols)), font(fonts::get(fontName)),
		headerFont(fonts::get("Kenshi_PaintedTextFont_Small"))
	{
		solid = true;
		colour = res::colour("Main");
		hoverColour = res::colour("Special");
		selectedColour = res::colour("Good");
	}

	Rect Table::body() const
	{
		const float h = std::round(headerH * scale_);
		return { screen_.x, screen_.y + h, screen_.w, std::max(0.0f, screen_.h - h) };
	}

	float Table::maxOffset() const { return std::max(0.0f, rows_.size() * rowH - body().h / scale_); }

	std::vector<float> Table::columnX(float w) const
	{
		float fixed = 0;
		int shared = 0;
		for (const Column& c : columns)
		{
			fixed += c.width * scale_;
			if (c.width <= 0) { shared++; }
		}
		const float rest = shared ? std::max(0.0f, w - fixed) / shared : 0;
		std::vector<float> x(1, 0);
		for (const Column& c : columns) { x.push_back(std::round(x.back() + (c.width > 0 ? c.width * scale_ : rest))); }
		return x;
	}

	int Table::rowAt(float x, float y) const
	{
		const Rect b = body();
		if (x < b.x || x >= b.right() || y < b.y || y >= b.y + b.h) { return -1; }
		const int i = (int)std::floor(((y - b.y) / scale_ + offset) / rowH);
		return i < (int)rows_.size() ? i : -1;
	}

	int Table::columnAt(float x, float y) const
	{
		if (y < screen_.y || y >= body().y) { return -1; }
		const std::vector<float> cx = columnX(screen_.w);
		for (size_t i = 0; i + 1 < cx.size(); i++)
		{
			if (x >= screen_.x + cx[i] && x < screen_.x + cx[i + 1]) { return (int)i; }
		}
		return -1;
	}

	void Table::sort()
	{
		const int c = sortCol_;
		if (c < 0 || c >= (int)columns.size()) { return; }
		const auto less = [c](const Row& a, const Row& b)
		{
			if (c < (int)a.keys.size() && c < (int)b.keys.size() && !std::isnan(a.keys[c]) && !std::isnan(b.keys[c])) { return a.keys[c] < b.keys[c]; }
			const std::string& x = c < (int)a.cells.size() ? a.cells[c] : std::string();
			const std::string& y = c < (int)b.cells.size() ? b.cells[c] : std::string();
			return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
				[](char p, char q) { return std::tolower((unsigned char)p) < std::tolower((unsigned char)q); });
		};
		if (sortDesc_) { std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) { return less(b, a); }); }
		else { std::stable_sort(rows_.begin(), rows_.end(), less); }
	}

	void Table::setRows(std::vector<Row> rows)
	{
		const std::string id = selectedId();
		rows_ = std::move(rows);
		sort();
		selected_ = -1;
		for (size_t i = 0; i < rows_.size() && !id.empty(); i++)
		{
			if (rows_[i].id == id) { selected_ = (int)i; }
		}
		offset = std::clamp(offset, 0.0f, maxOffset());
	}

	void Table::sortBy(int column, bool descending)
	{
		const std::string id = selectedId();
		sortCol_ = column;
		sortDesc_ = descending;
		sort();
		for (size_t i = 0; i < rows_.size() && !id.empty(); i++)
		{
			if (rows_[i].id == id) { selected_ = (int)i; }
		}
		reveal(selected_);
	}

	void Table::select(const std::string& id)
	{
		int i = -1;
		for (size_t j = 0; j < rows_.size() && !id.empty(); j++)
		{
			if (rows_[j].id == id) { i = (int)j; }
		}
		pick(i, true);
	}

	void Table::pick(int i, bool notify)
	{
		const bool changed = i != selected_;
		selected_ = i;
		reveal(i);
		if (notify && changed && onSelect) { auto f = onSelect; f(selectedId()); }
	}

	void Table::reveal(int i)
	{
		if (i < 0) { return; }
		const float viewH = body().h / scale_;
		offset = std::min(std::max(offset, (i + 1) * rowH - viewH), i * rowH);	// its top wins when the view is shorter than a row
		offset = std::clamp(offset, 0.0f, maxOffset());
	}

	bool Table::onPress(Btn, float x, float y)
	{
		pressRow_ = rowAt(x, y);
		pressCol_ = columnAt(x, y);
		return true;
	}

	void Table::onClick(Btn b)
	{
		float x, y;
		mouse(x, y);
		if (b != Btn::kLeft) { return; }
		const int c = columnAt(x, y), i = rowAt(x, y);
		clickRow_ = -1;
		if (c >= 0 && c == pressCol_)
		{
			sortBy(c, c == sortCol_ ? !sortDesc_ : false);
			if (onSort) { auto f = onSort; f(sortCol_, sortDesc_); }
		}
		else if (i >= 0 && i == pressRow_)
		{
			pick(i, true);
			clickRow_ = i;
		}
	}

	void Table::onDoubleClick(Btn b)
	{
		float x, y;
		mouse(x, y);
		const int i = rowAt(x, y);
		if (b != Btn::kLeft || i < 0 || i != clickRow_) { onClick(b); return; }	// the second click elsewhere (a header, another row): a click
		pick(i, true);
		if (onActivate) { auto f = onActivate; f(rows_[i].id); }
	}

	bool Table::onScroll(float dy)
	{
		offset = std::clamp(offset - dy * rowH, 0.0f, maxOffset());
		return true;
	}

	bool Table::onKey(int key, bool down)
	{
		const int n = (int)rows_.size(), page = std::max(1, (int)(body().h / scale_ / rowH) - 1);
		int to = -2;	// -2: not a move
		switch (key)
		{
			case SAPP_KEYCODE_UP: to = selected_ < 0 ? n - 1 : selected_ - 1; break;
			case SAPP_KEYCODE_DOWN: to = selected_ + 1; break;
			case SAPP_KEYCODE_PAGE_UP: to = selected_ - page; break;
			case SAPP_KEYCODE_PAGE_DOWN: to = selected_ + page; break;
			case SAPP_KEYCODE_HOME: to = 0; break;
			case SAPP_KEYCODE_END: to = n - 1; break;
			case SAPP_KEYCODE_ENTER:
			case SAPP_KEYCODE_KP_ENTER:
				if (down && selected_ >= 0 && onActivate) { auto f = onActivate; f(rows_[selected_].id); }
				return true;
			case SAPP_KEYCODE_DELETE:
				if (down && selected_ >= 0 && onDelete) { auto f = onDelete; f(rows_[selected_].id); }
				return true;
			default:
				for (Widget* p = parent(); p; p = p->parent())
				{
					if (p->focusable()) { return p->onKey(key, down); }
				}
				return false;
		}
		if (down && n > 0) { pick(std::clamp(to, 0, n - 1), true); }
		return true;
	}

	std::string Table::tip() const
	{
		float x, y;
		mouse(x, y);
		const int i = hovered(this) ? rowAt(x, y) : -1;
		return i >= 0 && !rows_[i].tip.empty() ? rows_[i].tip : tooltip;
	}

	void Table::paint() const
	{
		style().draw(Look::kInset, screen_, scale_, State::kNormal, false);
		const std::vector<float> cx = columnX(screen_.w);
		const float s = scale_, px = std::round(padX * s);
		const Rect b = body();
		// the header: titles, the sorted one lit with an arrow, a rule under them
		for (size_t c = 0; c < columns.size() && headerFont; c++)
		{
			const Rect cell = { screen_.x + cx[c], screen_.y, cx[c + 1] - cx[c], b.y - screen_.y };
			const bool sorted = (int)c == sortCol_;
			const Color ink = sorted ? hoverColour : colour;
			const float tw = headerFont->width(columns[c].title), arrow = std::round(8 * s);
			const float tx = columns[c].align == TextAlign::kEnd ? cell.right() - px - tw - (sorted ? arrow + px / 2 : 0)
				: columns[c].align == TextAlign::kCenter ? std::round(cell.x + (cell.w - tw) / 2) : cell.x + px;
			gfx::pushClip(cell);
			headerFont->draw(tx, std::round(cell.y + (cell.h - headerFont->lineHeight()) / 2), columns[c].title, ink);
			if (sorted)
			{
				const float ax = tx + tw + px / 2, ay = std::round(cell.y + cell.h / 2);
				if (sortDesc_) { gfx::triangle(ax, ay - arrow / 3, ax + arrow, ay - arrow / 3, ax + arrow / 2, ay + arrow / 3, ink); }
				else { gfx::triangle(ax, ay + arrow / 3, ax + arrow / 2, ay - arrow / 3, ax + arrow, ay + arrow / 3, ink); }
			}
			gfx::popClip();
		}
		gfx::fill({ screen_.x + px / 2, b.y - std::max(1.0f, std::round(s)), screen_.w - px, std::max(1.0f, std::round(s)) }, colour.alpha(0.3f));
		if (!font) { return; }

		// the rows
		float mx, my;
		mouse(mx, my);
		const int hot = hovered(this) ? rowAt(mx, my) : -1;
		const float off = std::clamp(offset, 0.0f, maxOffset()), rh = rowH * s;
		const int first = std::max(0, (int)std::floor(off / rowH)), last = std::min((int)rows_.size(), (int)std::ceil((off * s + b.h) / rh) + 1);
		gfx::pushClip(b);
		for (int i = first; i < last; i++)
		{
			const Row& r = rows_[i];
			const Rect box = rounded(b.x, b.y + i * rh - off * s, b.w, rh);
			if (i % 2) { gfx::fill(box, colour.alpha(0.04f)); }
			if (i == selected_) { gfx::fill(box, selectedColour.alpha(0.18f)); }
			if (i == hot) { gfx::fill(box, hoverColour.alpha(0.10f)); }
			const Color ink = r.colour.a > 0 ? r.colour : i == hot ? hoverColour : i == selected_ ? selectedColour : colour;
			const float ty = std::round(box.y + (box.h - font->lineHeight()) / 2);
			for (size_t c = 0; c < columns.size() && c < r.cells.size(); c++)
			{
				const Rect cell = { b.x + cx[c], box.y, cx[c + 1] - cx[c], box.h };
				const float tw = font->width(r.cells[c]);
				const float tx = columns[c].align == TextAlign::kEnd ? cell.right() - px - tw
					: columns[c].align == TextAlign::kCenter ? std::round(cell.x + (cell.w - tw) / 2) : cell.x + px;
				gfx::pushClip({ cell.x + px / 2, cell.y, cell.w - px, cell.h });
				font->draw(tx, ty, r.cells[c], enabled ? ink : ink.alpha(0.5f));
				gfx::popClip();
			}
		}
		gfx::popClip();

		// the position, as a ScrollView shows it
		const float viewH = b.h / s, contentH = rows_.size() * rowH;
		if (contentH <= viewH) { return; }
		const float trackX = b.right() - 4 * s, w = 3 * s;
		const float h = std::max(12 * s, b.h * viewH / contentH);
		const float y = b.y + (b.h - h) * (off / (contentH - viewH));
		gfx::fill({ trackX, b.y, w, b.h }, colour.alpha(0.15f));
		gfx::fill(rounded(trackX, y, w, h), colour.alpha(0.7f));
	}

	// ---- TextLog ----

	class TextLog::Lines : public Widget
	{
	public:
		Lines(const TextLog& log, std::string fontName, size_t max) : font(fonts::get(fontName)), max(max), log_(log) {}
		struct Entry { std::string text; Color colour; };
		std::deque<Entry> entries;
		const Font* font;
		size_t max;

		float widthPx() const { return std::max(1.0f, log_.screen().w - 8 * scale()); }

		Size preferred() const override
		{
			if (!font) { return {}; }
			size_t n = 0;
			for (const Entry& e : entries) { n += font->wrap(e.text, widthPx()).size(); }
			return { 0, n * font->lineHeight() / scale() };
		}

	protected:
		void paint() const override
		{
			if (!font) { return; }
			const Rect& clip = gfx::clip();
			float y = screen_.y;
			const float lh = font->lineHeight();
			for (const Entry& e : entries)
			{
				for (std::string_view l : font->wrap(e.text, widthPx()))
				{
					if (y + lh >= clip.y && y <= clip.bottom()) { textLine(*font, screen_.x + 2 * scale_, y, l, e.colour, true); }
					y += lh;
				}
			}
		}

	private:
		const TextLog& log_;
	};

	TextLog::TextLog(std::string fontName, size_t maxLines)
	{
		stickToEnd = true;
		auto l = std::make_unique<Lines>(*this, std::move(fontName), maxLines);
		lines_ = l.get();
		lines_->halign = Align::kStretch;
		body().adopt(std::move(l));
	}

	void TextLog::push(std::string text, const Color& c)
	{
		lines_->entries.push_back({ std::move(text), c });
		while (lines_->entries.size() > lines_->max) { lines_->entries.pop_front(); }
	}

	void TextLog::clearLines() { lines_->entries.clear(); }

	// ---- Menu ----

	Menu::Menu(std::vector<Item> it, std::string fontName) : Panel(Look::kMenu, 10), items(std::move(it)), font(fonts::get(fontName)),
		titleFont(fonts::get("Kenshi_PaintedTextFont_Medium")) {}

	Size Menu::preferred() const
	{
		if (!font) { return { 100, 40 }; }
		const float s = scale();
		float w = 0;
		for (const Item& i : items) { w = std::max(w, i.title && titleFont ? titleFont->width(i.text) + 16 * s : font->width(i.text)); }
		return { std::max(rect.w, std::ceil(w / s) + 2 * padding + 16), items.size() * (font->lineHeight() / s + 6) + 2 * padding };
	}

	int Menu::itemAt(float y) const
	{
		if (!font) { return -1; }
		const Rect c = content();
		const float rowH = font->lineHeight() + 6 * scale_;
		const int i = (int)std::floor((y - c.y) / rowH);
		return y >= c.y && i >= 0 && i < (int)items.size() ? i : -1;
	}

	void Menu::onClick(Btn)
	{
		float mx, my;
		mouse(mx, my);
		pick(pickAt(mx, my));
	}

	void Menu::pick(int i)
	{
		if (i < 0 || i >= (int)items.size() || !items[i].enabled || items[i].title) { closePopups(); return; }
		auto action = items[i].action;	// closing destroys this menu
		closePopups();
		if (action) { action(); }
	}

	void Menu::paint() const
	{
		Panel::paint();
		if (!font) { return; }
		const Rect c = content();
		const float rowH = font->lineHeight() + 6 * scale_;
		float mx, my;
		mouse(mx, my);
		const int hot = screen_.contains(mx, my) ? itemAt(my) : -1;
		const Color main = style().textColour(Look::kMenu, State::kNormal, false), hl = res::colour("Title", main);
		for (size_t i = 0; i < items.size(); i++)
		{
			const Rect row = { c.x, c.y + i * rowH, c.w, rowH };
			if (items[i].title)
			{
				const Font& f = titleFont ? *titleFont : *font;
				f.draw(std::round(row.x + (row.w - f.width(items[i].text)) / 2), std::round(row.y + (rowH - f.lineHeight()) / 2), items[i].text, hl);
				gfx::fill(rounded(row.x + 4 * scale_, row.bottom() - std::max(1.0f, scale_), row.w - 8 * scale_, std::max(1.0f, scale_)), hl.alpha(0.5f));
				continue;
			}
			if ((int)i == hot && items[i].enabled) { gfx::fill(row, hl.alpha(0.25f)); }
			font->draw(row.x + 8 * scale_, row.y + 3 * scale_, items[i].text, items[i].enabled ? main : main.alpha(0.4f));
		}
	}

	// ---- ContextMenu ----

	ContextMenu::ContextMenu(std::vector<Item> it) : Menu(std::move(it))
	{
		look = Look::kPanel;	// Kenshi_FloatingPanelSkin: no client inset, the rows start at its edge
		padding = 0;
		escapeCloses = false;
		for (Item& i : items) { i.enabled = true; }	// no greyed rows: the host leaves out what cannot be done
	}

	Size ContextMenu::preferred() const
	{
		return { std::round(kWidth * root().screen().w / scale()), top(items.size()) + kFootH };
	}

	Color ContextMenu::valueColour(const std::string& value)
	{
		char* end;
		const long n = std::strtol(value.c_str(), &end, 10);
		if (end == value.c_str()) { return res::colour("Accent"); }
		const float s = std::clamp(n / 100.0f, 0.0f, 1.0f);
		return { 1 - s, s, 0, 1 };
	}

	bool ContextMenu::values() const
	{
		for (const Item& i : items)
		{
			if (!i.value.empty()) { return true; }
		}
		return false;
	}

	float ContextMenu::top(size_t i) const
	{
		float y = 0;
		for (size_t k = 0; k < i && k < items.size(); k++) { y += items[k].title ? kTitleH : kRowH; }
		return y;
	}

	int ContextMenu::itemAt(float y) const
	{
		for (size_t i = 0; i < items.size(); i++)
		{
			const float t = screen_.y + top(i) * scale_, h = (items[i].title ? kTitleH : kRowH) * scale_;
			if (y >= t && y < t + h) { return (int)i; }
		}
		return -1;
	}

	int ContextMenu::pickAt(float x, float y) const
	{
		const int i = itemAt(y);
		return i >= 0 && !items[i].title && row(i).contains(x, y) ? i : -1;
	}

	Rect ContextMenu::row(size_t i) const
	{
		if (i >= items.size()) { return {}; }
		const float s = scale_, y = screen_.y + top(i) * s;
		if (items[i].title) { return rounded(screen_.x, y, screen_.w, kTitleH * s); }
		return rounded(screen_.x + kInsetX * s, y + kInsetY * s, screen_.w - (kInsetX + (values() ? kValueW + kInsetY : kInsetX)) * s, (kRowH - 2 * kInsetY) * s);
	}

	void ContextMenu::paint() const
	{
		Panel::paint();
		if (!font) { return; }
		float mx, my;
		mouse(mx, my);
		const int hot = pickAt(mx, my);
		for (size_t i = 0; i < items.size(); i++)
		{
			const Item& it = items[i];
			const Rect r = row(i);
			if (it.title)
			{
				style().draw(Look::kField, r, scale_, State::kNormal, false);
				const Font& f = titleFont ? *titleFont : *font;
				const float w = f.width(it.text);
				gfx::pushClip(r);	// the width never grows: a long name is cut
				f.draw(std::round(r.x + std::max(0.0f, r.w - w) / 2), std::round(r.y + (r.h - f.lineHeight()) / 2), it.text, style().textColour(Look::kField, State::kNormal, false));
				gfx::popClip();
				continue;
			}
			// hover: the original's Kenshi_Button1Skin keeps its face and turns the caption gold; flat has its own hover look
			const State st = (int)i != hot ? State::kNormal : pressed(this) ? State::kPushed : State::kHighlighted;
			style().draw(Look::kButton, r, scale_, st, false);
			const Rect area = style().textArea(Look::kButton, r, scale_, st, false);
			std::string caption = it.text;	// the original's button font draws capitals
			for (char& c : caption) { c = (char)std::toupper((unsigned char)c); }
			const float w = font->width(caption), ty = std::round(area.y + (area.h - font->lineHeight()) / 2);
			gfx::pushClip(area);
			font->draw(std::round(area.x + std::max(0.0f, area.w - w) / 2), ty, caption, style().textColour(Look::kButton, st, false));
			gfx::popClip();
			if (!it.value.empty())	// Value: Kenshi_TextboxStandardText, centred
			{
				const float vw = font->width(it.value), vx = screen_.right() - kValueW * scale_;
				font->draw(std::round(vx + (kValueW * scale_ - vw) / 2), ty, it.value, valueColour(it.value));
			}
		}
	}

	// ---- Toasts ----

	Toasts::Toasts(std::string fontName) : font(fonts::get(fontName)) {}

	void Toasts::prune() const
	{
		const double t = now();
		std::erase_if(msgs_, [t](const Msg& m) { return m.until <= t; });
	}

	bool Toasts::empty() const { return shown() == 0; }

	size_t Toasts::shown() const
	{
		prune();
		return msgs_.size();
	}

	int Toasts::count(const std::string& text) const
	{
		prune();
		for (const Msg& m : msgs_) { if (m.text == text) { return m.count; } }
		return 0;
	}

	void Toasts::push(std::string text, const Color& c, double seconds)
	{
		prune();
		const double t = now();
		for (Msg& m : msgs_)
		{
			if (m.text == text)	// the same message again: one bubble, counted, its time restarted (it keeps its place)
			{
				m.colour = c;
				m.at = t;
				m.until = t + seconds;
				m.count++;
				return;
			}
		}
		while (!msgs_.empty() && msgs_.size() >= std::max<size_t>(maxShown, 1))	// full: the least recently pushed leaves early
		{
			msgs_.erase(std::min_element(msgs_.begin(), msgs_.end(), [](const Msg& a, const Msg& b) { return a.at < b.at; }));
		}
		msgs_.push_back({ std::move(text), c, t, t + seconds, 1 });
	}

	void TextureImage::paint() const
	{
		if (tex) { gfx::textured(tex, screen_, uv[0], uv[1], uv[2], uv[3], tint); }
	}

	Banner::Banner(std::string fontName) : font(fonts::get(fontName)) {}

	void Banner::show(std::string text, double seconds)
	{
		text_ = std::move(text);
		from_ = now();
		until_ = from_ + seconds;
	}

	bool Banner::shown() const { return !text_.empty() && now() < until_; }

	void Banner::paint() const
	{
		const double t = now();
		if (text_.empty() || t >= until_ || !font) { return; }
		// In over half a second, out over the last half of its time: still clear but faded 4 s in, as in the capture.
		const float a = (float)std::clamp(std::min((t - from_) / 0.5, (until_ - t) / ((until_ - from_) / 2)), 0.0, 1.0);
		const float s = scale_;
		const Rect rule = { std::round(screen_.x + (screen_.w - 610 * s) / 2), 0, std::round(610 * s), std::round(8 * s) };	// the layout's lines, 610 x 8 at 1080
		// Rules and name where the capture has them (the layout says 12 / 112 and centred; the game draws them further apart and the name lower).
		style().draw(Look::kRuleTop, { rule.x, std::round(screen_.y + 2 * s), rule.w, rule.h }, s, State::kNormal, false, Color{}.alpha(a));
		style().draw(Look::kRuleBottom, { rule.x, std::round(screen_.y + 117 * s), rule.w, rule.h }, s, State::kNormal, false, Color{}.alpha(a));
		const float w = font->width(text_);
		textLine(*font, std::round(screen_.x + (screen_.w - w) / 2), std::round(screen_.y + (screen_.h - font->lineHeight()) / 2 + 13 * s), text_,
			res::colour("Banner").alpha(a), true);
	}

	HudMessage::HudMessage(std::string fontName) : font(fonts::get(fontName)) {}

	void HudMessage::show(std::string text, const Color& c, double seconds)
	{
		advance();
		if (text.empty()) { msgs_.clear(); return; }
		for (const Msg& m : msgs_) { if (m.text == text) { return; } }
		msgs_.push_back({ std::move(text), c, seconds });
		advance();
	}

	void HudMessage::advance() const
	{
		const double t = now();
		while (!msgs_.empty() && msgs_.front().until > 0 && msgs_.front().until <= t) { msgs_.pop_front(); }
		if (!msgs_.empty() && msgs_.front().until == 0) { msgs_.front().until = t + msgs_.front().seconds + kFade; }
	}

	const std::string& HudMessage::text() const
	{
		static const std::string none;
		advance();
		return msgs_.empty() ? none : msgs_.front().text;
	}

	size_t HudMessage::queued() const
	{
		advance();
		return msgs_.empty() ? 0 : msgs_.size() - 1;
	}

	float HudMessage::alpha() const
	{
		advance();
		if (msgs_.empty()) { return 0; }
		return (float)std::clamp((msgs_.front().until - now()) / kFade, 0.0, 1.0);
	}

	void HudMessage::paint() const
	{
		const float a = alpha();
		if (a <= 0) { return; }
		const Msg& m = msgs_.front();
		style().draw(Look::kMessage, screen_, scale_, State::kNormal, false, Color{}.alpha(a));
		if (!font) { return; }
		// The text box: the middle 85.5 % of the width, the full height (the layout's 0.072 / 0.855).
		const float tw = std::round(screen_.w * 0.855f), tx = screen_.x + std::round(screen_.w * 0.0725f);
		const auto lines = font->wrap(m.text, tw);
		float y = std::round(screen_.y + (screen_.h - lines.size() * font->lineHeight()) / 2);
		for (std::string_view l : lines)
		{
			textLine(*font, std::round(tx + (tw - font->width(l)) / 2), y, l, m.colour.alpha(a), false);
			y += font->lineHeight();
		}
	}

	void Toasts::paint() const
	{
		prune();
		const double t = now();
		bubbles_.clear();
		if (!font) { return; }
		// Each message in its own dark rounded bubble (ours in every style, kenshi-k16.2.300.1): readable over any scene or the debug text.
		const float lh = font->lineHeight();
		const float padX = std::round(lh * 0.9f), padY = std::round(lh * 0.35f), gap = std::round(lh * 0.3f);
		float y = screen_.y;
		for (const Msg& m : msgs_)
		{
			const float fade = (float)std::clamp((m.until - t) / kFade, 0.0, 1.0);
			const std::string text = m.count > 1 ? m.text + " (x" + std::to_string(m.count) + ")" : m.text;	// lines views into it
			const auto lines = font->wrap(text, screen_.w - 2 * padX);
			float w = 0;
			for (std::string_view l : lines) { w = std::max(w, font->width(l)); }
			const Rect b = { std::round(screen_.x + (screen_.w - w) / 2 - padX), y, std::round(w + 2 * padX), lines.size() * lh + 2 * padY };
			gfx::rounded(b, (lh + 2 * padY) / 2, Color{ 0, 0, 0, bubbleAlpha * fade });
			bubbles_.push_back(b);
			float ty = y + padY;
			for (std::string_view l : lines)
			{
				textLine(*font, std::round(screen_.x + (screen_.w - font->width(l)) / 2), ty, l, m.colour.alpha(fade), true);
				ty += lh;
			}
			y = b.bottom() + gap;
		}
	}
}
