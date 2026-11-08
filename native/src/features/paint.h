// Drawing helpers on top of Chrome's own gfx::Canvas: antialiased round rects and lines
// (through a cc::PaintFlags built from the offsets table), text in the UI font, and theme
// colours from the view's ui::ColorProvider. Everything degrades to Canvas::FillRect when a
// piece is missing from the offsets table.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

#include "core/offsets.h"

namespace arbortab {

// Colours of the current browser theme, as ARGB.
struct Ink {
  uint32_t primary;            // accent (kColorSysPrimary)
  uint32_t on_surface;         // body text
  uint32_t on_surface_subtle;  // secondary text and glyphs
  uint32_t tonal;              // tonal container (badges)
};

bool InitPaint(HMODULE chrome_dll, const Offsets& offsets);
// True when antialiased shapes and text are available (otherwise only FillRect works).
bool PaintIsRich();

// Theme colours for `view` (any views::View inside a browser window).
Ink ThemeInk(const void* view);
// Accent for drop targets: the configured colour, or the theme's primary.
uint32_t AccentColor(const Ink& ink);

// `color` with its alpha multiplied by `opacity` (0..1).
uint32_t Fade(uint32_t color, float opacity);
// `color` with alpha replaced by `alpha`.
uint32_t WithAlpha(uint32_t color, uint8_t alpha);

void FillRect(void* canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color);
void FillRoundRect(void* canvas, float x, float y, float width, float height, float radius,
                   uint32_t color);
void StrokeRoundRect(void* canvas, float x, float y, float width, float height, float radius,
                     float stroke, uint32_t color);
// Round-capped line.
void StrokeLine(void* canvas, float x1, float y1, float x2, float y2, float stroke, uint32_t color);

// Small UI font (one step below the label default). Width/height are 0 when text is
// unavailable.
int32_t SmallTextWidth(std::u16string_view text);
int32_t SmallTextHeight();
void DrawSmallText(void* canvas, std::u16string_view text, uint32_t color, int32_t x, int32_t y,
                   int32_t width, int32_t height);

// Text centred in the rectangle (falls back to left-aligned DrawSmallText).
void DrawSmallTextCentered(void* canvas, std::u16string_view text, uint32_t color, int32_t x,
                           int32_t y, int32_t width, int32_t height);

// Layer helpers: everything painted between the two calls is composited with `alpha`.
void BeginLayerAlpha(void* canvas, uint8_t alpha);
void EndLayer(void* canvas);
bool LayersAvailable();

}  // namespace arbortab
