// The runtime sim-data dir (kenshi-379.13.3): behaviour/ (the default Lua scripts) and ksim_ai_behaviours.tsv, the gameplay data a
// release ships beside the server binary as simdata/. Resolved once: setSimDataDir (--sim-data), else <exe dir>/simdata if it exists,
// else (builds with KSIM_BAKE_DATA_PATH only) the source tree's cpp/data.
#pragma once

#include <string>

namespace ksim {

// An explicit dir (--sim-data); empty clears it. Call before the first use.
void setSimDataDir(std::string dir);

// The resolved dir, "" when none exists. A dir given to setSimDataDir is returned as is (checked by simDataError).
std::string simDataDir();

// "" when the dir is usable, else a startup error naming every place looked.
std::string simDataError();

inline std::string simDataBehaviourDir() { return simDataDir() + "/behaviour/default"; }
inline std::string simDataAiTable() { return simDataDir() + "/ksim_ai_behaviours.tsv"; }

} // namespace ksim
