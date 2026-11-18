#include "ui/dev/debugpanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ui/keys.hpp"
#include "ui/ui.hpp"

namespace ui
{
	namespace
	{
		const float kBodyH = DebugPanel::kH - CaptionWindow::kHeaderH, kRowH = 24, kChordW = 190;
		const char* const kTimeIds[5] = { "dawn", "noon", "dusk", "midnight", "plus1h" };	// debug.layout ids
		const char* const kTimePresets[5] = { "dawn", "noon", "dusk", "midnight", "+1h" };	// what DebugActions::time gets
		const char* const kGroups[] = { "GAME", "BUILD MODE", "DEBUG" };	// keys::Group order
		const char* const kMouse[] = {	// our own wording of kclient's mouse handling
			"Left click: select a character (Shift adds)",
			"Right click: attack the character there, else move (Shift queues)",
			"Right button held: look around",
			"Wheel: fly speed (zoom; in direct drive it leaves the drive)",
		};

		Label& heading(Box& parent, const char* text)
		{
			Label& l = parent.add<Label>(text, "Kenshi_PaintedTextFont_Medium");
			l.colour = res::colour("DebugHeading");
			return l;
		}

		void keyRow(Box& parent, const std::string& chord, const std::string& title, float w)
		{
			Widget& row = parent.add<Widget>();
			row.rect = { 0, 0, w, kRowH };
			row.halign = Widget::Align::kStretch;
			Label& k = row.add<Label>(chord, "Kenshi_StandardFont_Medium");
			k.colour = res::colour("DebugKey");
			k.valign = Widget::Align::kCenter;
			Label& t = row.add<Label>(title, "Kenshi_StandardFont_Medium");
			t.colour = res::colour("Main");
			t.rect.x = kChordW;
			t.valign = Widget::Align::kCenter;
		}

		std::string withoutPrefix(const std::string& title)	// "Debug: navmesh overlay" under DEBUG reads "Navmesh overlay"
		{
			const std::string p = "Debug: ";
			if (title.rfind(p, 0) != 0 || title.size() == p.size()) { return title; }
			std::string t = title.substr(p.size());
			t[0] = (char)std::toupper((unsigned char)t[0]);
			return t;
		}
	}

	DebugPanel::DebugPanel(DebugActions actions) : act_(std::move(actions))
	{
		win_ = &root().add<CaptionWindow>("DEBUG", [this]
		{
			show(false);
			if (act_.close) { act_.close(); }
		}, 0);
		win_->rect = { 8, 230, kW, kH };
		win_->visible = false;
		win_->resizable = true;
		win_->minSize = { 860, 400 };
		win_->body().rect.h = kBodyH;
		layout_ = std::make_unique<Layout>(win_->body(), "debug", [this](Layout& l) { bind(l); });
		if (!toggles_) { bind(*layout_); }	// the file failed: spares, so the panel works under the red box
	}

	void DebugPanel::bind(Layout& l)
	{
		toggles_ = &l.at<Flex>("toggles");
		scenesBox_ = &l.at<Flex>("scenesBox");
		scenes_ = &l.at<Flex>("scenes");
		list_ = &l.at<ScrollView>("keys");
		commands_ = &l.at<Flex>("commands");
		reply_ = &l.at<Label>("reply");
		reply_->colour = res::colour("Main");
		reply_->rect.w = kMidW;	// wraps to the column (its height from the width)
		weathers_ = &l.at<ScrollView>("weathers");
		static_cast<Box&>(weathers_->body()).spacing = 0;
		for (int i = 0; i < 5; i++)
		{
			time_[i] = &l.at<Button>(kTimeIds[i]);
			time_[i]->onClicked = [this, i] { if (last_.timeEnabled && act_.time) { act_.time(kTimePresets[i]); } };
		}
		Widget& clock = l.at<Widget>("clock");
		if (clock.children().empty())	// a slot: the dial moves to the new tree on a reload
		{
			dial_ = &clock.add<ClockDial>();
			dial_->rect = { 0, 0, 92, 92 };
			dial_->onPick = [this](float hour)
			{
				char buf[16];
				snprintf(buf, sizeof buf, "%.2f", hour);
				if (last_.timeEnabled && act_.time) { act_.time(buf); }
			};
		}
		commandButtons_.clear();
		weatherButtons_.clear();
		keys_ = &static_cast<Box&>(list_->body());
		keys_->spacing = 0;
		toggleButtons_.clear();
		sceneButtons_.clear();
		keyRows_.clear();
		extra_ = nullptr;
		if (built_)	// a reload: the file's widgets are new
		{
			rebuildSide();
			rebuildMid();
			revealWeather_ = true;
			refreshMid();
			rebuildKeys();
		}
	}

