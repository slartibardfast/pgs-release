#!/bin/bash
# resolve-version-conflicts.sh — resolve FFmpeg version bump conflicts during rebase
#
# When rebasing patches across branches (e.g. master → release/8.1), library
# version bumps in */version.h and doc/APIchanges conflict because the minor
# version diverges between branches.
#
# Usage (during a conflicted rebase or cherry-pick):
#   ../scripts/resolve-version-conflicts.sh
#   git rebase --continue
#
# Usage (as rebase --exec for automatic resolution):
#   git rebase upstream/release/8.1 \
#     --exec '../scripts/resolve-version-conflicts.sh --exec-mode'
#
# In --exec-mode, the script is a no-op if there are no conflicts (rebase
# --exec runs after each successful pick). It only acts when called manually
# or when conflicts exist.
#
# What it does:
#   - */version.h: resolves stacked conflict markers by taking the highest
#     MINOR version from either side, then incrementing by 1. Handles the
#     MICRO reset to 100 that accompanies MINOR bumps.
#   - doc/APIchanges: keeps BOTH sides (ours = existing entries on target
#     branch, theirs = new entry from the rebased patch). Adjusts the
#     version number in theirs to be MINOR+1 from the highest ours entry.
#   - Changelog: keeps both sides.
#   - Any other conflicted file: exits with error (manual resolution needed).
#
# Environment:
#   Run from the ffmpeg/ submodule directory (or wherever .git is).
#   Requires python3 for the conflict resolution logic.
#
# Limitations:
#   - Assumes standard FFmpeg version.h format
#   - Does not resolve non-version conflicts (e.g. Makefile test lists)

set -euo pipefail

EXEC_MODE=0
if [ "${1:-}" = "--exec-mode" ]; then
    EXEC_MODE=1
fi

conflicts=$(git diff --name-only --diff-filter=U 2>/dev/null || true)

if [ -z "$conflicts" ]; then
    [ $EXEC_MODE -eq 1 ] && exit 0
    echo "No conflicted files."
    exit 0
fi

echo "Resolving version conflicts in: $conflicts"

for f in $conflicts; do
    case "$f" in
        */version.h)
            # Handle stacked conflict markers from multiple rebased version bumps.
            # Strategy: find the highest MINOR version mentioned anywhere in the
            # conflict blocks, then set MINOR = max + 1, MICRO = 100.
            python3 << 'PYEOF'
import re, sys

with open("CONFLICTFILE") as fh:
    content = fh.read()

# Find all MINOR version numbers in conflict blocks
minors = [int(m) for m in re.findall(
    r'VERSION_MINOR\s+(\d+)', content)]

if not minors:
    print("ERROR: no VERSION_MINOR found in CONFLICTFILE", file=sys.stderr)
    sys.exit(1)

# Also find MINOR outside conflict blocks (the resolved value)
highest = max(minors)

# Find the macro name (LIBAVUTIL_VERSION_MINOR, LIBAVCODEC_VERSION_MINOR, etc.)
macro = re.search(r'(LIB\w+_VERSION_MINOR)', content)
if not macro:
    print("ERROR: no version macro found in CONFLICTFILE", file=sys.stderr)
    sys.exit(1)
macro_name = macro.group(1)

# Remove all conflict blocks, replace with the highest + 1 if our patch
# is bumping, or highest if it's just catching up to the target branch.
# Heuristic: if theirs (the patch) has a lower number, it was written
# against an older base. Increment from ours (the target).
# If theirs is higher, the patch already bumped — keep the bump relative
# to our base by using max(ours) + 1.

# Extract ours and theirs from the FIRST conflict block
m = re.search(
    r'<<<<<<< HEAD\n.*?' + macro_name + r'\s+(\d+).*?\n'
    r'=======\n.*?' + macro_name + r'\s+(\d+).*?\n'
    r'>>>>>>> [^\n]+',
    content, re.DOTALL)

if m:
    ours_minor = int(m.group(1))
    theirs_minor = int(m.group(2))
    # If theirs bumped (theirs > some base), increment from ours
    new_minor = ours_minor + 1
else:
    new_minor = highest

# Strip ALL conflict markers and version lines within them,
# replace with single clean definition
cleaned = re.sub(
    r'<<<<<<< [^\n]*\n'
    r'(?:.*?\n)*?'
    r'=======\n'
    r'(?:.*?\n)*?'
    r'>>>>>>> [^\n]*\n',
    '',
    content)

# Now set the MINOR version
cleaned = re.sub(
    r'#define\s+' + macro_name + r'\s+\d+',
    f'#define {macro_name}  {new_minor}',
    cleaned)

# Reset MICRO to 100 (standard for MINOR bumps)
micro_name = macro_name.replace('MINOR', 'MICRO')
cleaned = re.sub(
    r'#define\s+' + micro_name + r'\s+\d+',
    f'#define {micro_name} 100',
    cleaned)

with open("CONFLICTFILE", 'w') as fh:
    fh.write(cleaned)

