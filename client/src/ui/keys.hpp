#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sokol_app.h"
#include "ui/widget.hpp"

// Key bindings (docs/UI.md "Keys"): every action is a cfg Text option `key.<action>` holding its keys, "S, Down" (at most three; "" = unbound),
// so they live in kclient.cfg and the Options window shows them as its Keys tab. Action names are the original's controls.cfg ones
// (toggle_inventory, speed_1, ...); defaults are the install's controls.cfg. `key.debug.*` are the client's development keys. A key is
// "[Ctrl+][Alt+][Shift+]Name" (keys::name lists the names). The host asks keys::pressed(e, "toggle_stats") in its event callback after
// ui::event, and keys::held("camera_forward") per frame. Two actions on one key in the same group (or a debug key on a game key; build
// and free camera keys clash only in their own group) are a conflict: reported at start and shown in red in the Options, never resolved
// for the player.
namespace ui::keys
{
	struct Chord
	{
		int key = 0;	// sapp_keycode; 0 = none
		uint32_t mods = 0;	// SAPP_MODIFIER_CTRL / ALT / SHIFT
		bool operator==(const Chord&) const = default;
	};

	// Build keys apply in build mode only and free camera keys while the free camera is on (--fly, the debug free camera), so either may
	// share game keys. Free camera keys are live without --debug (--fly runs without it); the host routes them only in the free camera.
	enum class Group { kGame, kBuild, kDebug, kFreeCamera };

	struct Action
	{
		std::string name;	// "toggle_inventory"; the option is "key." + name
		std::string title;	// "Inventory"
		Group group;
	};

	static constexpr int kSlots = 3;	// keys per action

	std::string name(const Chord& c);	// "Ctrl+Shift+F11"; "" for none
	bool parse(const std::string& text, Chord& c);	// case-insensitive; false on an unknown name
	const std::vector<Action>& actions();	// in the Options' order
	const Action* find(const std::string& action);
	std::vector<Chord> bindings(const std::string& action);	// its keys in slot order; empty: unbound or unknown
	// Puts c into the action's slot (c.key 0 clears it; a later slot moves up), as the user's choice (saved).
	void bind(const std::string& action, int slot, const Chord& c);

	// The debug gate (client main's --debug): without it the kDebug actions are inert (pressed / held false) and the Options'
	// Debug sections are not built. Set once at startup, before any window.
	void setDebug(bool on);
	bool debug();

	// A key press (not a repeat) of one of the action's keys, with exactly its modifiers.
	bool pressed(const sapp_event& e, const std::string& action);
	// One of its keys is down with at least its modifiers (extra ones allowed: Shift may speed a held camera key).
	bool held(const std::string& action);
	std::string label(const std::string& action);	// its first key, "" when unbound (tooltips: "Inventory (I)")

	static constexpr uint32_t kAllMods = SAPP_MODIFIER_CTRL | SAPP_MODIFIER_ALT | SAPP_MODIFIER_SHIFT;
	uint32_t modBit(int key);	// the modifier bit a key is (its own press may carry it), 0 for other keys

	struct Conflict { Chord chord; std::string a, b; };	// action a's key is also b's
	std::vector<Conflict> conflicts();
	std::vector<std::string> conflictsWith(const std::string& action, const Chord& c);	// the other actions c is a conflict with
	void report();	// prints conflicts() to stderr (the host at start)

	void track(const sapp_event& e);	// ui::event: the keys held down
	void releaseAll();	// focus lost: nothing is held
}

namespace ui
{
	// A key box (the Options' Keys tab): shows one slot of an action. A click starts capturing: the next key with its modifiers binds it
	// (a modifier alone binds on its release), Escape cancels, a right click clears the slot. Red when the key is a conflict; the tooltip says
	// with what.
	class KeyField : public Widget
	{
	public:
		KeyField(std::string action, int slot);
		~KeyField() override;
		const std::string action;
		const int slot;
		const Font* font;
		bool capturing() const;
		void onClick(Btn b) override;
		std::string tip() const override;

	protected:
		void paint() const override;

	private:
		int lastMod_ = 0;	// the last modifier key pressed alone, bound on its release
		bool capture(const sapp_event& e);
		void stop();
	};
}
