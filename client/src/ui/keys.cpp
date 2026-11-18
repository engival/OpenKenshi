#include "ui/keys.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>

#include "config.hpp"
#include "ui/ui.hpp"

namespace ui::keys
{
	namespace
	{
		struct Def { const char* name; const char* keys; const char* title; Group group; };

		// The original's actions (controls.cfg; the exe's InputHandler table adds change_squad, prospect, toggle_jobs, which the file leaves
		// unbound), defaults from the install's controls.cfg (UNSURE: that file may hold the owner's changes; kinspect to check against the
		// exe's built-in table). Left out: the editor's (editor_*, gizmo_*, rebuild_navmesh, reload_biomes) and the mouse buttons
		// (mouse_select / mouse_command / mouse_rotate), which stay fixed for now. toggle_squads and toggle_options are ours (the original
		// opens squads from the SQD button only, its options from the Escape menu).
		const Def kDefs[] = {
			{ "camera_forward", "W, Up", "Camera forward", Group::kGame },
			{ "camera_back", "S, Down", "Camera back", Group::kGame },
			{ "camera_left", "A, Left", "Camera left", Group::kGame },
			{ "camera_right", "D, Right", "Camera right", Group::kGame },
			{ "camera_rotate_left", "Q", "Camera rotate left", Group::kGame },
			{ "camera_rotate_right", "E", "Camera rotate right", Group::kGame },
			{ "camera_tilt+", "Comma", "Camera tilt up", Group::kGame },
			{ "camera_tilt-", "Period", "Camera tilt down", Group::kGame },
			{ "camera_zoom_in", "Home", "Camera zoom in", Group::kGame },
			{ "camera_zoom_out", "End", "Camera zoom out", Group::kGame },
			{ "focus_char", "Z", "Focus the camera on the selection", Group::kGame },
			{ "toggle_fps_camera", "Semicolon", "First person camera", Group::kGame },
			{ "mouse_rotate", "LCtrl", "Rotate the camera with the mouse (held)", Group::kGame },	// controls.cfg also binds Mouse3: the host's
			{ "camera_drive", "V", "Hover camera / direct drive on the selection", Group::kGame },	// V: MINE
			{ "toggle_camera_grid", "Shift+Semicolon", "Camera grid", Group::kGame },
			{ "pause", "Space", "Pause", Group::kGame },
			{ "speed_1", "F2", "Game speed 1", Group::kGame },
			{ "speed_2", "F3", "Game speed 2", Group::kGame },
			{ "speed_3", "F4", "Game speed 3", Group::kGame },
			{ "quicksave", "F5", "Quick save", Group::kGame },
			{ "quickload", "Ctrl+F9", "Quick load", Group::kGame },
			{ "screenshot", "F12, PrintScreen", "Screenshot", Group::kGame },	// F12 as Steam's (user 2026-11-04); two slots, so F8 went
			{ "screenshot_delayed", "Alt+F12", "Screenshot after 3 seconds", Group::kGame },	// a shot while keys are held (ALT labels; user 2026-11-05)
			{ "toggle_bar", "F7", "Hide the interface", Group::kGame },
			{ "toggle_hints", "F6", "Control hints panel", Group::kGame },
			{ "highlight", "LAlt", "Highlight items and characters (held)", Group::kGame },
			{ "toggle_help", "F1", "Help", Group::kGame },
			{ "toggle_inventory", "I", "Inventory", Group::kGame },
			{ "toggle_stats", "C", "Character stats", Group::kGame },
			{ "toggle_map", "M", "Map", Group::kGame },
			{ "toggle_research", "T", "Research", Group::kGame },
			{ "toggle_crafting", "Y", "Crafting queue", Group::kGame },
			{ "toggle_faction", "G", "Faction", Group::kGame },
			{ "toggle_squads", "U", "Squads", Group::kGame },
			{ "toggle_options", "F10", "Options", Group::kGame },
			{ "toggle_build", "B", "Build", Group::kGame },
			{ "select_all", "Grave", "Select all", Group::kGame },
			{ "select_0", "1", "Select character 1 (twice: focus)", Group::kGame },
			{ "select_1", "2", "Select character 2", Group::kGame },
			{ "select_2", "3", "Select character 3", Group::kGame },
			{ "select_3", "4", "Select character 4", Group::kGame },
			{ "select_4", "5", "Select character 5", Group::kGame },
			{ "select_5", "6", "Select character 6", Group::kGame },
			{ "select_6", "7", "Select character 7", Group::kGame },
			{ "select_7", "8", "Select character 8", Group::kGame },
			{ "select_8", "9", "Select character 9", Group::kGame },
			{ "select_9", "0", "Select character 10", Group::kGame },
			{ "character_next", "Tab", "Next character", Group::kGame },
			{ "character_prev", "LBracket", "Previous character", Group::kGame },
			{ "change_squad", "", "Next squad", Group::kGame },
			{ "stop_movement", "Backspace", "Stop", Group::kGame },
			{ "floor_up", "R", "Floor up", Group::kGame },
			{ "floor_down", "F", "Floor down", Group::kGame },
			{ "toggle_sneak", "X", "Sneak", Group::kGame },
			{ "toggle_hold", "Num1", "Hold position", Group::kGame },
			{ "toggle_passive", "Num2", "Passive", Group::kGame },
			{ "toggle_ranged", "Num3", "Ranged", Group::kGame },
			{ "toggle_taunt", "Num5", "Taunt", Group::kGame },
			{ "cycle_run_speed", "Num6", "Run speed", Group::kGame },
			{ "medic", "Num7", "Medic", Group::kGame },
			{ "rescue", "Num8", "Rescue", Group::kGame },
			{ "toggle_block", "Num0", "Block", Group::kGame },
			{ "toggle_jobs", "", "Jobs", Group::kGame },
			{ "prospect", "", "Prospect", Group::kGame },
			{ "build_apply", "Space", "Build: place", Group::kBuild },
			{ "build_undo", "Shift+Backspace", "Build: undo", Group::kBuild },
			{ "build_rotate_left", "Comma", "Build: rotate left", Group::kBuild },
			{ "build_rotate_right", "Period", "Build: rotate right", Group::kBuild },
			{ "build_tilt_decrease", "LBracket, NumMinus", "Build: tilt down", Group::kBuild },
			{ "build_tilt_increase", "RBracket, NumPlus", "Build: tilt up", Group::kBuild },
			{ "build_move_up", "Equals", "Build: move up", Group::kBuild },
			{ "build_move_down", "Minus", "Build: move down", Group::kBuild },
			{ "playtest.notes", "Ctrl+N", "Playtest: the notes window", Group::kGame },	// during a run (CLIENT_GOALS "Playtest")
			// The client's development keys (kclient main.cpp), on Ctrl so they stay off the game's.
			{ "debug.help", "F8", "Debug: the debug panel", Group::kDebug },
			{ "debug.terrain_view", "Ctrl+F2", "Debug: terrain view mode", Group::kDebug },
			{ "debug.shadows", "Ctrl+F3", "Debug: shadows", Group::kDebug },
			{ "debug.squad_overlay", "Ctrl+F4", "Debug: squad overlay", Group::kDebug },
			{ "debug.navmesh", "Ctrl+F5", "Debug: navmesh overlay", Group::kDebug },
			{ "debug.trails", "Ctrl+F6", "Debug: trails (the last 30 s)", Group::kDebug },
			{ "debug.trails_panel", "Shift+F6", "Debug: the trails panel", Group::kDebug },
			{ "debug.lights_panel", "Shift+F7", "Debug: the light debugger window", Group::kDebug },
			{ "debug.foliage_more", "Ctrl+PageUp", "Debug: foliage range up", Group::kDebug },
			{ "debug.foliage_less", "Ctrl+PageDown", "Debug: foliage range down", Group::kDebug },
			{ "debug.walk", "Ctrl+G", "Debug: walk mode", Group::kDebug },
			{ "debug.follow", "Ctrl+C", "Debug: follow the selection", Group::kDebug },
			{ "debug.free_camera", "Ctrl+F", "Debug: free look camera", Group::kDebug },
			{ "debug.autorun", "Ctrl+Q", "Debug: autorun while following", Group::kDebug },
			{ "debug.mouse_look", "Ctrl+Tab", "Debug: mouse look", Group::kDebug },
			{ "debug.fly_up", "PageUp", "Debug: free camera up (held)", Group::kDebug },
			{ "debug.fly_down", "PageDown", "Debug: free camera down (held)", Group::kDebug },
			{ "debug.fly_fast", "LShift", "Debug: free camera fast (held)", Group::kDebug },
			{ "debug.clock_pause", "Ctrl+P", "Debug: pause the sky clock", Group::kDebug },
			{ "debug.clock_speed", "Ctrl+T", "Debug: sky clock speed", Group::kDebug },
			{ "debug.clock_back", "Ctrl+LBracket", "Debug: sky clock back half an hour", Group::kDebug },
			{ "debug.clock_forward", "Ctrl+RBracket", "Debug: sky clock forward half an hour", Group::kDebug },
			{ "debug.clock_back_far", "Ctrl+Shift+LBracket", "Debug: sky clock back 3 hours", Group::kDebug },
			{ "debug.clock_forward_far", "Ctrl+Shift+RBracket", "Debug: sky clock forward 3 hours", Group::kDebug },
			{ "debug.weather_prev", "Ctrl+Comma", "Debug: previous weather", Group::kDebug },
			{ "debug.weather_next", "Ctrl+Period", "Debug: next weather", Group::kDebug },
			{ "debug.map_zoom_in", "Ctrl+Equals", "Debug: map zoom in", Group::kDebug },
			{ "debug.map_zoom_out", "Ctrl+Minus", "Debug: map zoom out", Group::kDebug },
			{ "debug.full_map", "Ctrl+M", "Debug: full-screen world map (click teleports)", Group::kDebug },
		};

