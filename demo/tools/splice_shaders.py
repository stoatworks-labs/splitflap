"""Copy the plugin's shaders and its font table into demo/plugin.js, by script.

    python3 demo/tools/splice_shaders.py

The page's claim is that it runs the plugin's OWN shaders, so `demo/plugin.js`
carries `source/Shaders.cpp`'s four string constants as template literals, and
`check_shaders.py` fails `tools/verify.sh` if a character of any of them
differs. Hand-copying a 200-line raw string is exactly how a character
differs, so this does the copy: it replaces everything between the
`// @@shaders` markers in plugin.js with the current C++ text, tabs and all,
and everything between the `// @@font` markers with the glyph table from
`source/Font.cpp`.

The only edit on the way across is the one a template literal forces: a
backtick in a GLSL comment becomes \\` (and check_shaders.py unescapes it).
A `${` cannot be carried at all and is refused here, before it could become a
silent difference.

Run it after any change to Shaders.cpp or Font.cpp, then commit plugin.js with
the C++. It never touches anything outside the markers.
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

SHADERS_BEGIN = "// @@shaders-begin -- written by demo/tools/splice_shaders.py, do not edit\n"
SHADERS_END = "// @@shaders-end\n"
FONT_BEGIN = "// @@font-begin -- written by demo/tools/splice_shaders.py from source/Font.cpp, do not edit\n"
FONT_END = "// @@font-end\n"


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def glyphs(font_cpp):
    """The 96 rows of kGlyphs, each seven 5-character strings, in order."""
    table = re.search(r'kGlyphs\[ kCount \]\[ kHeight \] = \{(.*?)\n\};', font_cpp, re.S)
    if table is None:
        raise SystemExit("kGlyphs not found in source/Font.cpp")
    rows = re.findall(r'\{\s*((?:"[.#]{5}"\s*,?\s*){7})\}', table.group(1))
    out = []
    for row in rows:
        out.append(re.findall(r'"([.#]{5})"', row))
    if len(out) != 96:
        raise SystemExit(f"expected 96 glyphs in Font.cpp, found {len(out)}")
    return out


def replace_between(text, begin, end, body):
    start = text.find(begin)
    stop = text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise SystemExit(f"markers {begin.strip()!r} / {end.strip()!r} not found in demo/plugin.js")
    return text[: start + len(begin)] + body + text[stop:]


def main():
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "source", "Font.cpp")) as handle:
        font_cpp = handle.read()
    js_path = os.path.join(REPO, "demo", "plugin.js")
    with open(js_path) as handle:
        js = handle.read()

    shader_block = ""
    for name, symbol in SNIPPETS:
        body = from_cpp(cpp, symbol)
        if body is None:
            raise SystemExit(f"{symbol} not found in source/Shaders.cpp")
        if "${" in body:
            raise SystemExit(f"{symbol} contains ${{, which a template literal would interpolate")
        if "\\" in body:
            raise SystemExit(f"{symbol} contains a backslash, which a template literal would read as an escape")
        shader_block += f"\nconst {name} = `{body.replace('`', chr(92) + '`')}`;\n"
    shader_block += "\n"

    font_block = "\nconst GLYPHS = [\n"
    for row in glyphs(font_cpp):
        font_block += "  [" + ", ".join(f"'{r}'" for r in row) + "],\n"
    font_block += "];\n\n"

    js = replace_between(js, SHADERS_BEGIN, SHADERS_END, shader_block)
    js = replace_between(js, FONT_BEGIN, FONT_END, font_block)
    with open(js_path, "w") as handle:
        handle.write(js)
    print(f"spliced {len(SNIPPETS)} shaders and 96 glyphs into demo/plugin.js")
    return 0


if __name__ == "__main__":
    sys.exit(main())
