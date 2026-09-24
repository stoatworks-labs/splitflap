# splitflap

The picture on a split-flap departures board, as an FFGL **effect** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT. Intended home `github.com/stoatworks-labs/splitflap`.

Read `AGENTS.md` before changing the motor, the state encoding, the flap curve
or the parameter list.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64` (use a different
  directory, e.g. `build-dev`; `tools/verify.sh` deletes and rebuilds `build`)
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (untested; never run by this repo's own workflow)
- Render a frame offline: `./build/sftest --out /tmp/f.png --size 1920x1080 --frames 120`
  (`--drift 2` walks the test card sideways so the board has something to chase)
- Set anything by name: `--set "Columns=16" --set "Drum=2" --set "Text=GATE 9"`
  (options by index: Drum 0 Tones, 1 Colours, 2 Text; Drum Order 0 Dark to
  Light, 1 Light to Dark, 2 Shuffled; Update 0 Continuous, 1 Interval, 2 Onset,
  3 Manual; Palette 0 Amber, 1 Airport, 2 Rainbow, 3 Ocean, 4 Ember, 5 Candy)
- Frames for video: `./build/sftest --pipe --size 1920x1080 --fps 60 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov`
  — a cue script is `frame  Parameter Name  value`; standard parameters ramp
  between keys, options/booleans/integers/events STEP. A reader that hangs up
  gets exit 1 (SIGPIPE is ignored). With nothing on stdin and `--frames N` it
  films the test card.
- List parameters: `./build/sftest --list` (prints the real range of an option)
- Demo clips: `tools/clips.sh [out-dir]` (needs ffmpeg and a Resolume install)

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check + two sweeps, ~3 min; bash, not zsh)
- **The checks, each read off the rendered picture at 640x360 and 320x180:**
  - `./build/sftest --flips` — every cell makes (t − c) mod N flips, as tone changes on its top row
  - `./build/sftest --asymmetry` — one step brighter is one flip time, one darker N − 1
  - `./build/sftest --settle` — a still input lands on the nearest flap within the bound, then bit-identical
  - `./build/sftest --fall` — the flap's projected height against the harness's own rigid-body solution; lands on frame 59 of a one-second flip
  - `./build/sftest --resize` — a picture resize and an 8 → 16 column regrid mid-flip keep every flap
  - `./build/sftest --prime` — loud audio on frame one fires nothing; a real onset fires
  - `./build/sftest --negative` — every check against its broken model; each must fail
  - `./build/sftest --names` — no name over 16 characters, none duplicated
  - `./build/sftest --font` — the Text drum's alphabet, as glyphs
- Cost: `./build/sftest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`, `--binary PATH`)
- The mutation test: `tools/mutate.sh` (one character of the shipped GLSL; a few minutes)

## Notes
- **The motor shader is the plugin.** One fragment per cell holds the state
  (flap, phase, target, timer) in an RGBA32F texel, latches a target when the
  update fires, and advances by the frame's `Dt`. A cell only ever moves
  forward, one flap at a time. A wrong count is a motor fix; a wrong pixel is
  a board-shader fix.
- **Time never reaches the shader as an absolute.** The CPU's `Clock` settles
  the host's unit and hands the motor a clamped frame delta. Resolume's clock
  is ~499 million ms; a float phase from it would step by 0.03 s.
- **The state's size is the grid's, not the picture's.** A picture resize does
  not touch it; a Columns/Rows change re-maps the old state onto the new grid
  in the motor pass (the buffer being written is sized to the new grid, the one
  being read is whatever the last frame's grid was).
- **Frame one primes the onset detector.** Previous spectrum = current, floor
  seeded from the level; the floor only moves when `dt > 0`.
- **A cell's mean is sixteen taps at a whole mip level** whose texel is at most
  an eighth of the cell, so the taps' footprints stop at the cell's edge.
  Trilinear at the cell's own level bleeds the neighbours in.
- **Options map by index in `Controls.cpp` (`OptionIndex`)**; the SDK's range
  for an option reads back 0..1 whatever the element count, and `--list`
  prints the real one for the sweep.
- **GLSL reserved words**: `patch sample input output filter common active half
  layout flat`. The plate's rest shade is `plateShade`, not `flat`.
- `SetParamInfo` clamps a STANDARD default into 0..1; Columns, Rows, Flaps and
  Module Width are `FF_TYPE_INTEGER`, which is exempt.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin. Here it also stores `Text`.
- `splitflap_core` is an OBJECT library, not STATIC.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `SF01`. Display name `SW Splitflap`. Bundle id `com.stoatworks.ffgl.splitflap`.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by stoatworks-backend
  (`scripts/sync-about.py` from the website's projects.json, and
  `scripts/sync-attributions.py`); never edit them here. The About block is five
  entries (text plus four buttons, `PT_ABOUT_BUTTON_4`), and `Splitflap.cpp`'s
  `static_assert` holds the count to `about::kParamCount`.
- The Text drum's threshold is a quarter of full luma (`targetFor` in the motor
  shader); it was a half until the release survey showed no bundled demo clip
  cleared it on more than 18 of 120 cells.
- `docs/USER-GUIDE.md` is the user guide's only source; the site page and
  `docs/USER-GUIDE.pdf` are built from it by the website's `build_guides.py`.

## Not done yet
- Never loaded into Resolume on macOS; no real audio has reached it in a host.
  CI runs the checks and the sweep on GitHub's macOS runner (software GL) and
  builds the Windows DLL.
- No presets, no OpenFX port. The user guide is `docs/USER-GUIDE.md`; the
  browser demo is `demo/` (see the section at the end of this file).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records the GL driver, which shader failed, the host clock's unit
and whether audio reached the layer.

    ~/Library/Logs/splitflap/splitflap.YYYY-MM-DD.log

## The browser demo
- `demo/` is the page at splitflap-demo.stoatworks-labs.com: the four shaders
  and the font table verbatim in `demo/plugin.js` (written by
  `demo/tools/splice_shaders.py`), over a JS port of `Flap.cpp`, `Drum.cpp`,
  `Font.cpp`, `Onset.cpp`, `Controls.cpp` and `ProcessOpenGL`'s sequence. No
  audio; Update Now is a button; the integer controls are dropdowns.
- `python3 demo/tools/check_shaders.py` holds every shader and the font to the
  C++; `tools/verify.sh` runs it. Change a shader: run the splice, never edit
  the JS copy.
- `demo/vendor/` is the shared kit, vendored by stoatworks-backend's
  `resolume-demo/sync.sh`; never edit it.
- Deploys on push to main (`.github/workflows/deploy.yml`), or by hand with
  `cf-run npx wrangler deploy`. The host is a Worker ROUTE behind a proxied
  AAAA record, not a custom domain (the zone is at Cloudflare's 100-domain
  limit).