		struct Name { int key; const char* name; };
		const Name kNames[] = {
			{ SAPP_KEYCODE_SPACE, "Space" }, { SAPP_KEYCODE_TAB, "Tab" }, { SAPP_KEYCODE_BACKSPACE, "Backspace" }, { SAPP_KEYCODE_ENTER, "Enter" },
			{ SAPP_KEYCODE_ESCAPE, "Escape" }, { SAPP_KEYCODE_INSERT, "Insert" }, { SAPP_KEYCODE_DELETE, "Delete" }, { SAPP_KEYCODE_HOME, "Home" },
			{ SAPP_KEYCODE_END, "End" }, { SAPP_KEYCODE_PAGE_UP, "PageUp" }, { SAPP_KEYCODE_PAGE_DOWN, "PageDown" }, { SAPP_KEYCODE_UP, "Up" },
			{ SAPP_KEYCODE_DOWN, "Down" }, { SAPP_KEYCODE_LEFT, "Left" }, { SAPP_KEYCODE_RIGHT, "Right" }, { SAPP_KEYCODE_COMMA, "Comma" },
			{ SAPP_KEYCODE_PERIOD, "Period" }, { SAPP_KEYCODE_MINUS, "Minus" }, { SAPP_KEYCODE_EQUAL, "Equals" },
			{ SAPP_KEYCODE_LEFT_BRACKET, "LBracket" }, { SAPP_KEYCODE_RIGHT_BRACKET, "RBracket" }, { SAPP_KEYCODE_SEMICOLON, "Semicolon" },
			{ SAPP_KEYCODE_APOSTROPHE, "Apostrophe" }, { SAPP_KEYCODE_GRAVE_ACCENT, "Grave" }, { SAPP_KEYCODE_SLASH, "Slash" },
			{ SAPP_KEYCODE_BACKSLASH, "Backslash" }, { SAPP_KEYCODE_KP_ADD, "NumPlus" }, { SAPP_KEYCODE_KP_SUBTRACT, "NumMinus" },
			{ SAPP_KEYCODE_KP_MULTIPLY, "NumStar" }, { SAPP_KEYCODE_KP_DIVIDE, "NumSlash" }, { SAPP_KEYCODE_KP_ENTER, "NumEnter" },
			{ SAPP_KEYCODE_KP_DECIMAL, "NumPeriod" }, { SAPP_KEYCODE_PRINT_SCREEN, "PrintScreen" }, { SAPP_KEYCODE_PAUSE, "Pause" },
			{ SAPP_KEYCODE_SCROLL_LOCK, "ScrollLock" }, { SAPP_KEYCODE_CAPS_LOCK, "CapsLock" }, { SAPP_KEYCODE_NUM_LOCK, "NumLock" },
			{ SAPP_KEYCODE_LEFT_SHIFT, "LShift" }, { SAPP_KEYCODE_RIGHT_SHIFT, "RShift" }, { SAPP_KEYCODE_LEFT_CONTROL, "LCtrl" },
			{ SAPP_KEYCODE_RIGHT_CONTROL, "RCtrl" }, { SAPP_KEYCODE_LEFT_ALT, "LAlt" }, { SAPP_KEYCODE_RIGHT_ALT, "RAlt" },
		};

