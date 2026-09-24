#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
#     tools/verify.sh
#
# ------------------------------------------------------------------ the point
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out
#   build         a fresh universal Release build, which is what ships
#   checks        the claims the plugin exists to make, read off the rendered
#                 picture at 640x360 and at 320x180: every cell makes exactly
#                 (t - c) mod N flips; a step brighter is one flip and a step
#                 darker N - 1; a still input settles and then holds
#                 bit-identical; the flap in the air follows the rigid-body
#                 fall to within a pixel and lands on the stated frame; a
#                 resize and a regrid mid-flip keep every flap; loud audio on
#                 frame one fires nothing
#   negative      every one of those against a model with one thing broken,
#                 and each has to fail
#   names         no parameter name over FFGL's 16 characters, none duplicated
#   pipe          the fleet's --pipe frame format writes whole frames, and a
#                 reader that hangs up gets exit 1, not SIGPIPE's silent 141
#   sweep         does every control change the picture, at two rasters
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before -DCMAKE_OSX_ARCHITECTURES arrived
#                 and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a *nested* object and mentions neither
#                 the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         instantiation and frames in a host, and the name, id and
#                 TYPE a host reads
#   bench         the render cost, for the record; not pass/fail
#
# ------------------------------------------------- why the build is deleted
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is where
# the architecture list lives. A developer who configured once with
# `-DCMAKE_OSX_ARCHITECTURES=arm64` for a fast iteration loop leaves a tree
# where this script happily rebuilds, finds a single-architecture binary and
# reports it as a defect in the source. So: fresh configure, every time.
#
# This is bash, not zsh, on purpose: the pipe step reads PIPESTATUS.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
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

FILES = [
	"source/Shaders.cpp",
]

# A shader may be several adjacent raw strings -- MSVC caps one literal at
# about 16 KB (C2026) -- so everything up to the terminating semicolon is
# joined. Lose this join and the check reports a syntax error in the middle
# of a function.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
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

	if [ "$n" -lt 4 ]; then
		# Fewer than four is a FAILURE, not a pass: a vertex shader and the
		# copy, motor and board fragments. The extraction has gone stale.
		printf '   only %d shader(s) extracted -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

step "build (fresh, universal)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/tmp/splitflap-configure.log 2>&1 \
   && cmake --build "$BUILD" --parallel >/tmp/splitflap-build.log 2>&1; then
	pass "configured and built"
else
	fail "build failed -- see /tmp/splitflap-build.log"
	tail -25 /tmp/splitflap-build.log
	exit 1
fi

SFTEST="$BUILD/sftest"

#---------------------------------------------------------------------------
# The checks, each at 640x360 and 320x180.
#---------------------------------------------------------------------------
step "checks (the rendered picture, two rasters)"
for t in flips asymmetry settle fall resize prime; do
	log="/tmp/splitflap-$t.log"
	if "$SFTEST" "--$t" >"$log" 2>&1; then
		pass "sftest --$t"
	else
		fail "sftest --$t -- see $log"
		grep -E "FAIL" "$log" | head -6
	fi
done

step "negative controls (each broken model must fail its check)"
if "$SFTEST" --negative >/tmp/splitflap-negative.log 2>&1; then
	pass "$( tail -1 /tmp/splitflap-negative.log )"
else
	fail "a broken model passed its check -- see /tmp/splitflap-negative.log"
	grep -E "MISS" /tmp/splitflap-negative.log | head -6
fi

step "names"
if "$SFTEST" --names >/tmp/splitflap-names.log 2>&1; then
	pass "no name over 16 characters, none duplicated"
else
	fail "sftest --names -- see /tmp/splitflap-names.log"
fi

#---------------------------------------------------------------------------
# The pipe. Whole frames, and the right exit status when the reader goes.
#
# `head -c 1` reads one byte and closes the pipe. Twenty 320x180 frames are
# 4.6 MB, far more than a pipe buffer holds, so the harness cannot finish
# before the reader has gone: its next write fails with EPIPE (SIGPIPE is
# ignored) and it must exit 1. PIPESTATUS is bash's; zsh has no such thing,
# which is why this file says bash on line one.
#---------------------------------------------------------------------------
step "pipe"
bytes=$("$SFTEST" --pipe --size 64x36 --frames 3 </dev/null 2>/dev/null | wc -c | tr -d ' ')
if [ "$bytes" = "27648" ]; then
	pass "--pipe writes three whole 64x36 RGBA frames"
else
	fail "--pipe wrote $bytes bytes, expected 27648"
fi
"$SFTEST" --pipe --size 320x180 --frames 20 </dev/null 2>/tmp/splitflap-pipe.log | head -c 1 >/dev/null
pipe_status=${PIPESTATUS[0]}
if [ "$pipe_status" = "1" ]; then
	pass "a reader that hangs up gets exit 1 ($(tr -d '\n' </tmp/splitflap-pipe.log))"
else
	fail "a reader that hung up got exit $pipe_status, expected 1"
fi

step "demo: the browser copy of the shaders"
# demo/plugin.js cannot include a C++ file, so it carries its own copy of every
# shader and of the font's glyph table; the page's claim to run the plugin's
# own shaders rests on the two staying identical, and only this check enforces
# it. demo/tools/splice_shaders.py writes the copy; never edit it by hand.
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "the demo's shaders have drifted from source/Shaders.cpp -- run demo/tools/splice_shaders.py"
		printf '%s\n' "$out" | grep -E '^FAIL' | sed 's/^/      /'
	fi
else
	printf '   skipped: no demo/\n'
fi

step "sweep"
for size in 640x360 320x180; do
	if python3 tools/sweep.py --binary "$SFTEST" --size $size --jobs 4 >/tmp/splitflap-sweep-$size.log 2>/dev/null; then
		pass "$size: $( tail -1 /tmp/splitflap-sweep-$size.log )"
	else
		fail "tools/sweep.py reports a dead control at $size -- see /tmp/splitflap-sweep-$size.log"
		tail -4 /tmp/splitflap-sweep-$size.log | sed 's/^/   /'
	fi
done

BUNDLE="$BUILD/Splitflap.bundle"
BIN="$BUNDLE/Contents/MacOS/Splitflap"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Splitflap.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "registers, instantiates and lights pixels" ;;
			*"id:"*) fail "registers but oxbow reports FAIL -- see: $OXBOW selftest $BUNDLE" ;;
			*) fail "oxbow did not recognise the bundle" ;;
		esac
		# And the identity a host reads, which nothing else here checks.
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$probe" in *"id:          SF01"*) pass "id is SF01" ;; *) fail "wrong FFGL id" ;; esac
		case "$probe" in *"name:        SW Splitflap"*) pass "name is SW Splitflap" ;; *) fail "wrong plugin name" ;; esac
		case "$probe" in *"type:        effect"*) pass "type is effect" ;; *) fail "wrong plugin type" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench"
"$SFTEST" --bench 2>/dev/null | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
