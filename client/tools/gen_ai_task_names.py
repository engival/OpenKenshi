#!/usr/bin/env python3
"""Generate the client's AI_TASK name table (src/aitask.hpp's aiTaskName) from docs/formats/AI_TASK_ENUMS.md.

The doc is the generated source of truth (tools/ai_task_names.py writes it from kinspect's enum map), so the names are never
hand-typed here: this parses its table at build time and emits one row per value, the value being the array index. Run by
client/CMakeLists.txt whenever the doc or this script changes:

    client/tools/gen_ai_task_names.py --doc <repo>/docs/formats/AI_TASK_ENUMS.md --out <build>/gen/ai_task_names.cpp
"""
import argparse
import re
import sys

ROW = re.compile(r'^\|\s*(\d+)\s*\|\s*`([^`]+)`\s*\|')


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument('--doc', required=True, help='docs/formats/AI_TASK_ENUMS.md')
	parser.add_argument('--out', required=True, help='the .cpp to write')
	args = parser.parse_args()

	names = {}
	try:
		with open(args.doc, encoding='utf-8') as handle:
			for line in handle.read().splitlines():
				match = ROW.match(line)
				if not match:
					continue
				value = int(match.group(1))
				if value in names:
					print('gen_ai_task_names: %s: value %d twice' % (args.doc, value), file=sys.stderr)
					return 1
				names[value] = match.group(2)
	except OSError as error:
		print('gen_ai_task_names: %s' % error, file=sys.stderr)
		return 1
	if not names:
		print('gen_ai_task_names: %s: no table rows' % args.doc, file=sys.stderr)
		return 1
	top = max(names)
	missing = [v for v in range(top + 1) if v not in names]
	if missing:	# a gap would shift every later name: refuse rather than guess
		print('gen_ai_task_names: %s: values missing: %s' % (args.doc, ', '.join(map(str, missing[:10]))), file=sys.stderr)
		return 1

	def literal(name: str) -> str:
		return '"' + name.replace('\\', '\\\\').replace('"', '\\"') + '"'

	rows = '\n'.join('\t\t%s,	// %d' % (literal(names[v]), v) for v in range(top + 1))
	text = (
		'// Generated from docs/formats/AI_TASK_ENUMS.md by client/tools/gen_ai_task_names.py: do not edit.\n'
		'#include "aitask.hpp"\n'
		'\n'
		'namespace\n'
		'{\n'
		'\tconst char* const kAiTaskNames[] =\n'
		'\t{\n'
		+ rows + '\n'
		'\t};\n'
		'}\n'
		'\n'
		'const char* aiTaskName(uint16_t value)\n'
		'{\n'
		'\treturn value < sizeof kAiTaskNames / sizeof *kAiTaskNames ? kAiTaskNames[value] : nullptr;\n'
		'}\n')
	try:
		with open(args.out, 'w', encoding='utf-8') as handle:
			handle.write(text)
	except OSError as error:
		print('gen_ai_task_names: %s' % error, file=sys.stderr)
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main())
