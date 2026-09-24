#pragma once

/**
    The three passes, as GLSL source.

    1. **copy**   picture size. Resolves MaxUV once and carries a mip chain,
                  so the motor pass can read a cell's mean with a few
                  fetches at the cell's own level.
    2. **motor**  grid size (Columns x Rows). One fragment per cell: reads the
                  previous state, latches a new target when the update fires,
                  and advances the drum by the frame's time. This is the one
                  constraint the plugin is built on: a cell can only turn
                  forward, one flap at a time, at the motor's rate. The state
                  is a float texel per cell -- see kMotorShader for the
                  encoding -- carried across frames, and re-mapped rather than
                  cleared when the grid changes.
    3. **board**  output size. Draws the static top and bottom halves, the
                  falling flap in perspective and the landed flap's flutter,
                  and shades each from its angle. Reads the flap curve the
                  CPU solved.

    Every shader is `#version 410 core`. Reserved words avoided as identifiers:
    patch sample input output filter common active half layout flat.

    Each shader is written as several adjacent raw strings, joined by the
    compiler: MSVC refuses a single literal past about 16 KB (C2026), and
    `tools/verify.sh`'s extraction joins them the same way.
*/
namespace splitflap
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kMotorShader;
extern const char* const kBoardShader;

} // namespace splitflap
