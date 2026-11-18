#!/usr/bin/env python3
"""Token-level clone detector for the kenshi C++ trees.

Finds duplicated blocks (copy-paste and renamed-variable copies) across .cpp/.hpp
files. Tokenizes C++ (comments stripped), replaces local identifiers with a
placeholder, keeps keywords, member names after `.`/`->`/`::`, and string literals
by value; then indexes k-gram token windows and extends shared k-grams into
maximal clone regions.

Usage:
    python3 -I tools/clone_detector.py cpp/src cpp/include -m 60
    python3 -I tools/clone_detector.py cpp/tests -m 60 --exact
    python3 -I tools/clone_detector.py cpp/src cpp/include -m 100 --max 400 > clones.txt

Why the defaults: -m 60 tokens is roughly 15-25 source lines; smaller matches are
mostly the generic scaffolding (if/return, field lists). --exact compares identifier
names too (true copy-paste); the default normalizes them (renamed copies). k-grams
occurring at more than 40 sites are skipped: those are generic patterns, and the
pair enumeration would be quadratic.

Output: one line per clone region, `TOKENS  FILE:LINE == FILE:LINE`; the most
clone-heavy files are summarized on stderr. See
docs/history/REVIEW_CODE_ROT_2026-11-09.md for the review that used it.
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq""".split())

TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lcom>//[^\n]*)
  | (?P<bcom>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\])*")
  | (?P<chr>'(?:\\.|[^'\\])*')
  | (?P<num>\.?\d[\w.']*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>\#\#|->\*|<<=|>>=|<=>|==|!=|<=|>=|&&|\|\||\+\+|--|->|::|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|[{}()\[\];,.:?~!%^&*+\-/=<>|])
""", re.S | re.X)


def tokenize(path, exact=False):
	"""The file's token stream; None when it cannot be read."""
	try:
		text = open(path, encoding='utf-8', errors='replace').read()
	except OSError:
		return []
	toks = []
	for m in TOKEN_RE.finditer(text):
		kind = m.lastgroup
		val = m.group()
		if kind in ('ws', 'lcom', 'bcom'):
			continue
		if kind == 'id':
			if val in KEYWORDS:
				toks.append(val)
			elif toks and toks[-1] in ('.', '->', '::'):
				toks.append('M_' + val)  # member/qualified names stay: blocks differ
			elif exact:
				toks.append(val)
			else:
				toks.append('ID')  # a renamed copy still matches
		elif kind == 'num':
			toks.append('NUM')
		elif kind == 'str':
			toks.append('S' + val)  # by value: a duplicated name table is a clone
		elif kind == 'chr':
			toks.append('CHR')
		else:
			toks.append(val)
	return toks


def find_clones(files, exact, k=35, min_tok=80, max_sites=40):
	streams = {}
	for f in files:
		toks = tokenize(f, exact)
		if len(toks) >= min_tok:
			streams[f] = toks
	print(f"# {len(streams)} files, {sum(map(len, streams.values()))} tokens", file=sys.stderr)

	# Stable token ids make the tuple hashes deterministic across runs.
	ids = {}
	streams = {f: [ids.setdefault(t, len(ids)) for t in toks] for f, toks in streams.items()}
	index = defaultdict(list)
	for f, toks in streams.items():
		for i in range(len(toks) - k + 1):
			index[hash(tuple(toks[i:i + k]))].append((f, i))

	seen = set()
	clones = []
	skipped = 0
	for occ in index.values():
		if len(occ) < 2:
			continue
		if len(occ) > max_sites:
			skipped += 1
			continue
		for a in range(len(occ)):
			fa, ia = occ[a]
			for b in range(a + 1, len(occ)):
				fb, ib = occ[b]
				if fa == fb and abs(ia - ib) < min_tok:
					continue
				if (fa, ia, fb, ib) in seen:
					continue
				ta, tb = streams[fa], streams[fb]
				back = 0
				while ia - back - 1 >= 0 and ib - back - 1 >= 0 and ta[ia - back - 1] == tb[ib - back - 1]:
					back += 1
				fwd = k
				while ia + fwd < len(ta) and ib + fwd < len(tb) and ta[ia + fwd] == tb[ib + fwd]:
					fwd += 1
				start_a, start_b, length = ia - back, ib - back, fwd + back
				if any((fa, start_a + i, fb, start_b + i) in seen for i in range(0, length, 5)):
					continue
				for i in range(length):
					seen.add((fa, start_a + i, fb, start_b + i))
				if length >= min_tok and (fa != fb or abs(start_a - start_b) >= min_tok):
					clones.append((length, fa, start_a, fb, start_b))
	clones.sort(reverse=True)
	if skipped:
		print(f"# {skipped} k-grams skipped (more than {max_sites} sites)", file=sys.stderr)
	return clones


def line_table(path):
	"""Token index -> 1-based line number."""
	text = open(path, encoding='utf-8', errors='replace').read()
	lines, line = [], 1
	for m in TOKEN_RE.finditer(text):
		kind = m.lastgroup
		val = m.group()
		if kind in ('ws', 'lcom', 'bcom'):
			line += val.count('\n')
			continue
		lines.append(line)
		line += val.count('\n')
	return lines


def main():
	ap = argparse.ArgumentParser(description='Token-level C++ clone detector.')
	ap.add_argument('roots', nargs='+', help='directories (or files) to scan')
	ap.add_argument('-k', type=int, default=35, help='k-gram length in tokens')
	ap.add_argument('-m', '--min', type=int, default=100, help='min clone length in tokens')
	ap.add_argument('--max', type=int, default=200, help='max clone regions printed')
	ap.add_argument('--exact', action='store_true', help='compare identifier names too')
	args = ap.parse_args()

	files = []
	for root in args.roots:
		if os.path.isfile(root):
			files.append(root)
			continue
		for dp, dn, fn in os.walk(root):
			if 'third_party' in dp:
				continue
			for f in fn:
				if f.endswith(('.cpp', '.cc', '.cxx', '.h', '.hpp')):
					files.append(os.path.join(dp, f))

	clones = find_clones(files, args.exact, k=args.k, min_tok=args.min)
	cache = {}

	def loc(f, idx):
		if f not in cache:
			cache[f] = line_table(f)
		return cache[f][idx] if idx < len(cache[f]) else -1

	for ln, fa, sa, fb, sb in clones[:args.max]:
		print(f"{ln:5d} tok  {fa}:{loc(fa, sa)}  ==  {fb}:{loc(fb, sb)}")
	print(f"# {len(clones)} clone regions >= {args.min} tokens", file=sys.stderr)

	mass = Counter()
	for ln, fa, sa, fb, sb in clones:
		mass[fa] += ln
		mass[fb] += ln
	print("--- most clone mass (tokens, file)", file=sys.stderr)
	for f, n in mass.most_common(25):
		print(f"{n:7d}  {f}", file=sys.stderr)


if __name__ == '__main__':
	main()