	DebugPanel::~DebugPanel()
	{
		layout_.reset();	// before its parent goes
		if (ready() && win_) { root().remove(win_); }
	}

	void DebugPanel::rebuildSide()
	{
		toggles_->clear();
		toggleButtons_.clear();
		for (const DebugToggle& t : last_.toggles)
		{
			Widget& row = toggles_->add<Widget>();
			row.rect = { 0, 0, kSideW, 28 };
			row.halign = Widget::Align::kStretch;
			TickBox& b = row.add<TickBox>(t.label);
			b.rect = { 0, 0, kSideW - 70, 28 };
			b.checkable = false;	// the host's state ticks it
			b.checked = t.on;
			b.onClicked = [this, id = t.id] { if (act_.toggle) { act_.toggle(id); } };
			toggleButtons_.push_back(&b);
			const std::string k = t.key.empty() ? std::string() : keys::label(t.key);
			if (k.empty()) { continue; }
			Label& kl = row.add<Label>(k, "Kenshi_StandardFont_MediumSmall");
			kl.colour = res::colour("DebugKey").alpha(0.8f);
			kl.halign = Widget::Align::kEnd;
			kl.valign = Widget::Align::kCenter;
		}
		scenes_->clear();
		sceneButtons_.clear();
		for (const std::string& name : last_.scenes)
		{
			Button& b = scenes_->add<Button>(name, Look::kButton, "Kenshi_StandardFont_Medium");
			b.rect = { 0, 0, kSideW, 28 };
			b.onClicked = [this, name] { if (act_.scene) { act_.scene(name); } };
			sceneButtons_.push_back(&b);
		}
		scenesBox_->visible = !last_.scenes.empty();
	}

	void DebugPanel::rebuildMid()
	{
		commands_->clear();
		commandButtons_.clear();
		for (const DebugCommand& c : last_.commands)
		{
			Button& b = commands_->add<Button>(c.label, Look::kButton, "Kenshi_StandardFont_Medium");
			b.rect = { 0, 0, kMidW, 28 };
			b.onClicked = [this, id = c.id]
			{
				for (const DebugCommand& d : last_.commands) { if (d.id == id && d.enabled && act_.command) { act_.command(id); } }
			};
			commandButtons_.push_back(&b);
		}
		Widget& body = weathers_->body();
		body.clear();
		weatherButtons_.clear();
		auto row = [&](const std::string& label, const std::string& name)
		{
			Button& b = body.add<Button>(label, Look::kTabSide, "Kenshi_StandardFont_Medium");
			b.rect = { 0, 0, kMidW - 8, 26 };
			b.halign = Widget::Align::kStretch;
			b.align = TextAlign::kStart;
			b.onClicked = [this, name] { if (last_.weatherEnabled && act_.weather) { act_.weather(name); } };
			weatherButtons_.push_back(&b);
		};
		row("Auto (normal weather)", "auto");
		for (const std::string& w : last_.weathers) { row(w, w); }
	}

	void DebugPanel::refreshMid()
	{
		// Enabled: Main text, so it stands apart from the greyed (the look's ButtonText is nearly as dim; client coordinator 2026-11-06).
		auto live = [](Button& b, bool on)
		{
			b.enabled = on;
			b.textColour = on ? res::colour("Main") : Color{ 0, 0, 0, 0 };
		};
		for (size_t i = 0; i < commandButtons_.size() && i < last_.commands.size(); i++) { live(*commandButtons_[i], last_.commands[i].enabled); }
		const bool autoOn = last_.weather.empty() || last_.weather == "auto";
		for (size_t i = 0; i < weatherButtons_.size(); i++)
		{
			live(*weatherButtons_[i], last_.weatherEnabled);
			weatherButtons_[i]->checked = i == 0 ? autoOn : !autoOn && last_.weathers[i - 1] == last_.weather;
			if (weatherButtons_[i]->checked && revealWeather_) { weathers_->reveal(i); }
		}
		for (Button* b : time_) { live(*b, last_.timeEnabled); }
		dial_->enabled = last_.timeEnabled;
		dial_->hour = last_.hour;
		dial_->sunrise = last_.sunrise;
		dial_->sunset = last_.sunset;
		revealWeather_ = false;
		reply_->text = last_.reply;
		reply_->visible = !last_.reply.empty();
	}

