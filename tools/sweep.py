"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A setting the CPU reads but never acts on
is quieter still. A control can therefore be completely dead while everything
compiles, links, loads and renders. Nothing else in this repo catches that.

So: render each parameter at both ends of its range against a context that makes
it mean something, and report any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**A still picture settles, and then Update means nothing.** Continuous, Interval
and Manual all latch the same targets on frame 0 and never need to again. Those
controls are swept with `--drift`, which walks the test card sideways a pixel
or two a frame, so the board has something to chase.

**An event is a press on a frame, not a value.** `--set "Update Now=1"` before
frame 0 is a press on frame 0, where the board latches anyway. The event is
swept with a cue script that presses it on frame 60, in Manual mode, over a
drifting card.

**Bounce only shows just after a landing.** With a one-second flip and no
stagger, the cells whose target is the second flap land it on frame 119; five
frames later the flutter has lifted the flap about 30 degrees, and on an 8 x 4
board that is five pixels. On the default 32 x 18 board one frame after landing
it is a twentieth of a pixel, and the sweep reported Bounce dead.

**Light Angle is even about straight on.** The plate at rest is lit by
cos( angle ), so -60 and +60 degrees -- the two ends of the slider -- draw the
same picture. It is swept against straight on.

**Drum Order only changes the path.** Every order settles on the same nearest
tone, so it is swept mid-run, where the intermediate flaps differ.

**Options are swept by index**, which `sftest --list` prints as the real range;
the SDK's own range for an option reads back 0..1 whatever its element count.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "sftest")
SCRATCH = tempfile.mkdtemp(prefix="sfsweep")

WIDTH, HEIGHT = 640, 360
FRAMES = 120          # two seconds at 60 fps

# `_frames`, `_drift`, `_low`, `_high` and `_press` are harness or sweep
# settings, not parameters; anything starting with an underscore is stripped
# before --set.
MOVING = {"_drift": 2, "_frames": 120}

CONTEXT = {
    "Fit": {"Cell Aspect": 0.0},                       # a cell far from the frame's own aspect
    "Cell Aspect": {"Fit": 0},
    "Drum": {"_high": 2},
    # The order only changes the PATH round the drum; settled, every order
    # shows the same nearest tone. Swept mid-run.
    "Drum Order": {"_high": 2, "_frames": 40},
    "Palette": {"Drum": 1, "_high": 5},
    "Text": {"Drum": 2, "_low": "A", "_high": "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"},
    "Flip Time": {"_frames": 90},
    "Stagger": {"_frames": 30},
    "Module Width": {"Stagger": 1.0, "_frames": 30},
    "Update": dict(MOVING, _high=3),
    "Interval": dict(MOVING, Update=1),
    "Update Now": dict(MOVING, Update=3, _press=60),
    # Big cells, five frames after the second flap lands: the flutter has
    # lifted the flap about 30 degrees, a tenth of the half-cell.
    "Bounce": {"Flip Time": 1.0, "Stagger": 0, "Columns": 8, "Rows": 4, "_frames": 125},
    # -60 and +60 degrees light the plate at rest identically (cos is even);
    # straight on against 60 degrees is the pair that differs.
    "Light Angle": {"_high": 0.5},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}",
            "--frames", str(frames), "--fps", "60"]
    if "_drift" in overrides:
        args += ["--drift", str(overrides["_drift"])]
    if "_script" in overrides:
        args += ["--script", overrides["_script"]]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, kind, low, high, context = job

    lo = dict(context)
    hi = dict(context)
    if kind == "event":
        # A press on a frame, by cue script; the low side never presses.
        cue = pathlib.Path(SCRATCH) / f"{pid}_press.txt"
        cue.write_text(f"{context.get('_press', 60)}  {name}  1\n")
        hi["_script"] = str(cue)
    else:
        lo[name] = context.get("_low", low)
        hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    BIN = args.binary
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind == "buffer":
            skipped.append((name, "the host's spectrum; --prime exercises it"))
            continue
        work.append((pid, name, kind, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept at {WIDTH}x{HEIGHT}, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
