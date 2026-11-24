#!/bin/bash
# Prune the knav_bake result cache (kenshi-k16.15.42; docs/reference/NAVMESH_CACHE.md "Bake cache").
# Usage: tools/knav_cache_prune.sh CACHE_DIR [MAX_AGE_DAYS=7] [MAX_GB=40]
# Deletes entries unused for MAX_AGE_DAYS (last use = mtime of .complete/.failed, touched on every hit), then the least recently
# used until the total is under MAX_GB. An entry is deleted only while holding its .lock (non-blocking: a held lock = a bake
# is running, skip it); a waiter that held the old lock file re-checks and starts over.
root=${1:?usage: knav_cache_prune.sh CACHE_DIR [MAX_AGE_DAYS] [MAX_GB]}
days=${2:-7}
maxgb=${3:-40}
[ -d "$root" ] || exit 0

drop()	# entry dir: remove it if nobody holds its lock
{
	local d=$1
	[ -e "$d/.lock" ] || { rm -rf -- "$d"; return; }
	(
		flock -n 9 || exit 1
		rm -rf -- "$d/data" "$d/.complete" "$d/.failed" "$d/.lock"
		rmdir -- "$d" 2>/dev/null
	) 9<"$d/.lock"
}

# last use of an entry: its marker's mtime; a dir without one (killed or running) uses the dir's own mtime
used()
{
	local f
	for f in "$1/.complete" "$1/.failed"; do [ -e "$f" ] && { stat -c %Y "$f"; return; }; done
	stat -c %Y "$1"
}

now=$(date +%s)
cut=$((now - days * 86400))
list=$(mktemp)
for d in "$root"/*/; do
	d=${d%/}
	[ -d "$d" ] || continue
	t=$(used "$d")
	# a dir with no marker is a bake in progress or a killed one: only the lock decides, and not within the hour
	if [ ! -e "$d/.complete" ] && [ ! -e "$d/.failed" ] && [ $((now - t)) -lt 3600 ]; then continue; fi
	if [ "$t" -lt "$cut" ]; then drop "$d"; else printf '%s %s\n' "$t" "$d" >>"$list"; fi
done

# size cap: oldest first
sort -n "$list" | while read -r t d; do
	[ -d "$d" ] || continue
	kb=$(du -sk -- "$root" | cut -f1)
	[ "$kb" -le $((maxgb * 1048576)) ] && break
	drop "$d"
done
rm -f -- "$list"