		struct Mod { uint32_t bit; const char* name; };
		const Mod kMods[] = { { SAPP_MODIFIER_CTRL, "Ctrl" }, { SAPP_MODIFIER_ALT, "Alt" }, { SAPP_MODIFIER_SHIFT, "Shift" } };
	}

	uint32_t modBit(int key)
	{
		switch (key)
		{
			case SAPP_KEYCODE_LEFT_SHIFT: case SAPP_KEYCODE_RIGHT_SHIFT: return SAPP_MODIFIER_SHIFT;
			case SAPP_KEYCODE_LEFT_CONTROL: case SAPP_KEYCODE_RIGHT_CONTROL: return SAPP_MODIFIER_CTRL;
			case SAPP_KEYCODE_LEFT_ALT: case SAPP_KEYCODE_RIGHT_ALT: return SAPP_MODIFIER_ALT;
			default: return 0;
		}
	}

	namespace
	{
		std::string lower(std::string s)
		{
			for (char& c : s) { c = (char)tolower((unsigned char)c); }
			return s;
		}

		std::string trim(const std::string& s)
		{
			const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
			return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
		}

		// "S, Down" -> its chords; bad names are dropped (with a message once per text).
		std::vector<Chord> parseList(const std::string& option, const std::string& text)
		{
			std::vector<Chord> out;
			size_t at = 0;
			while (at <= text.size())
			{
				const size_t comma = std::min(text.find(',', at), text.size());
				const std::string part = trim(text.substr(at, comma - at));
				at = comma + 1;
				if (part.empty()) { continue; }
				Chord c;
				if (!parse(part, c)) { fprintf(stderr, "kclient: %s: unknown key \"%s\"\n", option.c_str(), part.c_str()); continue; }
				if ((int)out.size() < kSlots && std::find(out.begin(), out.end(), c) == out.end()) { out.push_back(c); }
			}
			return out;
		}

