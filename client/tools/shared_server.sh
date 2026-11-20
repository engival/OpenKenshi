#!/bin/bash
# The shared OpenKenshi-server for a worktree (kenshi-k16.2.306.1): a client worktree whose branch changes nothing the
# server is built from runs the server the coordinator deployed from master, instead of building ksim_server again.
#
# usage: client/tools/shared_server.sh [--explain]
#        client/tools/shared_server.sh --cache BIN   (kenshi-k16.2.306.6.1: after a build, add BIN to the shared cache)
#   (no option)  prints the shared binary's path and exits 0; prints nothing when this worktree must build its own.
#   --explain    prints one line instead: 'shared PATH', or 'built (REASON)'; the master ref it used, its commit and the
#                merge base go to stderr.
#
# Rule 1 (kenshi-k16.2.306.6.1), the cpp/ tree key: when the worktree has no uncommitted or untracked change under cpp/, the
# server it needs is a pure function of `git rev-parse HEAD:cpp`. $HOME/git/kenshi/bin/OpenKenshi-server-cpp-<first 12 hex
# of that tree> is looked up from any branch or base (the coordinator's deploy hard-links one per master deploy; a client
# build of a clean tree is cached there by --cache, below). The entry handed out is `touch -h`ed: the coordinator prunes
# OpenKenshi-server-cpp-* untouched for 3 days, so trees in use stay. Rule 2 (the merge-base lookup below) is kept for
# one case rule 1 misses: deploys made before the cpp- links existed (OpenKenshi-server-<hash8> only). The wire-number rule
# (kenshi-k16.2.306.5.1, rule 3 below) sits between rule 2's cpp/ check (a) and its merge-base lookup (b).
#
# --cache BIN: copies BIN to OpenKenshi-server-cpp-<tree12> when cpp/ is clean by the same test and the name does not exist
# yet: a unique .tmp name in that dir, chmod 755, mv into place (atomic; concurrent workers race safely, the loser's tmp is
# removed). A build with uncommitted or untracked cpp/ changes is never cached. Prints 'cached PATH' / 'cache: SKIP-REASON'.
#
# Rule 2, the deploy: after every merge the coordinator builds master's server and copies it to
# $HOME/git/kenshi/bin/OpenKenshi-server-<hash8> (the newest 4 kept; OpenKenshi-server-master links the newest). This
# script answers only when both hold:
#  (a) the worktree's diff against its merge base with master touches no file the server is built from (committed and
#      uncommitted changes and untracked files all count). That is every path under cpp/: the target
#      (cpp/CMakeLists.txt `add_executable(ksim_server tools/ksim_server.cpp)`) compiles cpp/tools/ksim_server.cpp and
#      links ksim, kutil, kworld, knav, flecs and lua55, whose sources are all in cpp/src, cpp/third_party and cpp/cmake.
#      NO source outside cpp/ is compiled into it: client/CMakeLists.txt only does add_subdirectory(cpp) (the client
#      build's cpp/OpenKenshi-server is the same target from the same sources). Made at configure time in cpp/CMakeLists.txt:
#      build_info.cpp (the git hash the server reports).
#  (b) $HOME/git/kenshi/bin/OpenKenshi-server-<merge base's first 8 hex> exists and is executable. The deploy is keyed
#      by the master commit at the merge base, so a branch based on a client commit master does not hold yet falls back
#      to a build (master's server does not carry that cpp/).
#
# The master ref: this clone's `master` and every `<remote>/master` it has (the ds and dev remotes both have one); the
# newest commit of them wins, and --explain names it. Nothing is fetched here: a stale master only costs a build, it can
# never hand out a wrong binary (a cpp/ change master lacks is a cpp/ change against the merge base).
#
# The shared binary is not this tree's build: it is the client lead's build of master, so its baked game data and sim
# data paths (and KSIM_SCENARIOS_DIR, which callers sidestep with absolute --scenario-file paths) are that machine's. A
# caller therefore runs it with --data DIR (this tree's KENSHI_DATA) and --sim-data cpp/data — client/tools/server_bin.sh
# does that (SRV_ARGS) — and it reports master's git hash.
set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
bins="$HOME/git/kenshi/bin"
explain=0 cachebin=
[ "${1:-}" = --explain ] && explain=1
[ "${1:-}" = --cache ] && cachebin=${2:-}

# The cpp/ tree key, set when cpp/ is clean (no uncommitted, staged or untracked change; ignored build output does not count).
tree12= dirty=
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1
then
	dirty=$(git -C "$root" status --porcelain --untracked-files=all -- cpp 2>/dev/null | head -n 1)
	[ -z "$dirty" ] && tree12=$(git -C "$root" rev-parse HEAD:cpp 2>/dev/null | cut -c1-12)
fi

