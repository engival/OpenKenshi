#pragma once
#include <string>
#include <vector>

// The two Valve text-KeyValues files the data-dir discovery reads (kenshi-379.36.1): Steam's libraryfolders.vdf and an appmanifest_<id>.acf.
// Parsed by nesting, not by line (the "apps" block of a library lists "appid" "size" pairs that are not libraries).
namespace steamvdf
{
	// The library paths of a libraryfolders.vdf: "path" of each numbered block directly under "libraryfolders" (current format), or the value of a
	// numbered key at that depth (old flat format: "1" "/x/y"). VDF escapes (\\ and \") are undone. Order kept, duplicates dropped.
	std::vector<std::string> libraryPaths(const std::string& text);

	// The "installdir" of an appmanifest .acf (depth 1, inside "AppState"); empty when absent.
	std::string installDir(const std::string& text);
}
