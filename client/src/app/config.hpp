#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Unified client options (docs/CLIENT.md "Options"): one file under the per-user config root (app/userdirs.hpp: $XDG_CONFIG_HOME/kclient/kclient.cfg,
// default ~/.config; Windows %APPDATA%\OpenKenshi\kclient\kclient.cfg), "key=value" lines with dotted
// keys by owner (`audio.music`, `ui.scale`, `mod.nicemap`). Every option is declared once, next to the code that uses it, as a static cfg::Bool / Int /
// Float / Text with its default, range and help line; the registry is the list a future options screen shows. Saving rewrites the values in place,
// drops a setting whose value equals its static compiled-in default (so it follows a future default change) and keeps comments, blank lines and
// unknown keys (another build's options survive). Edits made to the file while the client runs are picked up by poll().
// Thread-safe: any thread may read; generation() lets a worker thread notice changes cheaply.
namespace cfg
{
	enum class Type { kBool, kInt, kFloat, kText };

	// Where an option is listed (kenshi-k16.2.244.1). Every declaration leads with one: `cfg::Bool k{ cfg::Group{ "Shadows" }, "render.x", ... }`.
	// `group` is the cluster the options window, the console listing and the saved file show it under; `needs` is an optional one-line note
	// of what it depends on ("needs render.interior_light"); `section` is the tab and is derived from the key's prefix (sectionOfKey) unless a
	// declaration overrides it, e.g. a dev-only key under a gameplay prefix (Group{ "Run files", "", "Debug" }).
	struct Group
	{
		std::string group, needs, section;
		Group(const char* g, const char* n = "", const char* s = "") : group(g), needs(n), section(s) {}
		Group(const std::string& g) : group(g) {}
	};

	struct Info
	{
		std::string key, help, value, def;
		Type type;
		double min, max;
		bool pinned = false;	// set on the command line: the value in effect is not the file's
		std::vector<std::string> choices;	// Text only: the allowed values (empty = any text)
		std::string section, group, needs;	// where it is listed (Group above); section and group are never empty for a declared option
	};

	// The section tabs, in display order, and the section a key prefix belongs to ("" = a prefix no section claims: sections() test fails).
	const std::vector<std::string>& sectionNames();
	std::string sectionOfKey(const std::string& key);

	class Option
	{
	public:
		const std::string& key() const { return key_; }

	protected:
		Option(const Group& g, const char* key, Type t, std::string def, double min, double max, const char* help);
		std::string text() const;
		double number() const;

	private:
		std::string key_;
	};

	class Bool : public Option
	{
	public:
		Bool(const Group& g, const char* key, bool def, const char* help) : Option(g, key, Type::kBool, def ? "on" : "off", 0, 1, help) {}
		bool get() const { return number() != 0; }
		void set(bool v) const;
	};

	class Int : public Option
	{
	public:
		Int(const Group& g, const char* key, int def, int min, int max, const char* help) : Option(g, key, Type::kInt, std::to_string(def), min, max, help) {}
		int get() const { return (int)number(); }
		void set(int v) const;
	};

	class Float : public Option
	{
	public:
		Float(const Group& g, const char* key, double def, double min, double max, const char* help);
		double get() const { return number(); }
		void set(double v) const;
	};

	class Text : public Option
	{
	public:
		Text(const Group& g, const char* key, const char* def, const char* help) : Option(g, key, Type::kText, def, 0, 0, help) {}
		// One of a fixed list (the options screen shows a menu); def must be one of them.
		Text(const Group& g, const char* key, const char* def, std::vector<std::string> choices, const char* help);
		std::string get() const { return text(); }
		void set(const std::string& v) const;
	};

	std::string path();
	// A key this build no longer has: load() prints `message` once (one line per key) when the file still sets it; the key is ignored and kept for
	// saving like any other unknown key. Call before load().
	void retire(const char* key, const char* message);
	// Reads the file (missing = all defaults). Bad values are logged and keep the default; unknown keys are kept for saving.
	void load();
	// Writes now if anything changed since the last save (temp file + rename). Also done by poll() a second after the last set().
	bool save();
	// Main thread, every frame: at most once a second, reloads the file if someone else changed it and saves pending sets. true when values changed.
	bool poll();
	// "key=value" from the command line or a script; false (with a message) on an unknown key or a bad value.
	bool set(const std::string& keyEqValue);
	// The user's choice by key (the options screen): like Option::set, saved, and it ends a command-line pin.
	bool setUser(const std::string& key, const std::string& value);
	// Free-form keys that are not declared options (a family such as `window.<fingerprint>.rect`, windowgeom.hpp): the value of the key's line in the
	// file ("" when absent), and an in-place rewrite or append of it, saved with the next save(). They never appear in list().
	std::string getRaw(const std::string& key);
	void setRaw(const std::string& key, const std::string& value);
	uint64_t generation();	// bumped on every change of any value
	std::vector<Info> list();	// every declared option, in key order
	// The options of a section / group (case-insensitive name) or of a key prefix ("render." or "render"), in section, group, key order; the
	// console's `set <name>` lists these. Empty when nothing matches.
	std::vector<Info> find(const std::string& name);
	// One line per declared option that has no section or group, or a section no tab has (the options_groups test; kclient --check-options).
	std::vector<std::string> problems();
	// The defaults listing (--print-defaults): "# Section / Group" headers, then key=default lines, in section, group, key order.
	std::string defaultsText();
}
