# splitflap — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **effect** for Resolume Arena/Avenue that shows
the clip on a split-flap departures board: every cell a drum of flaps that can
only turn forward, one flap at a time, at the motor's rate, and every flap in
the air a hinged plate falling under gravity. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/splitflap`; built 2026-09-24 as a local v0.1.0 and
**never loaded into Resolume**.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, every
number in the harness and where its tolerance comes from, whether each would
hold on another rasteriser, the negative controls, the mutation test, the traps
this build actually hit, the decisions taken without asking, what is verified
and what is assumed.

---

## The one idea

**A drum can only turn forward.** A split-flap unit holds N flaps, each printed
with one symbol, and to show a new one it passes through every flap between
the one on show and the target. Point a board of such cells at the clip and
the look of a departures board changing falls out of that one constraint:

- **A change ripples**, because each cell needs (target − current) mod N flips
  and they arrive at different times.
- **Asymmetry**, because with the drum printed dark to light a step brighter is
  one flip and a step darker is N − 1.
- **A moving picture is never finished**, because the board chases a clip that
  changes faster than the cells can flip, and a cell mid-flip shows the drum's
  intermediate flaps.
- **The flap is a hinged plate**: θ″ = (3g/2L) sin θ from three degrees past
  vertical, an inverted pendulum let go, which hesitates and then slaps on to
  the stop, where restitution e lets it flutter. Its Lambert shading follows
  its angle. `Flip Time` is the only knob: it scales g so the fall takes one
  flip time.

Nothing is animated. The motor is a state machine per cell in a float texel;
the board is drawn from that state and a table of the fall.

---

## Shape of the code

    source/Controls.*      parameter ids, 0..1 to engineering units, OptionIndex.
    source/Flap.*          the rigid plate solved in double: RK4, impacts
                           interpolated inside the step, sampled to a table in
                           flip units. angleAt() is the shader's arithmetic.
    source/Drum.*          what the flaps are printed with: tones, palettes, the
                           Text alphabet, the three orders, the message layout.
    source/Onset.*         spectral flux over the 64 bins, primed on frame one.
    source/Shaders.cpp     copy (mip chain); motor (one fragment per cell, the
                           state machine); board (halves, flaps, flutter, shade).
    source/Splitflap.*     the plugin: parameters, the clock, the update
                           decision, the three passes, the tables, test hooks.
    source/PassBuffer.*    tinsel's FFGLFBO with the leak fixed.
    source/{Clock,Diag,Font}  carried from graticule via pattern.
    tools/sftest/main.cpp  the harness: the six checks, negatives, --out,
                           --pipe, --bench, --list, --names, --font.
    tools/sweep.py         no control is silently dead, at two rasters.
    tools/verify.sh        all of it. Bash: the pipe step reads PIPESTATUS.
    tools/mutate.sh        one character of the shipped GLSL, per check.
    tools/clips.sh         Resolume's demo clips through the defaults, for a look.

### The state texel

RGBA32F, one per cell, ping-ponged between two `PassBuffer`s sized to the grid:

    r  c   the flap on show, 0..N-1
    g  p   the fall phase of the flap in the air, 0 < p < 1; 0 when none is
    b  t   the latched target
    a  a   seconds: > 0 the stagger still to wait; <= 0 minus the seconds since
           the last flap landed, which drives the flutter

    idle      t == c && p == 0        waiting   t != c && p == 0 && a > 0
    falling   p > 0                   armed     t != c && p == 0 && a <= 0

Advancing is closed-form: `np = p + dt / FlipTime`, `steps = floor( np )`,
compared with `remaining = ( t − c ) mod N` (N if that is 0 while a flap is
in the air: a target equal to the flap on show means the whole drum round).
No loop, no sampled comparator.

---

## Every number in the harness

All six checks read the rendered picture — never the state texture — through
the real plugin class, at 640×360 and 320×180, with a synthetic 60 fps clock
and the plugin's clock forced to seconds. The measurement board: full frame,
no gap, no split line, white flaps lit straight on (so a tone k of N draws as
0.06 + 0.94 k/(N−1), at least 34 levels apart for N ≤ 8), Tones dark to light,
no stagger, no bounce, updating continuously, 0.2 s a flap (12 frames).

| Check | The number | Where it comes from |
| --- | --- | --- |
| `--flips` | **0 of 32** cells wrong, both rasters | An 8×4 board of an 8-flap drum, picture A (tone (col+row) mod 8) then B ((3col+5row+2) mod 8). The count is the number of changes of the tone at the centre column of the top row of each cell's top half. The flap in the air uncovers that row once cos θ < 1 − 0.5/L, at u ≈ 0.3 of its fall, and the next flap covers it only once it has itself left, so every flap on the drum shows there for one flip time (12 frames) and cannot be missed. Tone reading is exact: 8-bit rounding of levels 34 apart. Run for 7×12 + 10 frames. |
| `--asymmetry` | up settles on frame **11**, down on **83**, ±1 | A 4×3 board, tone 3 → 4 → 3. "Settled" is the first frame from which every later frame of the run is bit-identical and shows the target; with e = 0 the landed flap is at exactly π, so the picture stops the frame it lands. The flap in the air has phase (j+1)/12 on the j-th frame after the change, so the d-th flap lands on frame 12d − 1: 11 and 83. The ±1 is the float accumulation of 1/12 twelve times, which need not reach 1.0 exactly. |
| `--settle` | **0 of 144** wrong after **127** frames, then **0 of 30** frames differ | A 16×9 board, tones k/7 + δ with δ within ±0.03 (never within 0.04 of a midpoint), 0.3 s stagger, e = 0.24. The bound is 7 flips × 0.2 s + 0.3 s + the flutter's tail, read from the plugin's own curve (`bounceEnd` = 2.9 flip units → 0.38 s), plus two frames. Nearest is unambiguous against the sixteen-tap mean because the taps stop at the cell's edge. |
| `--fall` | worst **0.49 px** over 59 frames; lands on frame **59**; flutter worst **0.48 px** over 40 frames | The smallest board (4×2), three flaps (0.06, 0.53, 1.0), a one-second flip from the middle one to the white one, light 60° from above, top-left cell. Height in the top half: rows from the hinge up that are not the white next symbol (≤ 127); in the bottom half: rows from the hinge down that are white (> 127) — at that light the white back is lit ≥ 0.65 to the stop, the plate at rest 0.65, the mid-grey front ≤ 0.34. A row's centre is covered when r + 0.5 < L|cos θ|, so the count is floor( L|cos θ| + 0.5 ): half a pixel from the centre rule, and the tolerance is **1 px** with the rest for the rasteriser. θ(t) is the **harness's own** velocity-Verlet solution (step 2e-5, a fifth of Flap.cpp's RK4 step), not the plugin's table. Landing on frame 59 is the (j+1)/60 phase convention reaching 1; ±1 frame for float accumulation. The flutter is measured from the landing frame the check itself observed. |
| `--resize` | **0 of 32** wrong (picture resize), **0 of 64** wrong (regrid) | As `--flips`, with the count carried across a 640×360 ↔ 320×180 resize on frame 30 of a 7-flip run, and across 8 → 16 columns on frame 30 where every child cell's count is its parent's (t − c) mod N. |
| `--prime` | fired **0**, moved on **0** of 60 frames; the real onset fires on frame **0** of its step | Onset mode, all 64 bins at 0.5 from frame 0, picture A latched on frame 0 and B from frame 1. A fire on frame 0 counts. Then bins to 1.0: flux 64(1 − √0.5) = 18.7 against a floor of ⅛ × 64 × 0.707 = 5.7 × 2.5. |
| sweep | **23 of 23** change the picture at both rasters | Two renders per control, low and high of its real range, against a context per control (`tools/sweep.py`). |
| pipe | **27,648** bytes for three 64×36 frames; exit **1** on hang-up | 64 × 36 × 4 × 3. `head -c 1` closes the pipe under a 4.6 MB write. |

## Would this hold on another rasteriser, at another raster?

- **`--flips`, `--asymmetry`, `--resize`, `--prime`, `--settle`** read whole
  pixels at whole-pixel positions on boards whose cells divide both rasters
  exactly (320 = 8 × 40 = 4 × 80 = 16 × 20; 180 = 4 × 45 = 3 × 60 = 9 × 20;
  640×360 likewise). The tones are ≥ 34 levels apart and every static plate
  is a flat fill, so no coverage, filtering or gamma is involved. They hold
  anywhere GL draws a flat quad.
- **`--fall`** measures a fractional edge (the flap's projected height) and
  says so: 1 px, of which 0.5 is the pixel-centre rule, and the rest is the
  rasteriser's. It held at 0.49 px here, i.e. the centre rule and nothing
  else; a rasteriser that samples elsewhere in the pixel has 0.5 px in hand.
  At 320×180 L is 45 px, so the 1 px tolerance is 2.2 % of L there and 1.1 %
  at 640×360 — both far under what the negative controls move (constant
  acceleration is 30 frames early; 15 % gravity is 4 frames early).
- **The float phase.** `p` accumulates 1/60 in float; twelve steps need not
  sum to 1.0 exactly, so every landing frame carries ±1. Nothing compares `p`
  against an exact multiple.
- **Nothing relies on exact cancellation.** The settled plate is bit-identical
  because the curve's table is pinned to exactly π past the last bounce and
  the idle timer is clamped, not because two floats cancel.
- **The cell mean** is sixteen taps at a whole mip level; a driver's mip
  generation (box) and bilinear weights (8-bit on some GPUs) can move a cell's
  mean by a level or two on a real picture. The checks feed flat cells, where
  every tap reads the same value.

## Negative controls

Shipped in `sftest --negative`, each through a `SplitflapPlugin::Debug` flag
the harness sets on the real plugin; **6 of 6 caught**:

| Broken model | Check | What failed |
| --- | --- | --- |
| backward flips allowed (the shorter way round) | `--flips` | 8 of 32 cells (those more than half a drum away) |
| backward flips allowed | `--asymmetry` | the darken settled on frame 12, not 83 |
| state cleared on a picture resize (the photofinish bug) | `--resize` | 28 of 32 cells |
| detector unprimed | `--prime` | a fire on frame 0, then deaf to the real onset (the snapped floor) |
| θ″ = constant | `--fall` | 6 of 6: lands on frame 29, shape off by up to 60 px |
| gravity 15 % high, flip time not following | `--fall` | 6 of 6: lands on frame 55 |

The prime negative was **missed** the first time: an unprimed detector fires on
frame 0, and frame 0 is where the board latches anyway, so nothing visible
happened. The check now counts a fire on frame 0 (it is the trap itself), and
the second half — the real onset later — catches the snapped floor.

## Mutation test

**Run on the committed tree (2026-09-24, 4514a41), two ways.** First by hand,
the brief's way: one character of the shipped GLSL, in the board shader's top
half, `float h = 0.5 * cos( theta )` became `0.6 * cos( theta )` — the flap in
the air drawn 20 % too tall while it is above the horizontal. `--fall` failed
its height comparison at both rasters (worst 14.67 px against L = 90 at
640×360, 7.60 px against 45 at 320×180); the landing and flutter assertions
held, as they should — the mutant touches neither. **`--flips` failed too**,
15 of 32 cells at each raster, every one over-counted by one or two: the
taller flap keeps the top row covered into angles where its Lambert shade has
moved the read tone a step, and each step counts as a transition. That is a
fact about the count worth having: it relies on the top row being uncovered
(u ≈ 0.3) before the shade moves a tone, which the correct height gives with
room to spare and 20 % more does not. `--asymmetry` and `--settle` were not
affected. Reverted with `git checkout -- source/Shaders.cpp`; `git diff
--stat -- source` read empty, and after the rebuild `--flips` and `--fall`
passed again.

Then `tools/mutate.sh`, which builds each mutant in its own copy of the tree
and never touches the working tree: the same height mutant (caught by
`--fall`); `( c + dir * steps ) % N + N` with the `+ N` made `- N`, a flap
index that goes negative, caught by `--flips` on every cell that wraps (6
assertions); `remaining = ( ( t - c ) % N + N ) % N` with the last `N` made
`9`, the flaps still to pass counted off the drum, caught by `--asymmetry` on
the darken at both rasters (4 assertions); and a control for the controls, a
whitespace-only edit to `Flap.cpp`, which builds and passes `--fall` as it
must. 4 of 4 behaved as expected.

---

## The traps

Ordered by how much time they cost.

**Comparing the picture against the plugin's own table proves only that a
texture can be read.** The first `--fall` used `CurveForTest()` — the table
the shader draws from — and passed to 0.49 px. Both negative controls (a
constant acceleration, gravity detuned) perturbed that same table, the picture
followed it, and the shape comparison *still passed*; only the landing-frame
assertion caught them. The harness now integrates the physics itself, with a
different method, and compares against that. The addendum's "a second copy of
the same formula" trap, met exactly as described.

**The controls' minima.** The fall check asked for a 1×1 board and got 4×2:
Columns clamps at 4 and Rows at 2, so the picture had two cells stacked and
every height read double. It looked like the shader drawing the cell twice.
Checks now use `kColumnsMin × kRowsMin` and say so.

**A shaded flap can match the plate it is over.** Lit straight on, the white
back of the flap at 100° is 0.3 + 0.7 cos(80°) = 0.42, and the mid-grey plate
under it is 0.53: the measurement read 0 rows for five frames. The check lights
the board 60° from above, where the back never drops below 0.65 and the front
never rises above 0.34, and 127 separates them at every angle.

**Trilinear at the cell's level bleeds the neighbours in.** One `textureLod`
at log2(cell) blends a texel wider than the cell across its boundary. The
motor samples sixteen taps at a whole level whose texel is at most an eighth
of the cell, at ±1/8 and ±3/8: a tap's bilinear footprint reaches the edge and
stops.

**Zero stagger must not restart the flutter.** Setting `a = Stagger × hash`
on a new target wrote 0 over the idle timer, so the flap that had just landed
bounced again when the next target arrived. A zero wait leaves `a` alone.

**Bounce, Light Angle and Drum Order read as dead** in the first sweep, all
correctly: Bounce moves a flap a twentieth of a pixel one frame after landing
on the default board; the plate at rest is lit by cos(angle), which is even,
so the slider's two ends draw the same picture; Drum Order changes only the
path round the drum, and every order settles on the same tone. Each has a
context in `sweep.py` (big cells five frames after a landing; straight on
against 60°; mid-run).

**Two 64-bit facts about the pipe.** With no stdin, the first version wrote
one frame and stopped, because the second read returned 0 and the loop took
that for a source running out; a source that was never there films the card
for `--frames`. And `head -c 1` must see the harness exit 1: SIGPIPE ignored,
write() fails, the loop says so.

**zsh's `=word` expansion.** `echo ====` fails in zsh with "==== not found";
verify.sh is bash for a different reason (PIPESTATUS) and does not hit it.

Inherited from the fleet and honoured without incident: the OBJECT library;
`SetTextParameter` returning success for the About block; the 0..1 clamp on
STANDARD defaults (the counts are `FF_TYPE_INTEGER`); an option's range
reading back 0..1; `StoatworksAboutParams.h` after the SDK; the viewport
captured before the passes; every `Ensure()` before anything binds a texture;
integer hashing; the reserved GLSL words; the synthetic clock; the shader
never seeing an absolute time.

---

## Decisions taken without asking

**The state is grid-sized, not picture-sized.** The spec says "reallocated on
resize without losing the flap index". A texel per cell has no reason to
follow the picture's size at all, so a picture resize never touches it, and
the resize trap moves to the grid: a Columns or Rows change writes a buffer of
the new size while reading the old one, and each new cell takes the old cell
at the same place on the board. `--resize` checks both.

**An initial latch on frame one in every Update mode.** A board in Onset or
Manual mode would otherwise be blank until the first onset or press, which on
a layer reads as broken. The first frame latches whatever is under it; the
mode decides every refresh after. `--prime` therefore counts flips from frame
one, and a detector that fires on frame 0 is still caught by the floor it
snaps.

**Fit is a control the spec did not list.** With Columns, Rows and Cell Aspect
all free the board rarely matches the frame; on by default the cells stretch
to fill it, and off the board is centred with a transparent surround (so it
composites over the layers below). The letterbox is transparent, not black,
for the same reason.

**Text: the message is the target where the clip is bright.** Each cell shows
its character of the message when its mean luma is ≥ 0.5, else the blank; the
drum is a fixed alphabet of 45 (blank first), so a letter is a run of flips
from the blank and back. `Flaps` is ignored in Text mode. The threshold is a
constant; a control for it was left out of 0.1.0.

**Drum Order keeps the Text blank at flap 0** and orders the rest, so
Shuffled still clears to blank in one direction.

**Module Width shares the stagger, not the phase.** Cells in a module take
the same start delay, so a row of them on one drive starts together; cells
whose targets differ still land at different times. Sharing the shaft's phase
between cells would need each fragment to read its neighbours; not for 0.1.0.

**Stagger is a start delay, not a phase offset.** The spec's settle bound
("max flips × Flip Time + stagger") reads as a delay; a phase offset on a
continuously running shaft would wrap at one flip time.

**Perspective is a widening of the free edge**, 1 + 0.10 sin θ at the edge
tapering to the hinge, drawn as a trapezoid. The projected height stays
orthographic (L|cos θ|), which is what keeps `--fall` invertible: a true
perspective height L cos θ / (1 − ρ sin θ) is not monotone near the top.

**The Lambert light is in the vertical plane only** (elevation ±60°). The
plate at rest is lit by cos(angle); the front face of a falling flap by
cos(θ + angle), the back by −cos(θ + angle); ambient 0.30.

**The cell mean is the mip pyramid**, not a reduce pass: sixteen taps at a
whole level per cell in the motor pass, 5,184 cells at most.

**Release at three degrees from rest.** At exactly vertical an inverted
pendulum never falls; the log-slow departure from three degrees is the
hesitation a real flap shows, and it puts the top row's reveal at u ≈ 0.3,
which is what makes the flip count readable off the picture. The
dimensionless fall time is 5.03.

**The flutter ends at half a degree.** Impacts reverse the velocity times e
until a rebound would rise less than 0.5°; the table is then exactly π, so a
settled board is bit-identical. Bounce maps to e = 0..0.6: at 0.6 the first
rebound reaches 72°.

**Onset is one detector over all 64 bins.** Root magnitude, flux, a
one-second floor seeded at an eighth of the level on frame one, fire on a
rising frame above 2.5 × floor (and 0.02), 100 ms refractory. No bin is mapped
to a frequency. The constants were set on synthetic spectra.

**Interval ticks from the mode's start, and a stall longer than the interval
gives one update, not a burst.**

**The frame delta is clamped to 0.25 s**, so a scrub or a sleep advances the
board by a quarter second rather than the whole drum.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement on this machine (M4 Max, macOS 26.4), `tools/verify.sh` green:**
everything in the table above, at both rasters; all six negative controls
caught; the mutation test; 23 of 23 controls alive at both rasters; the pipe's
byte count and hang-up status; the four shaders through glslc; the bundle
universal, exporting `plugMain`, ad-hoc signed, probed under oxbow as
SW Splitflap / SF01 / effect and rendering frames there; the render cost
(README).

**Looked at, not measured:** ten of Resolume's bundled demo clips through the
defaults (`tools/clips.sh`): every one reads as its picture, the churn shows
as split cells, none floods to a single tone.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** How the Text parameter presents on an
  effect, whether four groups of 23 read well, whether the RGB triple shows as
  a swatch: untested.
- **No real audio.** The detector's thresholds are from synthetic spectra; the
  bins' magnitude law is unmeasured and the detector deliberately assumes
  nothing about it.
- **This Mac's GPU only.** CI is written and unrun.
- No presets, no OpenFX port, no browser demo, no user guide.

## Open questions

1. **Shared shaft phase within a module.** Cells on one drive should flip in
   lockstep, not merely start together; that needs a per-module phase, i.e. a
   fragment reading its module's first cell or a second small pass.
2. **The Text threshold** (0.5 luma) is a constant. A control, or the clip's
   luma driving which *character* of a per-cell drum shows, are both plausible.
3. **A reduce pass for the cell mean** would make the target independent of
   the driver's mip generation; the pyramid was chosen for cost.
4. **The onset constants** want a session with programme material through
   Resolume's FFT.
5. **Cost at 4K on the largest board** is a third of a frame. The board shader
   fetches the state texel and the drum per pixel and branches per half; a
   coarser pass per cell for the static halves would cut most of it.
