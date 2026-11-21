#!/usr/bin/env bash
# Make the macOS build self-contained (kenshi-379.40.1). The binary links Homebrew's libvorbis / libogg by absolute path (/opt/homebrew/opt/...);
# a friend's Mac has neither. So `bundle` copies every non-system dylib it loads into <dir>/lib/, points its install name and every reference to it
# at @executable_path/lib/<name>, and recursively does the same for the bundled dylibs' own dependencies. `check` is the CI gate, the macOS
# counterpart of windows.yml's Runtime DLLs step: every load command must be /usr/lib, /System, or @executable_path/ with the file present.
#
# Ad-hoc signatures: Apple Silicon refuses to load unsigned code, and install_name_tool invalidates the linker's ad-hoc signature. `codesign
# --sign -` re-signs ad-hoc, which is not a Developer ID signature: nothing is signed with a certificate or notarised (the zip's README says so).
#
# usage: .github/macos_bundle.sh bundle|check <dir>     (<dir>: OpenKenshi, simdata/, lib/ and README.txt as the zip unpacks them)
# Runs on the macOS runner; written for the bash 3.2 that macOS ships with (no mapfile, no associative arrays, no empty-array expansion).
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 bundle|check <dir>" >&2
	exit 2
fi
mode=$1
dir=$2
case $mode in
bundle | check) ;;
*)
	echo "macos_bundle: mode must be bundle or check: $mode" >&2
	exit 2
	;;
esac
if [[ ! -f $dir/OpenKenshi ]]; then
	echo "macos_bundle: no $dir/OpenKenshi" >&2
	exit 2
fi

# The load commands of a Mach-O file, one per line: the "<file>:" header line is dropped, everything after it is a dependency. A dylib's first
# load command is its own id (LC_ID_DYLIB), so check treats a Homebrew id as a failure too, and bundle's `-change` of it is a no-op the linker's
# loader ignores (`-id` is what rewrites it).
deps() {
	otool -L "$1" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility.*$//'
}

is_system() {
	case $1 in
	/usr/lib/* | /System/*) return 0 ;;
	esac
	return 1
}

if [[ $mode == bundle ]]; then
	mkdir -p "$dir/lib"
	# The files whose dependencies still need looking at, one per line; bundled dylibs are appended while the loop reads the file.
	todo=$(mktemp)
	trap 'rm -f "$todo"' EXIT
	printf '%s\n' "$dir/OpenKenshi" >"$todo"
	while IFS= read -r file; do
		[[ -n $file ]] || continue
		while IFS= read -r lib; do
			[[ -n $lib ]] || continue
			is_system "$lib" && continue
			case $lib in
			@executable_path/*)
				# Already in the bundle: the file must be there, nothing to rewrite.
				if [[ ! -f "$dir/${lib#@executable_path/}" ]]; then
					echo "macos_bundle: $file wants $lib, missing from $dir" >&2
					exit 1
				fi
				continue
				;;
			@rpath/* | @loader_path/*)
				echo "macos_bundle: $file loads $lib: teach this script where it lives (only absolute paths are handled)" >&2
				exit 1
				;;
			esac
			base=$(basename "$lib")
			dst=$dir/lib/$base
			if [[ ! -f $dst ]]; then
				cp -L "$lib" "$dst"	# -L: Homebrew's .dylib entries are symlinks into the Cellar
				chmod u+w "$dst" "$file"
				install_name_tool -id "@executable_path/lib/$base" "$dst"
				printf '%s\n' "$dst" >>"$todo"
				echo "macos_bundle: bundled $base (from $lib)"
			fi
			install_name_tool -change "$lib" "@executable_path/lib/$base" "$file"
		done < <(deps "$file")
	done <"$todo"

	# Re-sign everything ad-hoc, the binary included: the linker's ad-hoc signature no longer matches the patched load commands.
	codesign --force --sign - --timestamp=none "$dir/OpenKenshi"
	for file in "$dir"/lib/*.dylib; do
		[[ -e $file ]] || continue
		codesign --force --sign - --timestamp=none "$file"
	done
	echo "macos_bundle: $dir is self-contained"
	exit 0
fi

# check: the gate. A load command outside /usr/lib, /System and the bundle fails the step, as a Homebrew path on a friend's Mac would.
bad=0
check_file() {
	file=$1
	while IFS= read -r lib; do
		[[ -n $lib ]] || continue
		is_system "$lib" && continue
		case $lib in
		@executable_path/*)
			if [[ ! -f "$dir/${lib#@executable_path/}" ]]; then
				echo "macos_bundle: NOT SELF-CONTAINED: $file loads $lib, which is not in the zip" >&2
				bad=1
			fi
			;;
		*)
			echo "macos_bundle: NOT SELF-CONTAINED: $file loads $lib (not /usr/lib, /System or the bundle)" >&2
			bad=1
			;;
		esac
	done < <(deps "$file")
}
otool -L "$dir/OpenKenshi"
check_file "$dir/OpenKenshi"
for file in "$dir"/lib/*.dylib; do
	[[ -e $file ]] || continue
	check_file "$file"
done
if ((bad != 0)); then
	exit 1
fi
echo "runtime libraries: self-contained (otool -L: /usr/lib, /System and $dir/lib only)"
