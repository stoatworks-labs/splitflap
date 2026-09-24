# Attributions

Splitflap is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Graticule bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Font.{h,cpp}, the 5×7 bitmap font every character on the Text drum is printed with, carried across from graticule unchanged by way of needle and pattern. It is graticule's own design, drawn as pictures in Font.cpp; not a copy of, and not traced from, any board's typeface or any computer's ROM font.

### Diag logger and host Clock — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* (the log-file writer, from orrery via graticule) and source/Clock.* (the clock that measures what unit the host's SetTime arrives in, from flipbook via graticule and pattern), renamed into this namespace.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.*, the SDK's FFGLFBO with the colour-texture leak fixed and the sampling mode owned by the buffer. The copy buffer and the two state buffers are PassBuffers.

### Effect template and harness plumbing — Stoatworks tinsel, pattern and asciify

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

tinsel is the structural template for an effect: the CMake shape, the OBJECT core, the viewport capture before the passes, the About headers, the synthetic-clock harness with --list/--out/--pipe/--bench and a PNG writer, tools/sweep.py, tools/verify.sh and the two workflows. pattern's harness supplied the --pipe hang-up handling (SIGPIPE ignored, exit 1), the cue script and the two-raster discipline; asciify the cell-as-unit framing. Nothing of any of their effects is here.

### Onset detector shape — Stoatworks pattern

<https://github.com/stoatworks-labs/pattern>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Onset.* follows pattern's tracker: spectral flux over root magnitude, a floor that follows over one second and moves only when time has passed, a rising-edge fire with a refractory, and priming on frame one. Here it is one detector over all 64 bins rather than one per band.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The split-flap display, as a mechanism

A drum of printed flaps turned one way by a motor, hinged plates that fall and slap on to a stop: the public mechanism of every departures board since the 1950s, and all that was taken. No manufacturer's flap artwork, typeface, colour, dimensions or sound was used or measured; the fall is the textbook rigid plate about an edge, the palettes are authored here, the font is graticule's, and the plugin makes no sound because FFGL has no audio output. No maker is named or depicted on screen.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.709** — The luma weights (0.2126, 0.7152, 0.0722) a cell's mean is read with when the drum is printed with tones or a message.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
