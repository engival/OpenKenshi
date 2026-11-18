#pragma once
#include <string>
#include <unordered_map>

#include "resources.hpp"

// Ogre .material scripts, just enough to map a material name to its diffuse texture name.
// Diffuse = the first texture_unit with `texture_alias diffuse`, else a unit named diffuse*, else the first unit with a `texture`;
// a material without one inherits from its `: parent`. Kenshi's own StaticObject family sets black.dds as a placeholder and
// binds the real texture at runtime from game data (MATERIAL_SPEC), so most mesh materials resolve only to a placeholder.
class MaterialLib
{
public:
	// Parses every *.material found in the resource directories. Returns the number of materials read.
	size_t load(const ResourceIndex& res);

	// Diffuse texture name (as written in the script, directory part stripped) or "" if none. Follows inheritance.
	// A name declared by more than one script keeps the FIRST declaration (k16.2.256.2): Ogre's MaterialTranslator calls
	// MaterialManager::create, which answers a duplicate with null ("duplicate definition resolved by use previous"), and the
	// translator then returns without touching the material already there (Ogre 1.11.2 OgreScriptTranslator.cpp). Scripts are
	// parsed in resource-location order, as Ogre's ResourceGroupManager does (ResourceIndex::withExtension). '01-Default' is
	// declared by four loaded scripts; the first in that order, data/buildings/techtowers/th.material, names TALL_ATLAS03.psd,
	// which the game does not ship, so its submeshes take the part's MATERIAL_SPEC texture instead (the medkit props atlas
	// data/items/misc/medkit.material was what the last declaration won before: hpg's orange body and white rim).
	std::string diffuse(const std::string& material) const;
	bool known(const std::string& material) const { return mats_.count(lower(material)) != 0; }
	// black.dds / flat.dds: the placeholders of the base materials, not a real texture.
	static bool placeholder(const std::string& texture);
	size_t files = 0;

private:
	struct Material
	{
		std::string parent, diffuse;
	};
	std::unordered_map<std::string, Material> mats_;
};