	void DebugPanel::rebuildKeys()
	{
		keys_->clear();
		keyRows_.clear();
		const float w = kW - kSideW - kMidW - 72;
		for (int g = 0; g < 3; g++)
		{
			bool any = false;
			for (const keys::Action& a : keys::actions())
			{
				if ((int)a.group != g) { continue; }
				const std::string k = keys::label(a.name);
				if (k.empty()) { continue; }
				if (!any) { heading(*keys_, kGroups[g]); }
				any = true;
				keyRow(*keys_, k, g == (int)keys::Group::kDebug ? withoutPrefix(a.title) : a.title, w);
				keyRows_.push_back({ kGroups[g], a.name });
			}
			if (any) { keys_->add<Widget>().rect = { 0, 0, w, 10 }; }
		}
		heading(*keys_, "MOUSE");
		for (const char* m : kMouse)
		{
			Label& l = keys_->add<Label>(m, "Kenshi_StandardFont_Medium");
			l.colour = res::colour("Main");
		}
		keys_->add<Widget>().rect = { 0, 0, w, 10 };
		extra_ = &keys_->add<RichText>(last_.extra);
		extra_->rect.w = w;
		extra_->halign = Widget::Align::kStretch;
	}

	void DebugPanel::update(const DebugState& s)
	{
		if (built_ && s == last_) { return; }
		const bool side = !built_ || s.toggles.size() != last_.toggles.size() || s.scenes != last_.scenes;
		const bool keys = !built_;
		const bool newWeather = !built_ || s.weather != last_.weather;
		const bool mid = !built_ || s.commands.size() != last_.commands.size() || s.weathers != last_.weathers
			|| [&] { for (size_t i = 0; i < s.commands.size(); i++) { if (s.commands[i].id != last_.commands[i].id) { return true; } } return false; }();
		last_ = s;
		if (mid) { rebuildMid(); }
		revealWeather_ = revealWeather_ || mid || newWeather;	// the current row scrolls into view only when it or the list changes
		for (size_t i = 0; i < s.commands.size(); i++) { commandButtons_[i]->text = s.commands[i].label; }
		refreshMid();
		if (side) { rebuildSide(); }
		else
		{
			for (size_t i = 0; i < s.toggles.size(); i++)
			{
				toggleButtons_[i]->text = s.toggles[i].label;
				toggleButtons_[i]->checked = s.toggles[i].on;
			}
		}
		if (keys) { rebuildKeys(); }
		extra_->text = s.extra;
		built_ = true;
	}

	void DebugPanel::show(bool on)
	{
		if (win_->visible == on) { return; }
		if (on && !built_) { update(last_); }
		win_->visible = on;
		if (on)
		{
			rebuildKeys();	// the bindings may have changed since
			win_->raise();
			focus(win_);
		}
		else if (focusedWidget() && !focusedWidget()->shown()) { focus(nullptr); }
	}

	bool DebugPanel::shown() const { return win_->visible; }
	Widget* DebugPanel::window() const { return win_; }
	Button* DebugPanel::toggle(size_t i) const { return i < toggleButtons_.size() ? toggleButtons_[i] : nullptr; }
	Button* DebugPanel::scene(size_t i) const { return i < sceneButtons_.size() ? sceneButtons_[i] : nullptr; }
	Widget* DebugPanel::scenesBox() const { return scenesBox_; }
	Widget* DebugPanel::extra() const { return extra_; }
	Button* DebugPanel::command(size_t i) const { return i < commandButtons_.size() ? commandButtons_[i] : nullptr; }
	Button* DebugPanel::weatherRow(size_t i) const { return i < weatherButtons_.size() ? weatherButtons_[i] : nullptr; }
	Button* DebugPanel::timeButton(size_t i) const { return i < 5 ? time_[i] : nullptr; }

	namespace
	{
		const float kTau = 6.2831853f;

		// The snapped hour under (x, y) on a dial in r (pixels): midnight at the bottom, clockwise; < 0 at the centre, or past the rim when bounded.
		float dialHour(const Rect& r, float x, float y, bool bounded)
		{
			const float rad = std::min(r.w, r.h) / 2, dx = x - (r.x + r.w / 2), dy = y - (r.y + r.h / 2), d = std::hypot(dx, dy);
			if (rad <= 0 || d < 0.2f * rad || (bounded && d > rad)) { return -1; }
			float t = std::atan2(-dx, dy);
			if (t < 0) { t += kTau; }
			const float h = std::round(t / kTau * 96) / 4;
			return h >= 24 ? h - 24 : h;
		}

		std::string clockText(float hour)
		{
			const int m = (int)std::lround(hour * 60) % 1440;
			char buf[8];
			snprintf(buf, sizeof buf, "%02d:%02d", m / 60, m % 60);
			return buf;
		}

		bool daylight(float h, float rise, float set)	// set before rise: the day wraps midnight
		{
			return rise <= set ? h >= rise && h < set : h >= rise || h < set;
		}
	}

