# kenshi-k16.2.301: resolve character ids by name at run time (a scenario's spawn ids move when it gains or loses a char; hard-coded ids went stale).
# kenshi-k16.2.308: a name may hold spaces (Slaver boss), and a name the scenario spawns more than once is picked by occurrence.
# Source it, then:
#   probe_char_ids BUILD TREE PORT OUTFILE  (a server must already listen on PORT) writes "ID<TAB>NAME" lines (the client's `dump chars` of the
#                                           fresh scenario) to OUTFILE, sorted by id; non-zero when the log held no characters.
#   char_id IDS NAME [N]                    prints the id of the Nth character named NAME in id (= spawn) order (default: the first); non-zero
#                                           plus a message when the scenario has no such character (or fewer than N of them).
#   charids_fill SCRIPT IDS OUTDIR OUTFILE  writes SCRIPT to OUTFILE with every OUTDIR by that output directory (absolute) and every placeholder
#                                           resolved: @NAME@ is the first character named NAME, @NAME#N@ the Nth (id order); non-zero (a message per name) when
#                                           one of them is absent. Names hold anything but @ and #. Comment lines (leading #) are left alone, so a
#                                           header may name the placeholders in prose (k16.2.308: lock.txt's header cost a run).
probe_char_ids() {
	local build=$1 tree=$2 port=$3 ids=$4 d
	d=$(dirname "$ids")
	# (a `dump things` follows for suites that need a container's id, e.g. sellburst's own box, k16.2.313)
	# three dumps: the first world frames can run before the sim's first snapshot lands (a stalled frame 10 showed no characters, k16.2.308)
	printf '30 dump chars\n60 dump chars\n90 dump chars\n100 dump things\n120 quit\n' > "$d/idprobe.txt"
	(cd "$tree" && timeout 200 nice -n 10 client/tools/headless.sh -- stdbuf -oL "$build/kclient" --simlink 127.0.0.1:$port --debug --script "$d/idprobe.txt" > "$d/idprobe.log" 2>&1)
	sed -n 's/^char \([0-9][0-9]*\) "\([^"]*\)".*/\1\t\2/p' "$d/idprobe.log" | sort -n -u > "$ids"	# id order: #N is spawn order; the dumps repeat, -u keeps one line per character
	[ -s "$ids" ]
}
char_id() {
	local id
	id=$(awk -F'\t' -v n="$2" -v want="${3:-1}" '$2 == n { if (++c == want) { print $1; exit } }' "$1")
	[ -n "$id" ] || { echo "charids: no character named '$2'${3:+ (#$3)} in $1" >&2; return 1; }
	echo "$id"
}
charids_fill() {
	local script=$1 ids=$2 outdir=$3 out=$4 ph name occ nid
	local -a sedargs=(-e "s|OUTDIR|$outdir|g")
	while IFS= read -r ph
	do
		name=${ph%%#*} occ=1
		case $ph in *'#'*) occ=${ph##*#} ;; esac
		nid=$(char_id "$ids" "$name" "$occ") || return 1
		sedargs+=(-e "s|@$ph@|$nid|g")
	done < <(grep -o '@[^@]*@' < <(grep -v '^[[:space:]]*#' "$script") | tr -d '@' | sort -u)
	sed "${sedargs[@]}" "$script" > "$out"
}
