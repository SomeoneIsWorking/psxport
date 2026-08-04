#!/bin/sh
# syntaxcheck.sh <source-file>...  — compile ONE translation unit with the project's real flags.
#
# Several agents share one build/ directory, so `cmake --build` from more than one of them at a time
# corrupts it. This gives an agent the compile check it needs without touching the build tree: it
# pulls the exact command CMake recorded for the file out of compile_commands.json and re-runs it
# with -fsyntax-only, writing no object file.
#
# Exits non-zero if the file is not in the database — a file the build does not compile is not
# "checked", and a silent 0 there would be the instrument lying.
# Repo-relative, deliberately: this file is tracked, and a hardcoded /home/<user> path in a shared
# framework is broken for everybody but its author.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for f in "$@"; do
  python3 - "$ROOT" "$f" <<'PY'
import json, os, shlex, subprocess, sys
root, want = sys.argv[1], os.path.realpath(sys.argv[2])
db = json.load(open(os.path.join(root, 'build', 'compile_commands.json')))
hit = [e for e in db if os.path.realpath(os.path.join(e['directory'], e['file'])) == want]
if not hit:
    sys.stderr.write('syntaxcheck: %s is not in compile_commands.json — NOT CHECKED\n' % want)
    sys.exit(2)
e = hit[-1]
argv = shlex.split(e['command']) if 'command' in e else list(e['arguments'])
out = [a for a in argv if a not in ('-c',) and not a.startswith('-o')]
# drop the "-o path" pair
cleaned, skip = [], False
for a in argv:
    if skip:
        skip = False; continue
    if a == '-o':
        skip = True; continue
    if a == '-c':
        continue
    cleaned.append(a)
cleaned.insert(1, '-fsyntax-only')
# -w is in this project's flags; force warnings back on for the format checks that matter here.
cleaned = [a for a in cleaned if a != '-w']
cleaned.insert(1, '-Wformat')
rc = subprocess.call(cleaned, cwd=e['directory'])
print('syntaxcheck %s: %s' % (os.path.basename(want), 'OK' if rc == 0 else 'FAILED'))
sys.exit(rc)
PY
done
