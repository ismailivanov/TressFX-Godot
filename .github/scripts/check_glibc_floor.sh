#!/bin/sh
# Fails if an ELF file needs a glibc symbol version newer than MAX (default 2.28,
# the floor of the official Godot 4.x Linux binaries), or links libstdc++/libgcc_s
# dynamically (both must be static, godot-cpp's use_static_cpp=yes default).
#   usage: check_glibc_floor.sh [--max 2.28] FILE...
# Needs objdump (binutils) and a POSIX awk. Exit 0 = all files OK, 1 = violation, 2 = usage.
set -eu

max=2.28
if [ "${1:-}" = "--max" ]; then max=$2; shift 2; fi
[ $# -gt 0 ] || { echo "usage: $0 [--max X.Y] FILE..." >&2; exit 2; }

command -v objdump >/dev/null 2>&1 || { echo "objdump not found (install binutils)" >&2; exit 2; }

status=0
for f in "$@"; do
	[ -f "$f" ] || { echo "MISSING $f" >&2; status=1; continue; }
	# objdump -p prints the dynamic section (NEEDED) and the version-need table
	# ("required from libc.so.6:" followed by one GLIBC_x.y[.z] line per version).
	if ! headers=$(objdump -p "$f" 2>&1); then
		echo "FAIL $f: objdump cannot read it: $headers"
		status=1
		continue
	fi
	printf '%s\n' "$headers" | awk -v max="$max" -v file="$f" '
		function vcmp(a, b,   i, n, m, x, y) {      # -1/0/1, numeric per component
			n = split(a, x, "."); m = split(b, y, ".")
			for (i = 1; i <= (n > m ? n : m); i++) {
				if ((x[i] + 0) < (y[i] + 0)) return -1
				if ((x[i] + 0) > (y[i] + 0)) return 1
			}
			return 0
		}
		$1 == "NEEDED" && ($2 ~ /^libstdc\+\+\.so/ || $2 ~ /^libgcc_s\.so/) {
			printf "FAIL %s: dynamically links %s (build with use_static_cpp=yes)\n", file, $2; bad = 1
		}
		/^  required from / { lib = $3; sub(/:$/, "", lib) }
		$NF ~ /^GLIBC_[0-9]/ {
			v = $NF; sub(/^GLIBC_/, "", v)
			if (vcmp(v, top) > 0) top = v
			if (vcmp(v, max) > 0) { printf "FAIL %s: needs GLIBC_%s from %s (max allowed GLIBC_%s)\n", file, v, lib, max; bad = 1 }
		}
		END {
			# A glibc-linked shared library always needs some GLIBC_ version; none means objdump
			# did not see what this script expects.
			if (top == "") { printf "FAIL %s: no GLIBC_ version references found\n", file; bad = 1; top = "none" }
			printf "%s %s: highest GLIBC_%s\n", (bad ? "BAD " : "OK  "), file, top
			exit bad
		}' || status=1
done
exit $status