		std::vector<Action>& table()
		{
			static std::vector<Action> t = []
			{
				std::vector<Action> v;
				for (const Def& d : kDefs) { v.push_back({ d.name, d.title, d.group }); }
				return v;
			}();
			return t;
		}

		// The options, declared at start like every cfg option (before cfg::load reads the file).
		std::deque<cfg::Text>& options()
		{
			static std::deque<cfg::Text> o = []
			{
				std::deque<cfg::Text> d;
				for (const Def& x : kDefs) { d.emplace_back(x.group == Group::kDebug ? cfg::Group{ "Debug keys", "", "Debug" } : cfg::Group{ "Keys" }, ("key." + std::string(x.name)).c_str(), x.keys, x.title); }
				return d;
			}();
			return o;
		}
		const bool kDeclared = (options(), true);

		// The parsed bindings, rebuilt when any option changes.
		const std::map<std::string, std::vector<Chord>>& parsed()
		{
			static std::map<std::string, std::vector<Chord>> m;
			static uint64_t gen = 0;
			if (gen != cfg::generation())
			{
				gen = cfg::generation();
				m.clear();
				for (const cfg::Text& o : options()) { m[o.key().substr(4)] = parseList(o.key(), o.get()); }
			}
			return m;
		}

		const std::vector<Chord>& of(const std::string& action)
		{
			static const std::vector<Chord> none;
			const auto& m = parsed();
			const auto it = m.find(action);
			if (it != m.end()) { return it->second; }
			static std::vector<std::string> told;
			if (std::find(told.begin(), told.end(), action) == told.end())
			{
				told.push_back(action);
				fprintf(stderr, "kclient: no key action \"%s\"\n", action.c_str());
			}
			return none;
		}

