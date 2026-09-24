"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.
Atrac's check (galvo's shape), for a plugin whose shaders are four whole
programs rather than snippets to be joined.

------------------------------------------------------------------- why

`source/Shaders.cpp` holds four GLSL string constants -- the vertex shader,
the copy pass, the motor and the board -- and `demo/plugin.js` holds the same
four as template literals. That is two copies of the same text, and two copies
drift -- quietly, because a demo that renders a *plausible* board looks exactly
like a demo that renders the right one. The whole claim of these pages is that
they run the plugin's own shader rather than something reimplemented to look
similar, so the claim needs something enforcing it.

Nothing else can. `sftest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists; `tools/mutate.sh` mutates the C++
copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment that has been updated on one side and not the other
is exactly the drift worth catching, because comments in this repo carry the
reasoning that justifies the code.

The one transformation is a decode, not a normalisation. A backtick cannot
appear raw inside a JavaScript template literal, so `plugin.js` escapes the two
the board shader's comment carries (`What flap \\`flap\\` shows at plate
position \\`pl\\``) as \\`. This unescapes that and *rejects any other backslash
on the JS side*; there are none anywhere in the C++, so a second escape could
only be somebody hiding a difference. A `${` would be interpolated by the
literal, so it is refused on the C++ side before it can become a silent
difference.

It also checks the Text drum's 5x7 font: `demo/plugin.js` carries
`source/Font.cpp`'s glyph table as 96 rows of seven strings, and those have to
be the same pictures, or a letter on the browser's board is not the letter on
the plugin's.

`demo/tools/splice_shaders.py` writes both blocks from the C++; run it rather
than editing plugin.js by hand.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. The fall table (Flap.cpp), the drum
print (Drum.cpp), the onset detector (Onset.cpp), every conversion in
Controls.cpp, the update decision and the frame sequence of
SplitflapPlugin::ProcessOpenGL are a hand translation in plugin.js, and only a
reader can tell whether they still agree. When you change one of those, change
it here too -- and remember that a wrong mapping shows up on the page as a board
that is subtly wrong, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ symbol. Every string constant in Shaders.cpp, in its order.
SNIPPETS = [
    ("VERTEX", "kVertexShader"),
    ("COPY", "kCopyShader"),
    ("MOTOR", "kMotorShader"),
    ("BOARD", "kBoardShader"),
]


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    return body.replace("\\`", "`"), None


def cpp_glyphs(source):
    table = re.search(r'kGlyphs\[ kCount \]\[ kHeight \] = \{(.*?)\n\};', source, re.S)
    if table is None:
        return None
    rows = re.findall(r'\{\s*((?:"[.#]{5}"\s*,?\s*){7})\}', table.group(1))
    return [re.findall(r'"([.#]{5})"', row) for row in rows]


def js_glyphs(source):
    table = re.search(r'^const GLYPHS = \[\n(.*?)\n\];$', source, re.S | re.M)
    if table is None:
        return None
    rows = re.findall(r"\[((?:'[.#]{5}'(?:, )?){7})\]", table.group(1))
    return [re.findall(r"'([.#]{5})'", row) for row in rows]


def main():
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "source", "Font.cpp")) as handle:
        font = handle.read()
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, symbol in SNIPPETS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
            continue
        if "${" in cpp_text:
            print(f"FAIL  {symbol} contains ${{, which a template literal would interpolate")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<8} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    # The constants in Shaders.cpp that this table does not know are drift too.
    known = {symbol for _, symbol in SNIPPETS}
    for symbol in re.findall(r'const char\* const (k\w+) = R"\(', cpp):
        if symbol not in known:
            print(f"FAIL  {symbol} is in source/Shaders.cpp but not in this check's table")
            problems += 1

    # The font.
    want = cpp_glyphs(font)
    got = js_glyphs(js)
    if want is None or len(want) != 96:
        print("FAIL  kGlyphs not found in source/Font.cpp, or not 96 rows")
        problems += 1
    elif got is None:
        print("FAIL  GLYPHS not found in demo/plugin.js")
        problems += 1
    elif want != got:
        problems += 1
        print("FAIL  GLYPHS in demo/plugin.js has drifted from kGlyphs in source/Font.cpp")
        for i, (a, b) in enumerate(zip(want, got)):
            if a != b:
                print(f"        first difference at glyph {i} (code {32 + i})")
                break
        if len(want) != len(got):
            print(f"        {len(want)} glyphs in the C++, {len(got)} in the JS")
    else:
        print(f"ok    GLYPHS   matches kGlyphs ({len(want)} glyphs)")

    print()
    if problems:
        print(f"{problems} problem(s) -- run demo/tools/splice_shaders.py, do not edit plugin.js by hand")
        return 1
    print(f"all {len(SNIPPETS)} shaders and the font are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
