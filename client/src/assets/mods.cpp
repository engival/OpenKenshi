#include "assets/mods.hpp"

#include <cstdio>

#include "app/config.hpp"
#include "kworld/loadorder.hpp"

namespace
{

// One option per supported mod, "mod.KEY=on|off" in kclient.cfg (config.hpp); a found mod is used unless switched off.
const cfg::Bool kModOptions[] = {
	{ cfg::Group{ "Installed mods" }, "mod.nicemap", true, "Nice Map: the world map with zones, zone names and roads (when installed)" },
	{ cfg::Group{ "Installed mods" }, "mod.darkui", true, "Dark UI: dark skins and atlas for the UI style \"original\" (when installed; next start)" },
	{ cfg::Group{ "Installed mods" }, "mod.slopeless", true, "Slopeless (when installed; not used yet)" },
	{ cfg::Group{ "Installed mods" }, "mod.bughouse", true, "Less Buggy Bughouse (when installed; not used yet)" },
};

const cfg::Bool* option(const char* key)
{
	for (const cfg::Bool& o : kModOptions) { if (o.key() == std::string("mod.") + key) { return &o; } }
	return nullptr;
}

}

Mods::Mods()
{
	mods_ = {
		{ "nicemap", "Nice Map [Zones + Zone names + Roads]", "1613829988", true, {} },
		{ "darkui", "Dark UI", "1200632417", true, {} },
		{ "slopeless", "Slopeless", "1740462542", false, {} },
		{ "bughouse", "Less Buggy Bughouse", "2600663487", false, {} },
	};
}

void Mods::discover(const std::string& dataDir)
{
	for (const kworld::ModFolder& f : kworld::discoverModFolders(dataDir))
	{
		for (ModInfo& m : mods_) { if (m.path.empty() && f.id == m.id) { m.path = f.path; } }
	}
}

std::string Mods::configPath() const { return cfg::path(); }

void Mods::loadConfig()
{
	cfg::load();
	for (ModInfo& m : mods_) { if (const cfg::Bool* o = option(m.key)) { m.enabled = o->get(); } }
}

bool Mods::set(const std::string& keyEqValue)
{
	const size_t eq = keyEqValue.find('=');
	if (eq == std::string::npos) { return false; }
	ModInfo* m = find(keyEqValue.substr(0, eq));
	const std::string v = keyEqValue.substr(eq + 1);
	if (!m || (v != "on" && v != "off") || !cfg::set("mod." + keyEqValue)) { return false; }	// a command-line pin: not saved unless --save-mods
	m->enabled = v == "on";
	return true;
}

bool Mods::saveConfig() const
{
	for (const ModInfo& m : mods_) { if (const cfg::Bool* o = option(m.key)) { o->set(m.enabled); } }
	return cfg::save();
}

void Mods::list() const
{
	printf("%-10s %-38s %-10s %-8s %-8s %s\n", "key", "name", "id", "enabled", "use", "found at");
	for (const ModInfo& m : mods_)
	{
		printf("%-10s %-38s %-10s %-8s %-8s %s\n", m.key, m.name, m.id, m.enabled ? "on" : "off", m.used ? "used" : "planned", m.path.empty() ? "(not found)" : m.path.c_str());
	}
}

ModInfo* Mods::find(const std::string& key)
{
	for (ModInfo& m : mods_) { if (key == m.key) { return &m; } }
	return nullptr;
}

std::string Mods::activePath(const std::string& key)
{
	const ModInfo* m = find(key);
	return m && m->enabled ? m->path : std::string();
}
