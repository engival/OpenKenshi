#!/usr/bin/env bash
# The rolling 'nightly' pre-release (kenshi-379.40.1): one GitHub pre-release on engival/OpenKenshi, tag `nightly`, title "OpenKenshi nightly",
# holding one stable asset name per platform (OpenKenshi-windows-x64.zip, OpenKenshi-macos-arm64.zip; Linux joins with kenshi-379.39). There are
# no numbered releases (user 2026-11-20): friends download the same two links and always get the latest green main.
#
# Each workflow runs this once, only after all of its build and check steps were green, and only on a push to main. That is what leaves a failed
# platform's previous asset and note line in place: this script never touches the other platforms' lines. The notes are "windows: <short sha>
# <UTC date>" one per platform, and a run rewrites its own line only.
#
# usage: .github/nightly.sh <platform> <zip>
#   platform  the notes prefix and asset family: windows, macos (later linux)
#   zip       the asset to upload, from the workflow's build job
# Env: GITHUB_REPOSITORY (owner/repo) and GITHUB_SHA, which GitHub Actions sets; GH_TOKEN, the job's GITHUB_TOKEN (the job needs contents: write).
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 <platform> <zip>" >&2
	exit 2
fi
platform=$1
zip=$2
case $platform in
windows | macos | linux) ;;
*)
	echo "nightly: platform must be windows, macos or linux: $platform" >&2
	exit 2
	;;
esac
if [[ ! -f $zip ]]; then
	echo "nightly: no such zip: $zip" >&2
	exit 2
fi
: "${GITHUB_REPOSITORY:?nightly.sh needs GITHUB_REPOSITORY (owner/repo)}"
: "${GITHUB_SHA:?nightly.sh needs GITHUB_SHA}"

tag=nightly
title='OpenKenshi nightly'
asset=$(basename "$zip")
short=${GITHUB_SHA:0:8}
now=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
line="$platform: $short $now"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
body=$work/body.md
notes=$work/notes.md

# 1. The release itself: created at this commit the first time, reused every time after. `gh release view` exits non-zero when there is none.
if gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json body -q .body >"$body" 2>/dev/null; then
	echo "nightly: $tag exists, updating it"
else
	echo "nightly: no $tag yet, creating it at $GITHUB_SHA"
	: >"$body"
	gh release create "$tag" --repo "$GITHUB_REPOSITORY" --target "$GITHUB_SHA" --prerelease --title "$title" --notes ''
fi

# 2. The asset, under the name friends download from: --clobber replaces the previous build of that name in place, so the link never changes.
gh release upload "$tag" "$zip" --repo "$GITHUB_REPOSITORY" --clobber

# 3. The tag follows the commit this asset was built from (a lightweight tag, so the ref is simply repointed).
gh api -X PATCH "repos/$GITHUB_REPOSITORY/git/refs/tags/$tag" -f sha="$GITHUB_SHA" -F force=true >/dev/null

# 4. The notes, last, so as long as this step has not run the platform's line still names the asset that is actually there. Drop empty lines (GitHub adds them on read-back) and this platform's line
# (a literal prefix match: `index`, not a regex), keep every other platform's line verbatim, append the new one.
awk -v p="$platform" 'NF && index($0, p ": ") != 1' "$body" >"$notes"
printf '%s\n' "$line" >>"$notes"
gh release edit "$tag" --repo "$GITHUB_REPOSITORY" --prerelease --title "$title" --notes-file "$notes"

echo "nightly: $asset uploaded, notes line '$line'"
