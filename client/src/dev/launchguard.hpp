#pragma once
// The dev-only launch guard (kenshi-k16.2.392, user 2026-11-19): kclient and kui_demo refuse to open a window from a Claude session
// that no gamescope wraps, so an agent that forgot client/tools/headless.sh cannot put a window on the user's desktop.
//
// Game code reaches it through dev/devhooks.hpp (src/dev/launchguard.cpp); kui_demo, a dev tool executable, includes this header itself.
namespace dev
{
	// A Claude session's shell (CLAUDECODE / any CLAUDE_* variable set) with no gamescope ancestor process: print one line to stderr
	// naming the program, headless.sh and KCLIENT_ALLOW_DESKTOP=1, and exit 1. KCLIENT_ALLOW_DESKTOP=1 (not "0") allows the window and
	// prints the skip instead. Linux walks /proc to find the ancestor; elsewhere it is a no-op (that OS has no headless.sh either).
	//
	// Call it where the command line's no-window modes (--help, --print-defaults, --check-options, ...) have already exited and before
	// anything opens a window: the game data resolve's first-run folder picker is a window too.
	void launchGuard(const char* program);
}
