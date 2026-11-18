#pragma once
#include <string>
#include <vector>

// Officially supported Steam Workshop mods (client/MODS.md). Found by the <id> in their _*.info, not by folder name.
struct ModInfo
{
	const char* key;
	const char* name;
	const char* id;
	bool used;		// false = recognised but not used by the client yet ("planned")
	std::string path;	// folder it was found in; empty = not installed
	bool enabled = true;	// default on when found
};

class Mods
{
public:
	Mods();

	// Scans <install>/mods/*/ then <steamapps>/workshop/content/233860/*/ (install first wins).
	// install is the parent of --data; steamapps is the install's parent's parent.
	void discover(const std::string& dataDir);
	// Loads kclient.cfg (cfg::load) and takes the mod.KEY options from it.
	void loadConfig();
	// "KEY=on|off" from the command line: pinned for this run, not saved. False on a bad key or value.
	bool set(const std::string& keyEqValue);
	// --save-mods: the current choices become the saved mod.KEY options (other options in the file are kept).
	bool saveConfig() const;
	void list() const;
	std::string configPath() const;

	ModInfo* find(const std::string& key);
	// Folder of an enabled, found mod; empty otherwise.
	std::string activePath(const std::string& key);

private:
	std::vector<ModInfo> mods_;
};