	ClockDial::ClockDial()
	{
		solid = true;
	}

	float ClockDial::hourAt(float x, float y) const { return dialHour(screen_, x, y, true); }

	Widget* ClockDial::hit(float x, float y)
	{
		return hourAt(x, y) >= 0 ? Widget::hit(x, y) : nullptr;
	}

	bool ClockDial::onPress(Btn b, float x, float y)
	{
		pick_ = b == Btn::kLeft ? hourAt(x, y) : -1;
		return true;
	}

	void ClockDial::onDrag(float x, float y)
	{
		const float h = dialHour(screen_, x, y, false);	// past the rim still steers; the centre keeps the last pick
		if (pick_ >= 0 && h >= 0) { pick_ = h; }
	}

	void ClockDial::onRelease(Btn b, float, float)
	{
		const float h = pick_;
		pick_ = -1;
		if (b == Btn::kLeft && h >= 0 && enabled && onPick) { onPick(h); }
	}

	std::string ClockDial::tip() const
	{
		float x, y;
		mouse(x, y);
		const float h = pressed(this) && pick_ >= 0 ? pick_ : hourAt(x, y);
		if (h >= 0) { return "Set the time: " + clockText(h); }
		return hour >= 0 ? "Game time " + clockText(hour) : tooltip;
	}

	void ClockDial::paint() const
	{
		const float b = scale_, cx = screen_.x + screen_.w / 2, cy = screen_.y + screen_.h / 2, rad = std::min(screen_.w, screen_.h) / 2;
		const float face = rad - 2 * b;
		auto px = [&](float h, float r) { return cx - r * std::sin(h / 24 * kTau); };
		auto py = [&](float h, float r) { return cy + r * std::cos(h / 24 * kTau); };
		auto wedge = [&](float h0, float h1, float r, const Color& c) { gfx::triangle(cx, cy, px(h0, r), py(h0, r), px(h1, r), py(h1, r), c); };
		auto line = [&](float x0, float y0, float x1, float y1, float w, const Color& c)
		{
			const float dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
			if (len <= 0) { return; }
			const float nx = -dy / len * w / 2, ny = dx / len * w / 2;
			gfx::triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
			gfx::triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
		};
		const float a = enabled ? 1.0f : 0.45f;
		const Color day = res::colour("Accent").alpha(0.30f * a), night = res::colour("Inset");
		for (int i = 0; i < 96; i++)	// a quarter hour a slice
		{
			const float h0 = i / 4.0f, h1 = (i + 1) / 4.0f;
			wedge(h0, h1, rad, res::colour("Edge"));
			wedge(h0, h1, rad - b, res::colour("Border"));
			wedge(h0, h1, face, night);
			if (daylight(h0 + 0.125f, sunrise, sunset)) { wedge(h0, h1, face, day); }
		}
		const Color ink = res::colour("Main").alpha(a);
		for (int h = 0; h < 24; h++)
		{
			const float len = (h % 6 == 0 ? 7 : 3) * b;
			line(px(h, face - len), py(h, face - len), px(h, face), py(h, face), (h % 6 == 0 ? 2 : 1) * b, ink.alpha(0.7f * a));
		}
		if (const Font* f = fonts::get("Kenshi_StandardFont_MediumSmall"))
		{
			for (int h = 0; h < 24; h += 6)
			{
				const std::string t = std::to_string(h);
				const float r = face - 17 * b;
				f->draw(std::round(px(h, r) - f->width(t) / 2), std::round(py(h, r) - f->lineHeight() / 2), t, ink.alpha(0.8f * a));
			}
		}
		float mx, my;
		mouse(mx, my);
		const bool held = pressed(this) && pick_ >= 0;
		if (!held && hovered(this) && enabled)	// where a click lands
		{
			const float h = hourAt(mx, my);
			if (h >= 0) { line(px(h, face - 7 * b), py(h, face - 7 * b), px(h, face), py(h, face), 3 * b, res::colour("Accent").alpha(0.8f)); }
		}
		if (hour >= 0) { line(cx, cy, px(hour, face - 9 * b), py(hour, face - 9 * b), 2.5f * b, ink); }
		if (held) { line(cx, cy, px(pick_, face - 4 * b), py(pick_, face - 4 * b), 2.5f * b, res::colour("Accent")); }
		for (int i = 0; i < 16; i++) { gfx::triangle(cx, cy, cx + 3 * b * std::cos(i * kTau / 16), cy + 3 * b * std::sin(i * kTau / 16), cx + 3 * b * std::cos((i + 1) * kTau / 16), cy + 3 * b * std::sin((i + 1) * kTau / 16), ink); }
	}
}