		bool down_[SAPP_MAX_KEYCODES] = {};
		bool debug_ = false;	// --debug: the kDebug actions are live

		// Two actions that may not share a key: the same group, or a debug key and a game one.
		bool clash(Group a, Group b)
		{
			return a == b || (a != Group::kBuild && b != Group::kBuild);
		}
	}

	std::string name(const Chord& c)
	{
		if (!c.key) { return {}; }
		std::string s;
		for (const Mod& m : kMods) { if (c.mods & m.bit) { s += std::string(m.name) + "+"; } }
		if ((c.key >= SAPP_KEYCODE_A && c.key <= SAPP_KEYCODE_Z) || (c.key >= SAPP_KEYCODE_0 && c.key <= SAPP_KEYCODE_9)) { return s + (char)c.key; }
		if (c.key >= SAPP_KEYCODE_F1 && c.key <= SAPP_KEYCODE_F25) { return s + "F" + std::to_string(c.key - SAPP_KEYCODE_F1 + 1); }
		if (c.key >= SAPP_KEYCODE_KP_0 && c.key <= SAPP_KEYCODE_KP_9) { return s + "Num" + std::to_string(c.key - SAPP_KEYCODE_KP_0); }
		for (const Name& n : kNames) { if (n.key == c.key) { return s + n.name; } }
		return s + "Key" + std::to_string(c.key);
	}

	bool parse(const std::string& text, Chord& c)
	{
		c = {};
		std::string rest = trim(text);
		for (bool more = true; more;)
		{
			more = false;
			for (const Mod& m : kMods)
			{
				const std::string p = lower(m.name) + "+";
				if (rest.size() > p.size() && lower(rest.substr(0, p.size())) == p)
				{
					c.mods |= m.bit;
					rest = trim(rest.substr(p.size()));
					more = true;
				}
			}
		}
		const std::string l = lower(rest);
		if (l.size() == 1 && ((l[0] >= 'a' && l[0] <= 'z') || (l[0] >= '0' && l[0] <= '9'))) { c.key = toupper((unsigned char)l[0]); }
		else if (l.size() >= 2 && l[0] == 'f' && isdigit((unsigned char)l[1]))
		{
			const int n = atoi(l.c_str() + 1);
			if (n >= 1 && n <= 25) { c.key = SAPP_KEYCODE_F1 + n - 1; }
		}
		else if (l.size() == 4 && l.compare(0, 3, "num") == 0 && isdigit((unsigned char)l[3])) { c.key = SAPP_KEYCODE_KP_0 + (l[3] - '0'); }
		else
		{
			for (const Name& n : kNames) { if (l == lower(n.name)) { c.key = n.key; } }
		}
		if (c.key && modBit(c.key)) { c.mods &= ~modBit(c.key); }	// "Shift+LShift" is LShift
		return c.key != 0;
	}

	const std::vector<Action>& actions() { return table(); }

	const Action* find(const std::string& action)
	{
		for (const Action& a : table()) { if (a.name == action) { return &a; } }
		return nullptr;
	}

