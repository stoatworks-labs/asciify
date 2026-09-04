#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Four things get checked, and they fail in different ways:
#
#   --font    the font's own invariants: that every glyph fits in the atlas,
#             that no code point is drawn twice, and that no two glyphs are
#             drawn identically. The last one is a precondition of --match
#             rather than tidiness -- two identical bitmaps and a rendered cell
#             could not be attributed to one character.
#   --match   the GLSL copy of the matching maths against the C++ copy, by
#             rendering at one output pixel per glyph pixel and reading the
#             characters back out. Run across the Structure range and every
#             character set, because a set with almost no tonal range (box
#             drawing) and one with a full range (blocks) exercise opposite
#             ends of the cost function.
#   --ramp    that the alphabet measures at all, and is printed for eyeballing.
#             Not a pass/fail; it is here so the log of a verify run records
#             what the ramp was on the day.
#   sweep.py  that no control is silently dead.
#
# --match reports ties separately from disagreements. A tie is two characters
# whose costs sit inside the half-float step the copy buffer introduces, and
# which of them wins is then down to the last bit of a float. That is not the
# same question as whether the two implementations agree, so it is not counted
# as a failure. See kTieTolerance in tools/asctest/main.cpp.
set -uo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/asctest ]]; then
	echo "build/asctest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

failures=()

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

echo "== shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures+=("shaders")
fi
echo

# The parameter plumbing first: it needs no GPU, it takes a moment, and it is
# the half an external user actually got stuck on (vertigo issue #2).
echo "== presets: every factory preset survives every host behaviour"
if ./build/asctest --presets | tail -1; then
	:
else
	failures+=("presets")
fi

echo "== font: the drawn glyphs"
if ./build/asctest --font; then
	:
else
	failures+=("font")
fi

echo
echo "== match: GLSL against C++, over structure x character set"
match_pass=0
match_fail=0
for structure in 0.0 0.25 0.5 0.75 1.0; do
	for set in 0 1 2 3 4 5 6 7; do
		result="$(./build/asctest --match --set "Structure=$structure" --set "Characters=$set" 2>&1 | grep '^match:')"
		status=$?
		if [[ $status -eq 0 && "$result" == *" 0 disagreed, 0 unreadable" ]]; then
			match_pass=$((match_pass + 1))
		else
			match_fail=$((match_fail + 1))
			failures+=("match structure=$structure set=$set -- $result")
		fi
	done
done
# The custom alphabet as well, which --set cannot reach.
for custom in " .:-=+*#%@" " |-/\\" " 01"; do
	result="$(./build/asctest --match --set "Characters=8" --custom "$custom" 2>&1 | grep '^match:')"
	if [[ "$result" == *" 0 disagreed, 0 unreadable" ]]; then
		match_pass=$((match_pass + 1))
	else
		match_fail=$((match_fail + 1))
		failures+=("match custom='$custom' -- $result")
	fi
done
echo "   $match_pass passed, $match_fail failed"

echo
echo "== ramp: the measured ordering, for the record"
./build/asctest --ramp | head -3

echo
echo "== sweep: no control silently dead"
if python3 tools/sweep.py > /tmp/asciify-sweep.txt 2>&1; then
	echo "   all parameters affect the output"
else
	echo "   *** dead controls, see /tmp/asciify-sweep.txt"
	tail -4 /tmp/asciify-sweep.txt
	failures+=("sweep")
fi

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
