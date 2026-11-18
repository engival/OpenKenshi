#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// Ogre-style resource lookup by bare name over the FileSystem= directories of <install>/resources.cfg.
// Each entry is one directory, non-recursive (Ogre FileSystem archives without "recursive" only list their own
// files). Names are matched case-insensitively (Kenshi shipped on Windows). All groups are flattened in file order and the
// first directory listing a name wins (Ogre's index insert does not overwrite). Mod directories are not indexed.
class ResourceIndex
{
public:
	// installRoot: the directory that holds resources.cfg and data/. Returns false if resources.cfg is missing.
	bool init(const std::string& installRoot);

	// Bare name ("door_n.dds"; a directory part is ignored) -> absolute path; "" if not in a resource directory.
	std::string find(const std::string& name) const;
	// A record path (".\data\buildings\x.mesh": backslashes, relative to the install root or its data/) if the file exists, else find() on the bare name.
	std::string locate(const std::string& path) const;

	size_t dirCount() const { return dirs_; }
	size_t fileCount() const { return files_.size(); }
	const std::string& root() const { return root_; }
	// Every indexed file with this extension (lower case, with dot), as absolute paths, in resource-location order:
	// resources.cfg dirs in file order, the dir's own listing order within one. Ogre parses scripts in that order
	// (ResourceGroupManager::parseResourceGroupScripts walks the group's locations), which decides duplicate script names.
	std::vector<std::string> withExtension(const std::string& ext) const;

private:
	std::string root_;
	size_t dirs_ = 0;
	std::unordered_map<std::string, std::string> files_;	// lower-case bare name -> absolute path
	std::vector<std::string> order_;			// the paths as they were first listed (the first directory listing a name wins)
};

std::string lower(std::string s);