print(f"  CONFLICTFILE: {macro_name} → {new_minor}")
PYEOF
            # Replace placeholder with actual filename
            sed_safe=$(echo "$f" | sed 's/[&/\]/\\&/g')
            python3 -c "
import re, sys

f = '$f'
with open(f) as fh:
    content = fh.read()

minors = [int(m) for m in re.findall(r'VERSION_MINOR\s+(\d+)', content)]
if not minors:
    print(f'ERROR: no VERSION_MINOR found in {f}', file=sys.stderr)
    sys.exit(1)

macro = re.search(r'(LIB\w+_VERSION_MINOR)', content)
if not macro:
    print(f'ERROR: no version macro found in {f}', file=sys.stderr)
    sys.exit(1)
macro_name = macro.group(1)
micro_name = macro_name.replace('MINOR', 'MICRO')

# Find ours (HEAD) minor from first conflict
m = re.search(
    r'<<<<<<< HEAD\n(?:.*?\n)*?#define\s+' + macro_name + r'\s+(\d+)',
    content, re.DOTALL)
ours_minor = int(m.group(1)) if m else max(minors)
new_minor = ours_minor + 1

# Remove all conflict blocks
cleaned = re.sub(
    r'<<<<<<< [^\n]*\n(?:.*?\n)*?=======\n(?:.*?\n)*?>>>>>>> [^\n]*\n',
    '', content)

cleaned = re.sub(
    r'#define\s+' + macro_name + r'\s+\d+',
    f'#define {macro_name}  {new_minor}', cleaned)
cleaned = re.sub(
    r'#define\s+' + micro_name + r'\s+\d+',
    f'#define {micro_name} 100', cleaned)

with open(f, 'w') as fh:
    fh.write(cleaned)
print(f'  {f}: {macro_name} {ours_minor} -> {new_minor}')
"
            if grep -q '<<<<<<' "$f"; then
                echo "ERROR: unresolved markers in $f"
                exit 1
            fi
            git add "$f"
            ;;

        doc/APIchanges)
            # Keep BOTH sides: ours (target branch entries) + theirs (new entry).
            # Adjust theirs version number to be consistent with the target branch.
            python3 -c "
import re

f = 'doc/APIchanges'
with open(f) as fh:
    content = fh.read()

# Find conflict block
m = re.search(
    r'<<<<<<< HEAD\n(.*?)\n=======\n(.*?)\n>>>>>>> [^\n]+',
    content, re.DOTALL)

if not m:
    print(f'  {f}: no conflict block found')
    exit(0)

ours = m.group(1).strip()
theirs = m.group(2).strip()

# Find the highest version in ours for each library
# Format: '2026-xx-xx - hash - libname major.minor.100 - header'
ours_versions = {}
for line in ours.split('\n'):
    vm = re.match(r'.*- (la\w+) (\d+)\.(\d+)\.\d+', line)
    if vm:
        lib, major, minor = vm.group(1), int(vm.group(2)), int(vm.group(3))
        if lib not in ours_versions or minor > ours_versions[lib][1]:
            ours_versions[lib] = (major, minor)

# Find what library theirs references
theirs_lib = None
theirs_line_match = None
for line in theirs.split('\n'):
    vm = re.match(r'(.*- )(la\w+) (\d+)\.(\d+)(\.\d+ .*)', line)
    if vm:
        theirs_lib = vm.group(2)
        theirs_major = int(vm.group(3))
        theirs_minor = int(vm.group(4))
        theirs_line_match = vm
        break

if theirs_lib and theirs_lib in ours_versions:
    # Increment from ours
    new_minor = ours_versions[theirs_lib][1] + 1
    theirs = re.sub(
        r'(- ' + theirs_lib + r' \d+\.)\d+(\.)',
        r'\g<1>' + str(new_minor) + r'\2', theirs, count=1)

# Combine: theirs (new) on top, then ours (existing)
combined = theirs + '\n\n' + ours
content = content[:m.start()] + combined + content[m.end():]

with open(f, 'w') as fh:
    fh.write(content)
print(f'  {f}: merged (theirs on top, version adjusted)')
"
            if grep -q '<<<<<<' "$f"; then
                echo "ERROR: unresolved markers in $f"
                exit 1
            fi
            git add "$f"
            ;;

        Changelog)
            # Keep both sides
            python3 -c "
import re
with open('Changelog') as fh:
    c = fh.read()
c = re.sub(r'<<<<<<< [^\n]*\n', '', c)
c = re.sub(r'=======\n', '', c)
c = re.sub(r'>>>>>>> [^\n]*\n', '', c)
with open('Changelog', 'w') as fh:
    fh.write(c)
print('  Changelog: merged (both sides)')
"
            if grep -q '<<<<<<' "Changelog"; then
                echo "ERROR: unresolved markers in Changelog"
                exit 1
            fi
            git add Changelog
            ;;

        *)
            echo "ERROR: unexpected conflict in $f — manual resolution needed"
            exit 1
            ;;
    esac
done

echo "All version conflicts resolved. Run: git rebase --continue"
