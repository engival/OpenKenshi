#pragma once

// The save directory behind SIMLINK orders 80-83 and the SAVES (32) record (docs/formats/SIMLINK.md SAVES,
// docs/formats/RUNTIME_SAVE.md). Plain filesystem helpers; the session half is in simlink_saves.cpp (Session::runSaveControl).

#include "ksim/save.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ksim::simlink {

inline constexpr std::size_t kSaveNameMaxChars = 64;
inline constexpr std::size_t kSaveNameMaxBytes = 255; // NAME_MAX: 64 characters of UTF-8 can be 256 bytes

// "" when the name is a valid save name, else "bad_value": empty, over 64 characters (or 255 bytes), invalid UTF-8
// (overlong forms, surrogates and code points past U+10FFFF included), a control character (U+0000-U+001F, U+007F,
// U+0080-U+009F), `/`, `\`, `:`, or a leading `.`. The file's name is exactly the name.
std::string checkSaveName(const std::string& name);

// $XDG_DATA_HOME/ksim/saves, else $HOME/.local/share/ksim/saves; throws when neither variable is set.
std::string defaultSaveDir();

// Creates the directory (and parents) and returns its canonical path. Throws std::runtime_error when it cannot be
// made or when it lies inside a forbidden tree (the game install: the fixed Kenshi path, and each of `forbidden`).
std::string prepareSaveDir(const std::string& dir, const std::vector<std::string>& forbidden = {});

struct SaveEntry
{
	std::string name;
	std::int64_t mtime = 0; // unix seconds
	SaveInfo info;
	std::uint8_t status = 2; // 0 ok; 1 older, migrates (never produced yet: no migrations); 2 unreadable or newer
};

// Every regular file of the directory with a valid save name (dot files, subdirectories, symlinks and invalid names are
// skipped), newest first, then by name. Reads only each file's head.
std::vector<SaveEntry> listSaves(const std::string& dir);

// "<dir>/<name>" for a name that passed checkSaveName.
std::string savePath(const std::string& dir, const std::string& name);

// Writes the bytes to a temp file beside the target and renames it over it. Throws std::runtime_error.
void writeFileAtomic(const std::string& path, const std::string& bytes);

} // namespace ksim::simlink