if [ -n "$cachebin" ] || [ "${1:-}" = --cache ]
then
	[ -x "$cachebin" ] || { echo "cache: no executable '$cachebin'"; exit 0; }
	[ -n "$tree12" ] || { echo "cache: skipped (cpp/ is not clean${dirty:+: $dirty})"; exit 0; }
	dest=$bins/OpenKenshi-server-cpp-$tree12
	[ -e "$dest" ] && { echo "cache: skipped ($dest exists)"; exit 0; }
	mkdir -p "$bins" 2>/dev/null
	tmp=$(mktemp "$bins/.OpenKenshi-server-cpp-$tree12.XXXXXX.tmp") || { echo "cache: cannot write $bins"; exit 0; }
	if cp "$cachebin" "$tmp" && chmod 755 "$tmp" && mv -n "$tmp" "$dest"
	then
		if [ -e "$tmp" ]
		then
			rm -f "$tmp"	# mv -n leaves the source behind when another worker won the race
			echo "cache: skipped ($dest exists)"
		else
			echo "cached $dest"
		fi
	else
		rm -f "$tmp"
		echo "cache: copy to $dest failed"
	fi
	exit 0
fi

say() { [ "$explain" = 1 ] && printf 'shared_server: %s\n' "$*" >&2; return 0; }
shared() { [ "$explain" = 1 ] && printf 'shared %s\n' "$1" || printf '%s\n' "$1"; exit 0; }
built() { [ "$explain" = 1 ] && printf 'built (%s)\n' "$1"; exit 0; }

git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { say "$root is not a git worktree"; built "not a git worktree"; }

# Rule 1: the cpp/ tree key.
if [ -n "$tree12" ]
then
	tbin=$bins/OpenKenshi-server-cpp-$tree12
	if [ -x "$tbin" ]
	then
		touch -h "$tbin" 2>/dev/null
		say "rule 1: cpp/ tree $tree12 clean, $tbin exists"
		shared "$tbin"
	fi
	say "rule 1: cpp/ tree $tree12 clean, no $tbin"
else
	say "rule 1: cpp/ not clean (${dirty:-no git}), no tree key"
fi

# The newest master this clone has: local master and every <remote>/master, by commit time (ties keep the earlier name).
master= masterref= mastertime=
consider()
{
	local c t
	c=$(git -C "$root" rev-parse --verify --quiet "$1^{commit}" 2>/dev/null) || return 0
	t=$(git -C "$root" log -1 --format=%ct "$c" 2>/dev/null) || return 0
	if [ -z "$master" ] || [ "$t" -gt "$mastertime" ]
	then
		master=$c masterref=$1 mastertime=$t
	fi
}
consider refs/heads/master
while IFS= read -r r
do
	consider "$r"
done < <(git -C "$root" for-each-ref --format='%(refname)' 'refs/remotes/*/master')
[ -n "$master" ] || { say "no master ref (refs/heads/master, refs/remotes/*/master)"; built "no master ref"; }

mb=$(git -C "$root" merge-base HEAD "$master" 2>/dev/null) || { say "no merge base with $masterref (${master:0:8})"; built "no merge base with master"; }
say "master $masterref ${master:0:8}, merge base ${mb:0:8}"

say "rule 2: merge base"
# (a) no server source touched: every cpp/ path is the server's, nothing outside cpp/ is.
while IFS= read -r p
do
	[ -n "$p" ] || continue
	case $p in
		cpp/*) say "touches $p"; built "cpp/ changed" ;;
	esac
done < <(git -C "$root" diff --name-only "$mb" --; git -C "$root" ls-files --others --exclude-standard 2>/dev/null)

# Rule 3 (kenshi-k16.2.306.5.1), the wire number: N = kProtocol read from this branch's client/src/net/simlink.hpp. The newest (by
# mtime) executable $HOME/git/kenshi/bin/OpenKenshi-server-w<N>-* wins; a binary that answers `--wire-version` must answer N (one
# that does not understand the flag is taken on its name). Only reached when (a) held: a branch whose commits since the merge base
# touch cpp/ builds its own server, because additive wire changes are only safe newer-server / older-client, never the reverse.
wire=$(sed -n 's/^[[:space:]]*constexpr[[:space:]]\+uint16_t[[:space:]]\+kProtocol[[:space:]]*=[[:space:]]*\([0-9]\+\).*/\1/p' "$root/client/src/net/simlink.hpp" 2>/dev/null | head -n 1)
if [ -n "$wire" ]
then
	say "rule 3: wire number $wire"
	while IFS= read -r c
	do
		[ -x "$c" ] || continue
		got=$("$c" --wire-version 2>/dev/null </dev/null | head -n 1)
		if [ -n "$got" ] && [ "$got" != "$wire" ]
		then
			say "skip $c (--wire-version $got, want $wire)"
			continue
		fi
		touch -h "$c" 2>/dev/null
		if [ "$explain" = 1 ]
		then
			printf 'shared %s (w%s: cpp/ unchanged since the merge base, newest server with the same kProtocol)\n' "$c" "$wire"
			exit 0
		fi
		shared "$c"
	done < <(ls -t "$bins"/OpenKenshi-server-w"$wire"-* 2>/dev/null)
	say "no OpenKenshi-server-w$wire-* binary"
else
	say "rule 3: no kProtocol in client/src/net/simlink.hpp"
fi

# (b) the deploy for that commit
bin=$bins/OpenKenshi-server-${mb:0:8}
if [ -x "$bin" ]
then
	shared "$bin"
fi
say "no $bin"
built "no w${wire:-?} binary, no shared binary for ${mb:0:8}"
