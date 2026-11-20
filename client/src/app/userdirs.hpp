#pragma once
#include <string>

// The per-user roots every file the client writes goes under (kenshi-379.31.3): one place, one answer per OS, so nothing is built from
// XDG_* / HOME on Windows again (under Proton Wine leaves the Linux HOME in place, and the cache, config and screenshots would go through
// Z: into the Linux user's own folders, shared with the Linux client; on real Windows HOME is usually unset and they landed beside the exe).
//
// The definitions are the OS layer's, one file per OS: platform/linux/userdirs.cpp, macos/userdirs.cpp, windows/userdirs.cpp. Only the
// declarations live here, in kc_app (the lowest client library), because cfg::path() (app/config.cpp) resolves the config file and kc_app
// cannot include platform/ (kc_platform is built on top of kc_app). Every executable that links kc_app links kc_platform, which defines them.
//
// Linux / macOS keep the layout the client has always used (byte-identical paths): $XDG_*_HOME with the $HOME fallback, macOS config under
// ~/Library/Application Support. Windows names them under OpenKenshi: %APPDATA%\OpenKenshi for config, %LOCALAPPDATA%\OpenKenshi\{cache,data}
// for the rest, so the files live with the user's other application data and never beside the exe.
namespace plat {

// The user's home root ($HOME; %USERPROFILE%). dev/devhost.cpp's default findings tree is the only caller (a developer machine path).
std::string userHomeDir();
// The per-user config root: its kclient/ holds kclient.cfg and audio.cfg, kenshi-client/ the saved data.conf.
std::string userConfigDir();
// The per-user cache root: kclient/ under it holds the lightbake and colour-map caches.
std::string userCacheDir();
// The per-user data root: kclient/screenshots (and cameras/, crashes/ beside it) go under it.
std::string userDataDir();

}
