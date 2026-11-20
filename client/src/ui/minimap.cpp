#include "ui/minimap.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const char* const kKinds[] = { "Town", "Outpost", "Village", "Camp", "Nest", "Workcamp", "SmallPlace", "Ruin" };	// Kenshi_MapMarkers' frames
		const char* const kRelations[] = { "MarkerPlayer", "MarkerAlly", "MarkerNeutral", "MarkerEnemy" };
		// Units: a place icon at half its frame (the overview's farthest level), a character dot, the camera arrow (the overview's).
		const float kIcon = 0.5f, kDot = 16, kCamera = 20;
		const float kWedge = 0.22f, kWedgeHalf = 35 * 3.14159265f / 180;	// the facing wedge: its length a share of the map's side, its half angle
		const float kDragSlop = 3;	// pixels before a press is a drag

		Rect frameOf(const char* name)
		{
			const ImageSet* is = res::images("Kenshi_MapMarkers");
			if (!is) { return {}; }
			auto it = is->frames.find(name);
			return it != is->frames.end() ? it->second : Rect{};
		}
	}

	double Minimap::span(int level) { return 4608.0 * (double)(1 << std::clamp(level, 0, kLevels - 1)); }

	// The window itself: the frame, the host's map inside it, the markers over the map. Its place: px (units from the parent's top-left; x < 0
	// the default top right), or while a drag is on, where the drag put it; always kept inside the parent.
	class Minimap::View : public Widget
	{
	public:
		const MinimapActions* act = nullptr;
		MinimapState state;	// the markers as the host last gave them
		int level = Minimap::kDefaultLevel;
		float px = -1, py = -1;

		View() { solid = true; }

		Size preferred() const override { return { kSize, kSize }; }

		Rect area() const { return parent() ? parent()->content() : screen_; }

		// (x, y) pixels moved inside the area for a side of kSize.
		void keepIn(float& x, float& y, float s) const
		{
			const Rect a = area();
			const float side = std::round(kSize * s);
			x = std::round(std::clamp(x, a.x, std::max(a.x, a.right() - side)));
			y = std::round(std::clamp(y, a.y, std::max(a.y, a.bottom() - side)));
		}

		void place(const Rect&, float s) override
		{
			const Rect a = area();
			float x, y;
			if (dragging_ && moved_)
			{
				x = baseX_ + dragX_;
				y = baseY_ + dragY_;
			}
			else if (px < 0)
			{
				x = a.right() - (kSize + kMargin) * s;
				y = a.y + kTop * s;
			}
			else
			{
				x = a.x + px * s;
				y = a.y + py * s;
			}
			keepIn(x, y, s);
			const float side = std::round(kSize * s);
			Widget::place({ x, y, side, side }, s);
		}

		Rect inner() const { return style().textArea(Look::kMapFrame, screen_, scale_, State::kNormal, false); }

		MapFrame frame() const
		{
			const Rect r = inner();
			return { r.x, r.y, r.w, r.h, state.map.cameraX, state.map.cameraZ, r.w > 0 ? r.w / span(level) : 0 };
		}

		void toScreen(double x, double z, float& sx, float& sy) const
		{
			const MapFrame f = frame();
			sx = (float)(f.x + f.w / 2 + (x - f.cx) * f.pxPerUnit);
			sy = (float)(f.y + f.h / 2 + (z - f.cz) * f.pxPerUnit);
		}

		Rect icon(const MapPlace& p) const
		{
			const Rect t = frameOf(kKinds[(int)p.kind]);
			if (t.w <= 0) { return {}; }
			float sx, sy;
			toScreen(p.x, p.z, sx, sy);
			const float w = t.w * kIcon * scale_, h = t.h * kIcon * scale_;
			const Rect r = { std::round(sx - w / 2), std::round(sy - h / 2), std::round(w), std::round(h) };
			const Rect c = r.intersect(inner());
			return c.w > 0 && c.h > 0 ? r : Rect{};
		}

		bool onPress(Btn b, float x, float y) override
		{
			if (b != Btn::kLeft) { return true; }	// taken, nothing done: the host's later
			raise();	// over the other windows, as a header press raises a window
			dragging_ = true;
			moved_ = false;
			pressX_ = x;
			pressY_ = y;
			baseX_ = screen_.x;
			baseY_ = screen_.y;
			dragX_ = dragY_ = 0;
			return true;
		}

		void onDrag(float x, float y) override
		{
			if (!dragging_) { return; }
			dragX_ = x - pressX_;
			dragY_ = y - pressY_;
			if (!moved_ && std::abs(dragX_) + std::abs(dragY_) >= kDragSlop) { moved_ = true; }
			if (moved_) { place(screen_, scale_); }
		}

		void onRelease(Btn b, float x, float y) override
		{
			if (b != Btn::kLeft || !dragging_) { return; }
			onDrag(x, y);
			const bool moved = moved_;
			dragging_ = moved_ = false;
			if (!moved || scale_ <= 0) { return; }
			float nx = baseX_ + dragX_, ny = baseY_ + dragY_;
			keepIn(nx, ny, scale_);
			const Rect a = area();
			px = (nx - a.x) / scale_;
			py = (ny - a.y) / scale_;
			place(screen_, scale_);
			if (act && act->moved) { act->moved(px, py); }
		}

		bool onScroll(float dy) override
		{
			wheel_ += dy;	// a fine-stepped wheel or a touchpad steps once its notches add up
			int steps = 0;
			for (; wheel_ >= 1; wheel_ -= 1) { steps--; }	// up: closer
			for (; wheel_ <= -1; wheel_ += 1) { steps++; }
			const int to = std::clamp(level + steps, 0, kLevels - 1);
			if (to != level)
			{
				level = to;
				if (act && act->level) { act->level(level); }
			}
			if (to == 0 || to == kLevels - 1) { wheel_ = 0; }	// no credit banked at the ends
			return true;
		}

	protected:
		void paint() const override
		{
			style().draw(Look::kMapFrame, screen_, scale_, State::kNormal, false);
			const MapFrame f = frame();
			if (f.pxPerUnit <= 0) { return; }
			const Rect r = inner();
			if (act && act->draw) { gfx::host(r, [draw = act->draw, f](float, float, float, float) { draw(f); }); }	// copied into the list: runs at the replay
			gfx::pushClip(r);
			for (int t = (int)MapTier::kMax; t >= (int)MapTier::kMin; t--)	// Max < Mid < Min, as the overview
			{
				for (const MapPlace& p : state.map.places)
				{
					if ((int)p.tier != t) { continue; }
					const Rect i = icon(p);
					if (i.w > 0) { gfx::sprite(i, frameOf(kKinds[(int)p.kind]), style().colour(kRelations[(int)p.relation], {})); }
				}
			}
			const Color player = style().colour("MarkerPlayer", {});
			const Rect dot = frameOf("Character");
			const float d = kDot * scale_;
			float sx, sy;
			for (const auto& [x, z] : state.map.characters)
			{
				toScreen(x, z, sx, sy);
				gfx::sprite({ std::round(sx - d / 2), std::round(sy - d / 2), d, d }, dot, player);
			}
			toScreen(state.map.cameraX, state.map.cameraZ, sx, sy);
			const float yaw = state.map.cameraYaw, len = r.w * kWedge;
			const float ax = std::sin(yaw - kWedgeHalf), ay = -std::cos(yaw - kWedgeHalf), bx = std::sin(yaw + kWedgeHalf), by = -std::cos(yaw + kWedgeHalf);
			gfx::triangle(sx, sy, sx + ax * len, sy + ay * len, sx + bx * len, sy + by * len, player.alpha(0.3f));
			gfx::spriteRotated(sx, sy, kCamera * scale_, kCamera * scale_, yaw, frameOf("Camera"), player);
			gfx::popClip();
		}

	private:
		bool dragging_ = false, moved_ = false;
		float pressX_ = 0, pressY_ = 0, baseX_ = 0, baseY_ = 0, dragX_ = 0, dragY_ = 0;
		float wheel_ = 0;
	};

	Minimap::Minimap(MinimapActions actions)
	{
		act_ = std::move(actions);
		view_ = &root().add<View>();
		view_->act = &act_;
		view_->visible = false;
	}

	Minimap::~Minimap()
	{
		if (ready() && view_) { root().remove(view_); }
	}

	void Minimap::update(const MinimapState& s)
	{
		if (s == last_) { return; }
		if (s.level != last_.level) { view_->level = std::clamp(s.level, 0, kLevels - 1); }
		if (s.x != last_.x || s.y != last_.y)
		{
			view_->px = s.x;
			view_->py = s.y;
		}
		view_->state = s;
		last_ = s;
	}

	void Minimap::show(bool on) { view_->visible = on; }
	bool Minimap::shown() const { return view_->visible; }
	int Minimap::level() const { return view_->level; }

	float Minimap::x() const
	{
		const Rect a = view_->area();
		const float s = scale();
		return s > 0 ? (view_->screen().x - a.x) / s : 0;
	}

	float Minimap::y() const
	{
		const Rect a = view_->area();
		const float s = scale();
		return s > 0 ? (view_->screen().y - a.y) / s : 0;
	}

	Widget* Minimap::window() const { return view_; }
	Rect Minimap::mapRect() const { return view_->inner(); }
	MapFrame Minimap::frame() const { return view_->frame(); }
	Rect Minimap::marker(size_t i) const { return i < view_->state.map.places.size() ? view_->icon(view_->state.map.places[i]) : Rect{}; }
}