	std::vector<Chord> bindings(const std::string& action)
	{
		const auto& m = parsed();
		const auto it = m.find(action);
		return it == m.end() ? std::vector<Chord>() : it->second;
	}

	void bind(const std::string& action, int slot, const Chord& c)
	{
		std::vector<Chord> b = bindings(action);
		if (slot < 0 || slot >= kSlots) { return; }
		if (slot < (int)b.size()) { b.erase(b.begin() + slot); }
		if (c.key)
		{
			b.erase(std::remove(b.begin(), b.end(), c), b.end());
			b.insert(b.begin() + std::min((size_t)slot, b.size()), c);
		}
		std::string text;
		for (const Chord& x : b) { text += (text.empty() ? "" : ", ") + name(x); }
		cfg::setUser("key." + action, text);
	}

	void setDebug(bool on) { debug_ = on; }
	bool debug() { return debug_; }

	// A kDebug action is live only with the debug gate (--debug); its bindings and conflicts stay, it never fires.
	namespace
	{
		bool live(const std::string& action)
		{
			const Action* a = find(action);
			return a && (debug_ || a->group != Group::kDebug);
		}
	}

	bool pressed(const sapp_event& e, const std::string& action)
	{
		if (e.type != SAPP_EVENTTYPE_KEY_DOWN || e.key_repeat || !live(action)) { return false; }
		const uint32_t mods = e.modifiers & kAllMods & ~modBit(e.key_code);
		for (const Chord& c : of(action)) { if (c.key == (int)e.key_code && c.mods == mods) { return true; } }
		return false;
	}

	bool held(const std::string& action)
	{
		if (!live(action)) { return false; }
		uint32_t mods = 0;
		for (int k = 0; k < SAPP_MAX_KEYCODES; k++) { if (down_[k]) { mods |= modBit(k); } }
		for (const Chord& c : of(action))
		{
			if (c.key > 0 && c.key < SAPP_MAX_KEYCODES && down_[c.key] && (c.mods & mods) == c.mods) { return true; }
		}
		return false;
	}

	std::string label(const std::string& action)
	{
		const std::vector<Chord>& b = of(action);
		return b.empty() ? std::string() : name(b[0]);
	}

	std::vector<std::string> conflictsWith(const std::string& action, const Chord& c)
	{
		std::vector<std::string> out;
		const Action* me = find(action);
		if (!me || !c.key) { return out; }
		for (const Action& a : table())
		{
			if (a.name == action || !clash(me->group, a.group)) { continue; }
			const std::vector<Chord>& b = of(a.name);
			if (std::find(b.begin(), b.end(), c) != b.end()) { out.push_back(a.name); }
		}
		return out;
	}

	std::vector<Conflict> conflicts()
	{
		std::vector<Conflict> out;
		const std::vector<Action>& t = table();
		for (size_t i = 0; i < t.size(); i++)
		{
			for (const Chord& c : of(t[i].name))
			{
				for (const std::string& other : conflictsWith(t[i].name, c))
				{
					const auto at = std::find_if(t.begin(), t.end(), [&](const Action& a) { return a.name == other; });
					if ((size_t)(at - t.begin()) > i) { out.push_back({ c, t[i].name, other }); }	// each pair once
				}
			}
		}
		return out;
	}

	void report()
	{
		for (const Conflict& c : conflicts()) { fprintf(stderr, "kclient: key conflict: %s is both %s and %s\n", name(c.chord).c_str(), c.a.c_str(), c.b.c_str()); }
	}

	void track(const sapp_event& e)
	{
		if (e.type == SAPP_EVENTTYPE_UNFOCUSED) { releaseAll(); }
		if ((e.type != SAPP_EVENTTYPE_KEY_DOWN && e.type != SAPP_EVENTTYPE_KEY_UP) || (int)e.key_code <= 0 || (int)e.key_code >= SAPP_MAX_KEYCODES) { return; }
		down_[e.key_code] = e.type == SAPP_EVENTTYPE_KEY_DOWN;
	}

