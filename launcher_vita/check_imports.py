#!/usr/bin/env python3
# Checks offline that so_loader will be able to resolve every import of every
# module: each undefined symbol must be in the launcher's export table or be
# defined in the module's own DT_NEEDED tree, which is exactly the scope the
# loader searches. Catches missing exports before anything runs on a Vita.
#
# usage: check_imports.py <nm> <readelf> <exports.S> <module.so>...

import os
import re
import subprocess
import sys

def run(*args):
	return subprocess.run(args, check=True, capture_output=True, text=True).stdout

def main():
	nm, readelf, exports_path, paths = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]

	exports = set(re.findall(r'\.word \.Lname\d+, "([^"]+)"', open(exports_path).read()))

	modules = {}
	for path in paths:
		defined, imports = set(), {}
		for line in run(nm, '-D', path).splitlines():
			parts = line.split()
			if len(parts) == 2:
				imports[parts[1]] = parts[0]
			elif len(parts) == 3:
				defined.add(parts[2])
		needed = re.findall(r'\(NEEDED\).*\[(.+)\]', run(readelf, '-dW', path))
		modules[os.path.basename(path)] = (defined, imports, needed)

	problems = 0
	for name, (defined, imports, needed) in sorted(modules.items()):
		# breadth-first, like so_loader's BuildScope
		scope = [name]
		for mod in scope:
			for dep in modules[mod][2]:
				if dep not in modules:
					print('%s: dependency %s not found' % (name, dep))
					problems += 1
				elif dep not in scope:
					scope.append(dep)

		available = exports.union(*(modules[m][0] for m in scope))
		missing = sorted(s for s, kind in imports.items() if kind == 'U' and s not in available)
		if missing:
			problems += 1
			print('%s: %d unresolved imports: %s' % (name, len(missing), ' '.join(missing[:10])))

	print('%d modules checked, %d with problems' % (len(modules), problems))
	return 1 if problems else 0

if __name__ == '__main__':
	sys.exit(main())
