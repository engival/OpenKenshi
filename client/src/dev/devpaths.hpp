#pragma once
// Where a dev build finds its own files when its build dir is not client/build (kenshi-cym.40.2.13): the source tree it was configured from.
// Dev only (CLIENT.md "Dev-only code is fenced"): the tree's path is a compile definition that a release build never carries.
#include <string>

namespace dev
{
	// The source tree's client/data when it holds ui/ (empty: none, or no tree path compiled in).
	std::string sourceTreeDataDir();
	// The shared master server for the source tree (client/tools/shared_server.sh); empty when it answers none. The deploy carries no
	// simdata/ beside it: the caller adds `--sim-data simData` unless it has one.
	struct SharedServer { std::string path, tree, simData; };
	SharedServer sharedServer();
}