	void releaseAll()
	{
		for (bool& d : down_) { d = false; }
	}
}

namespace ui
{
	namespace
	{
		KeyField* active_ = nullptr;	// the box taking the next key
	}

	KeyField::KeyField(std::string action, int slot) : action(std::move(action)), slot(slot), font(fonts::get("Kenshi_StandardFont_Medium"))
	{
		solid = true;
	}

	KeyField::~KeyField()
	{
		stop();
	}

	bool KeyField::capturing() const { return active_ == this; }

	void KeyField::onClick(Btn b)
	{
		if (b == Btn::kRight)
		{
			stop();
			keys::bind(action, slot, {});
			return;
		}
		if (b != Btn::kLeft) { return; }
		if (active_) { active_->stop(); }
		active_ = this;
		lastMod_ = 0;
		setInterceptor([this](const sapp_event& e) { return capture(e); });
	}

	void KeyField::stop()
	{
		if (active_ != this) { return; }
		active_ = nullptr;
		setInterceptor({});
	}

	// While capturing every key is ours; a press elsewhere ends it (and still goes to what was pressed).
	bool KeyField::capture(const sapp_event& e)
	{
		if (!shown())
		{
			stop();
			return false;
		}
		switch (e.type)
		{
			case SAPP_EVENTTYPE_KEY_DOWN:
			{
				if (e.key_repeat) { return true; }
				if (e.key_code == SAPP_KEYCODE_ESCAPE)
				{
					stop();
					return true;
				}
				if (keys::modBit(e.key_code))	// a modifier: wait, it may be the start of Ctrl+X
				{
					lastMod_ = e.key_code;
					return true;
				}
				const keys::Chord c = { (int)e.key_code, e.modifiers & keys::kAllMods };
				stop();
				keys::bind(action, slot, c);
				return true;
			}
			case SAPP_EVENTTYPE_KEY_UP:
				if (lastMod_ && e.key_code == lastMod_)	// released alone: the modifier itself
				{
					stop();
					keys::bind(action, slot, { lastMod_, 0 });
				}
				return true;
			case SAPP_EVENTTYPE_CHAR:
				return true;
			case SAPP_EVENTTYPE_MOUSE_DOWN:
			{
				float x, y;
				mouse(x, y);
				if (screen().contains(x, y)) { return false; }	// its own click: a right click clears, a left one starts again
				stop();
				return false;
			}
			default:
				return false;
		}
	}

	std::string KeyField::tip() const
	{
		if (capturing()) { return "Press a key (Escape: cancel)"; }
		const std::vector<keys::Chord> b = keys::bindings(action);
		if (slot >= (int)b.size()) { return "Click, then press a key"; }
		std::string t;
		for (const std::string& other : keys::conflictsWith(action, b[slot]))
		{
			const keys::Action* a = keys::find(other);
			t += (t.empty() ? "Also: " : ", ") + (a ? a->title : other);
		}
		return t.empty() ? "Click to change, right click to clear" : t;
	}

	void KeyField::paint() const
	{
		style().draw(Look::kInput, screen_, scale_, enabled ? (capturing() || hovered(this) ? State::kHighlighted : State::kNormal) : State::kDisabled, false);
		if (!font) { return; }
		const std::vector<keys::Chord> b = keys::bindings(action);
		std::string text = slot < (int)b.size() ? keys::name(b[slot]) : std::string();
		Color c = style().textColour(Look::kInput, enabled ? State::kNormal : State::kDisabled, false);
		if (capturing())
		{
			text = "...";
			c = res::colour("Main");
		}
		else if (slot < (int)b.size() && !keys::conflictsWith(action, b[slot]).empty()) { c = res::colour("TipBad", c); }
		gfx::pushClip(screen_.inset(std::round(4 * scale_)));
		font->draw(std::round(screen_.x + (screen_.w - font->width(text)) / 2), std::round(screen_.y + (screen_.h - font->lineHeight()) / 2), text, c);
		gfx::popClip();
	}
}
