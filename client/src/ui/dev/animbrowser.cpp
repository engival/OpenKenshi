#include "ui/dev/animbrowser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "sokol_app.h"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const char* const kButtons[] = { "prev", "next", "play", "stepback", "stepfwd", "side", "front", "top", "orbit", "lheel", "lball", "rheel", "rball", "none",
			"measure", "mark", "copy" };
		const char* const kCameras[] = { "side", "front", "top", "orbit" };
		const char* const kPoints[] = { "lheel", "lball", "rheel", "rball" };
		const char* const kOverlays[] = { "trails", "root", "bones" };

		std::string lower(std::string s)
		{
			for (char& c : s) { c = (char)std::tolower((unsigned char)c); }
			return s;
		}

		std::string fmt(const char* f, double a, double b = 0)
		{
			char buf[64];
			snprintf(buf, sizeof buf, f, a, b);
			return buf;
		}
	}

	// The window: its own keys before the caption window's Escape.
	class AnimBrowser::Win : public CaptionWindow
	{
	public:
		Win(AnimBrowser& b, std::function<void()> onClose) : CaptionWindow("ANIMATIONS", std::move(onClose), 0), b_(b) {}
		bool onKey(int key, bool down) override
		{
			if (key == SAPP_KEYCODE_ESCAPE) { return CaptionWindow::onKey(key, down); }
			return b_.key(key, down);
		}

	private:
		AnimBrowser& b_;
	};

	// The host's render: drawn over the box, a drag orbits, the wheel zooms.
	class AnimBrowser::View : public Widget
	{
	public:
		explicit View(AnimBrowser& b) : b_(b) { solid = true; tooltip = "Drag: orbit the camera. Wheel: zoom"; }
		PortraitRef ref;
		bool onPress(Btn btn, float x, float y) override
		{
			lx_ = x;
			ly_ = y;
			return btn == Btn::kLeft;
		}
		void onDrag(float x, float y) override
		{
			const float dx = (x - lx_) / scale_, dy = (y - ly_) / scale_;
			lx_ = x;
			ly_ = y;
			if ((dx != 0 || dy != 0) && b_.act_.orbit) { b_.act_.orbit(dx, dy); }
		}
		bool onScroll(float dy) override
		{
			if (b_.act_.zoom) { b_.act_.zoom(dy); }
			return true;
		}

	protected:
		void paint() const override
		{
			if (ref.tex)
			{
				gfx::textured(ref.tex, screen_, ref.uv[0], ref.uv[1], ref.uv[2], ref.uv[3], {});
				return;
			}
			gfx::fill(screen_, Color{ 0, 0, 0, 0.35f });
			if (const Font* f = fonts::get("Kenshi_StandardFont_Medium"))
			{
				const std::string t = "No render";
				f->draw(screen_.x + (screen_.w - f->width(t)) / 2, screen_.y + (screen_.h - f->lineHeight()) / 2, t, res::colour("Main").alpha(0.5f));
			}
		}

	private:
		AnimBrowser& b_;
		float lx_ = 0, ly_ = 0;
	};

	AnimBrowser::AnimBrowser(AnimBrowserActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<Win>(*this, [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		});
		win_->rect = { 40, 80, 1240, 760 };
		win_->resizable = true;
		win_->minSize = { 860, 520 };
		win_->visible = false;
		win_->body().rect.h = 760 - CaptionWindow::kHeaderH;
		layout_ = std::make_unique<Layout>(win_->body(), "animbrowser", [this](Layout& l) { bind(l); });
		if (!filter_) { bind(*layout_); }	// the file failed: spares, so the window works under the red box
	}

	AnimBrowser::~AnimBrowser()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void AnimBrowser::bind(Layout& l)
	{
		race_ = &l.at<Dropdown>("race");
		race_->onPick = [this](int i) { if (act_.race && i >= 0 && i < (int)last_.races.size()) { act_.race(last_.races[i]); } };
		female_ = &l.at<TickBox>("female");
		female_->checkable = false;	// the host's state ticks it
		female_->onClicked = [this] { if (act_.female) { act_.female(!last_.female); } };
		filter_ = &l.at<EditBox>("filter");
		filter_->text = filterText_;
		filter_->onChange = [this](const std::string& t)
		{
			filterText_ = t;
			clipsDirty_ = true;
		};
		filter_->onCommit = filter_->onChange;
		group_ = &l.at<Dropdown>("group");
		group_->onPick = [this](int i) { if (act_.group && i >= 0 && i <= (int)last_.groups.size()) { act_.group(i == 0 ? std::string() : last_.groups[i - 1]); } };
		follow_ = &l.at<TickBox>("follow");
		follow_->checkable = false;	// the host's state ticks it
		follow_->onClicked = [this] { if (act_.follow) { act_.follow(!last_.follow); } };
		usedBy_ = &l.at<Label>("usedby");
		rootText_ = &l.at<Label>("rootline");
		clips_ = &l.at<ScrollView>("clips");
		static_cast<Box&>(clips_->body()).spacing = 0;
		count_ = &l.at<Label>("count");
		Widget& slot = l.at<Widget>("view");
		if (slot.children().empty())	// a slot's children stay across a reload
		{
			View& v = slot.add<View>(*this);
			v.halign = v.valign = Widget::Align::kStretch;
		}
		view_ = slot.children().front().get();
		scrub_ = &l.at<Slider>("scrub");
		scrub_->onChange = [this](double v)
		{
			if (!act_.seek || last_.frames < 2) { return; }
			float t = 0;	// the clip's length, from the list
			for (const AnimClip& c : last_.clips) { if (c.name == last_.clip) { t = c.length; } }
			act_.seek((float)(v / (last_.frames - 1) * t));
		};
		frameText_ = &l.at<Label>("frame");
		speed_ = &l.at<Slider>("speed");
		speed_->notches = { 1.0 };
		speed_->onChange = [this](double v) { if (act_.speed) { act_.speed(v); } };
		speedText_ = &l.at<Label>("speedText");
		reply_ = &l.at<Label>("reply");
		readoutBox_ = &l.at<ScrollView>("readout");
		readout_ = &readoutBox_->body().add<Label>("", "Kenshi_StandardFont_Medium");
		readout_->wrap = true;
		readout_->halign = Widget::Align::kStretch;
		readout_->colour = res::colour("Main");
		bone_ = &l.at<Dropdown>("bone");
		bone_->onPick = [this](int i) { if (act_.plant && i >= 0 && i < (int)last_.bones.size()) { act_.plant(last_.bones[i]); } };
		buttons_.clear();
		for (const char* id : kButtons)
		{
			Button& b = l.at<Button>(id);
			b.textColour = res::colour("Main");	// enabled reads as enabled (the look's ButtonText is nearly the greyed tone); the tabs below drop it
			buttons_.push_back({ id, &b });
		}
		auto tab = [this](const char* id) { button(id)->look = Look::kTabTop; button(id)->textColour = {}; button(id)->textColour.a = 0; };
		for (const char* id : kCameras) { tab(id); button(id)->onClicked = [this, id] { if (act_.camera) { act_.camera(id); } }; }
		for (const char* id : kPoints) { tab(id); button(id)->onClicked = [this, id] { if (act_.plant) { act_.plant(id); } }; }
		tab("none");
		button("none")->onClicked = [this] { if (act_.plant) { act_.plant(""); } };
		button("prev")->onClicked = [this] { walk(-1); };
		button("next")->onClicked = [this] { walk(1); };
		button("play")->onClicked = [this] { if (act_.play) { act_.play(!last_.playing); } };
		button("stepback")->onClicked = [this] { if (act_.step) { act_.step(-1); } };
		button("stepfwd")->onClicked = [this] { if (act_.step) { act_.step(1); } };
		button("measure")->onClicked = [this] { if (act_.measure) { act_.measure(); } };
		button("mark")->onClicked = [this] { if (act_.mark) { act_.mark(); } };
		button("copy")->onClicked = [this] { if (act_.copy) { act_.copy(last_.readout); } };
		overlays_.clear();
		for (const char* id : kOverlays)
		{
			TickBox& t = l.at<TickBox>(id);
			t.checkable = false;
			t.onClicked = [this, id = std::string(id)]
			{
				const bool on = id == "trails" ? last_.trails : id == "root" ? last_.rootPath : last_.boneNames;
				if (act_.overlay) { act_.overlay(id, !on); }
			};
			overlays_.push_back({ id, &t });
		}
		rows_.clear();
		more_ = nullptr;
		clipsDirty_ = true;
		reveal_ = true;
		refresh();
	}

	Button* AnimBrowser::button(const std::string& id) const
	{
		for (const auto& [k, b] : buttons_) { if (k == id) { return b; } }
		return nullptr;
	}

	Button* AnimBrowser::overlayBox(const std::string& id) const
	{
		for (const auto& [k, b] : overlays_) { if (k == id) { return b; } }
		return nullptr;
	}

	void AnimBrowser::rebuildClips()
	{
		clipsDirty_ = false;
		const std::string needle = lower(filterText_);
		match_.clear();
		size_t inGroup = 0;
		for (size_t i = 0; i < last_.clips.size(); i++)
		{
			const AnimClip& c = last_.clips[i];
			if (!last_.group.empty() && std::find(c.groups.begin(), c.groups.end(), last_.group) == c.groups.end()) { continue; }
			inGroup++;
			if (needle.empty() || lower(c.name).find(needle) != std::string::npos) { match_.push_back(i); }
		}
		Widget& body = clips_->body();
		body.clear();
		rows_.clear();
		more_ = nullptr;
		empty_ = nullptr;
		if (match_.empty() && !last_.clips.empty())
		{
			empty_ = &body.add<Label>(inGroup == 0 ? "no clips in this group for " + (last_.race.empty() ? std::string("this race") : last_.race) : std::string("no clips match the filter"),
				"Kenshi_StandardFont_Medium");
			empty_->colour = res::colour("Main").alpha(0.6f);
		}
		for (size_t k = 0; k < match_.size() && k < kMaxRows; k++)
		{
			const AnimClip& c = last_.clips[match_[k]];
			Button& b = body.add<Button>(c.name, Look::kTabSide, "Kenshi_StandardFont_Medium");
			b.rect = { 0, 0, 0, 26 };
			b.halign = Widget::Align::kStretch;
			b.align = TextAlign::kStart;
			b.textColour = res::colour("Main");	// the look's ButtonText reads as greyed (as in the debug panel)
			b.tooltip = c.kind + ", " + fmt("%.2f s, ", c.length) + std::to_string(c.frames) + " frames" + (c.clip.empty() || c.clip == c.name ? "" : "; plays " + c.clip)
				+ (c.relocates ? "; relocates" : "");
			b.clipChildren = true;
			if (!c.groups.empty())
			{
				std::string g;
				for (const std::string& n : c.groups) { g += (g.empty() ? "" : ", ") + n; }
				Label& gl = b.add<Label>(g, "Kenshi_StandardFont_MediumSmall");
				gl.colour = res::colour("Main").alpha(0.45f);
				gl.halign = Widget::Align::kEnd;
				gl.valign = Widget::Align::kCenter;
				gl.rect.x = 6;
			}
			b.onClicked = [this, name = c.name] { if (act_.select) { act_.select(name); } };
			rows_.push_back(&b);
		}
		if (match_.size() > kMaxRows)
		{
			Label& m = body.add<Label>(std::to_string(match_.size() - kMaxRows) + " more: narrow the filter", "Kenshi_StandardFont_Medium");
			m.colour = res::colour("Main").alpha(0.6f);
			more_ = &m;
		}
		reveal_ = true;
	}

	void AnimBrowser::refresh()
	{
		race_->items = last_.races;
		race_->selected = -1;
		for (size_t i = 0; i < last_.races.size(); i++) { if (last_.races[i] == last_.race) { race_->selected = (int)i; } }
		female_->checked = last_.female;
		group_->items = { "All" };
		group_->items.insert(group_->items.end(), last_.groups.begin(), last_.groups.end());
		group_->selected = last_.group.empty() ? 0 : -1;
		for (size_t i = 0; i < last_.groups.size(); i++) { if (last_.groups[i] == last_.group) { group_->selected = (int)i + 1; } }
		if (clipsDirty_) { rebuildClips(); }
		for (size_t k = 0; k < rows_.size(); k++)
		{
			rows_[k]->checked = last_.clips[match_[k]].name == last_.clip;
			if (rows_[k]->checked && reveal_) { clips_->reveal(k); }
		}
		reveal_ = false;
		count_->text = std::to_string(match_.size()) + " of " + std::to_string(last_.clips.size());
		View& v = static_cast<View&>(*view_);
		v.ref = last_.view;
		button("play")->text = last_.playing ? "PAUSE" : "PLAY";
		scrub_->max = std::max(1, last_.frames - 1);
		if (!pressed(scrub_)) { scrub_->value = std::clamp<double>(last_.frame, 0, scrub_->max); }
		scrub_->enabled = last_.frames > 1;
		frameText_->text = last_.frames > 0 ? "f " + std::to_string(last_.frame) + " / " + std::to_string(last_.frames) + fmt("  %.2f s", last_.time) : "no clip";
		if (!pressed(speed_)) { speed_->value = std::clamp(last_.speed, speed_->min, speed_->max); }
		speedText_->text = fmt("%.2fx", last_.speed);
		for (const char* id : kCameras) { button(id)->checked = last_.camera == id; }
		bool point = last_.plant.empty();
		for (const char* id : kPoints) { button(id)->checked = last_.plant == id; point = point || last_.plant == id; }
		button("none")->checked = last_.plant.empty();
		bone_->items = last_.bones;
		bone_->selected = -1;
		if (!point) { for (size_t i = 0; i < last_.bones.size(); i++) { if (last_.bones[i] == last_.plant) { bone_->selected = (int)i; } } }
		overlayBox("trails")->checked = last_.trails;
		overlayBox("root")->checked = last_.rootPath;
		overlayBox("bones")->checked = last_.boneNames;
		const AnimClip* cur = nullptr;
		for (const AnimClip& c : last_.clips) { if (c.name == last_.clip) { cur = &c; } }
		const bool locked = cur && cur->autoFollow;
		follow_->checked = last_.follow;
		follow_->enabled = !locked;
		follow_->tooltip = locked ? "The game locks this clip's root (it relocates, or it is a technique)" : "Carry the root's travel: the camera and grid follow the body";
		usedBy_->text = cur ? cur->usedBy : std::string();
		usedBy_->visible = !usedBy_->text.empty();
		rootText_->text = last_.root;
		rootText_->visible = !last_.root.empty();
		readout_->text = last_.readout;
		button("copy")->enabled = !last_.readout.empty();
		reply_->text = last_.reply;
		reply_->visible = !last_.reply.empty();
	}

	void AnimBrowser::update(const AnimBrowserState& s)
	{
		const bool changed = !built_ || !(s == last_);
		if (changed)
		{
			if (!built_ || s.clips != last_.clips || s.group != last_.group || s.race != last_.race) { clipsDirty_ = true; }
			if (s.clip != last_.clip) { reveal_ = true; }
			last_ = s;
		}
		if (changed || clipsDirty_) { refresh(); }
		built_ = true;
		// The render box's size, once laid out and whenever it changes.
		const Rect& r = view_->screen();
		const int w = (int)std::lround(r.w), h = (int)std::lround(r.h);
		if (view_->shown() && w > 0 && h > 0 && (w != sentW_ || h != sentH_))
		{
			sentW_ = w;
			sentH_ = h;
			if (act_.viewSize) { act_.viewSize(w, h); }
		}
	}

	void AnimBrowser::walk(int dir)
	{
		if (match_.empty() || !act_.select) { return; }
		const size_t n = match_.size();
		size_t at = n;
		for (size_t k = 0; k < n; k++) { if (last_.clips[match_[k]].name == last_.clip) { at = k; } }
		const size_t to = at == n ? (dir > 0 ? 0 : n - 1) : (at + n + dir) % n;
		act_.select(last_.clips[match_[to]].name);
	}

	bool AnimBrowser::key(int k, bool down)
	{
		switch (k)
		{
			case SAPP_KEYCODE_SPACE: if (down && act_.play) { act_.play(!last_.playing); } return true;
			case SAPP_KEYCODE_COMMA: if (down && act_.step) { act_.step(-1); } return true;
			case SAPP_KEYCODE_PERIOD: if (down && act_.step) { act_.step(1); } return true;
			case SAPP_KEYCODE_M: if (down && act_.mark) { act_.mark(); } return true;
			default: return false;
		}
	}

	void AnimBrowser::show(bool on)
	{
		if (win_->visible == on) { return; }
		if (on && !built_) { update(last_); }
		win_->visible = on;
		if (on)
		{
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool AnimBrowser::shown() const { return win_->visible; }
}
