#include "features/paint.h"

#include <cstring>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/state.h"

namespace arbortab {
namespace {

// libc++ std::u16string_view; MSVC passes this 16-byte aggregate by reference to a copy.
struct U16View {
  const char16_t* data;
  size_t size;
};

struct RectF {
  float x, y, width, height;
};

struct PointF {
  float x, y;
};

using FillRectFn = void (*)(void* canvas, const int32_t* rect, uint32_t color);
using PaintFlagsCtorFn = void* (*)(void* flags);
using DrawRoundRectFn = void (*)(void* canvas, const RectF* rect, float radius, const void* flags);
using DrawLineFn = void (*)(void* canvas, const PointF* p1, const PointF* p2, const void* flags);
using GetColorProviderFn = const void* (*)(const void* view);
using GetColorFn = uint32_t (*)(const void* provider, int id);
using DefaultFontListFn = const void* (*)();
using DeriveFontFn = void* (*)(const void* font_list, void* out, int size_delta);
using FontHeightFn = int (*)(const void* font_list);
using StringWidthFn = int (*)(const U16View* text, const void* font_list);
using DrawStringRectFn = void (*)(void* canvas, const U16View* text, const void* font_list,
                                  uint32_t color, const int32_t* rect);
using DrawStringRectFlagsFn = void (*)(void* canvas, const U16View* text, const void* font_list,
                                       uint32_t color, const int32_t* rect, int flags);
using SaveLayerAlphaFn = void (*)(void* canvas, uint8_t alpha);
using RestoreFn = void (*)(void* canvas);

constexpr size_t kMaxPaintFlagsSize = 0x80;

struct PaintApi {
  FillRectFn fill_rect = nullptr;
  PaintFlagsCtorFn flags_ctor = nullptr;
  DrawRoundRectFn draw_round_rect = nullptr;
  DrawLineFn draw_line = nullptr;
  GetColorProviderFn color_provider = nullptr;
  GetColorFn get_color = nullptr;
  DefaultFontListFn default_font = nullptr;
  DeriveFontFn derive_font = nullptr;
  FontHeightFn font_height = nullptr;
  FontHeightFn font_baseline = nullptr;
  FontHeightFn font_cap_height = nullptr;
  StringWidthFn string_width = nullptr;
  DrawStringRectFn draw_string = nullptr;
  DrawStringRectFlagsFn draw_string_flags = nullptr;
  SaveLayerAlphaFn save_layer_alpha = nullptr;
  RestoreFn restore = nullptr;

  uint32_t flags_size = 0;
  uint32_t off_color = 0;
  uint32_t off_width = 0;
  uint32_t off_bits = 0;
  BitField antialias;
  BitField cap;
  BitField style;
  bool shapes = false;

  int color_primary = -1;
  int color_on_surface = -1;
  int color_on_surface_subtle = -1;
  int color_tonal = -1;
};

PaintApi g_api;
alignas(16) uint8_t g_small_font[16];
bool g_small_font_ready = false;

constexpr Ink kFallbackInk{0xFF0B57D0, 0xFF1F1F1F, 0xFF474747, 0xFFD3E3FD};

void SetBits(uint32_t& word, const BitField& field, uint32_t value) {
  const uint32_t mask = ((1u << field.width) - 1u) << field.bit;
  word = (word & ~mask) | ((value << field.bit) & mask);
}

// cc::PaintFlags::Style / Cap
constexpr uint32_t kFillStyle = 0;
constexpr uint32_t kStrokeStyle = 1;
constexpr uint32_t kRoundCap = 1;

// Builds a cc::PaintFlags in `storage`. The default constructor leaves every sk_sp member
// null, so the object owns nothing and needs no destructor call.
const void* MakeFlags(uint8_t* storage, uint32_t color, bool stroke, float stroke_width) {
  g_api.flags_ctor(storage);
  const float rgba[4] = {static_cast<float>((color >> 16) & 0xFF) / 255.f,
                         static_cast<float>((color >> 8) & 0xFF) / 255.f,
                         static_cast<float>(color & 0xFF) / 255.f,
                         static_cast<float>(color >> 24) / 255.f};
  memcpy(storage + g_api.off_color, rgba, sizeof(rgba));
  memcpy(storage + g_api.off_width, &stroke_width, sizeof(stroke_width));
  uint32_t bits = 0;
  memcpy(&bits, storage + g_api.off_bits, sizeof(bits));
  SetBits(bits, g_api.antialias, 1);
  SetBits(bits, g_api.style, stroke ? kStrokeStyle : kFillStyle);
  if (stroke) {
    SetBits(bits, g_api.cap, kRoundCap);
  }
  memcpy(storage + g_api.off_bits, &bits, sizeof(bits));
  return storage;
}

const void* SmallFont() {
  if (g_small_font_ready) {
    return g_small_font;
  }
  if (!g_api.default_font) {
    return nullptr;
  }
  const void* base = g_api.default_font();
  if (!base) {
    return nullptr;
  }
  if (!g_api.derive_font) {
    return base;
  }
  // gfx::FontList is a single scoped_refptr; this one is intentionally never released.
  g_api.derive_font(base, g_small_font, -1);
  g_small_font_ready = true;
  return g_small_font;
}

template <typename Fn>
Fn Function(uint8_t* base, const Offsets& offsets, const char* name) {
  const auto found = offsets.functions.find(name);
  return found == offsets.functions.end() ? nullptr
                                          : reinterpret_cast<Fn>(base + found->second.rva);
}

}  // namespace

bool InitPaint(HMODULE chrome_dll, const Offsets& offsets) {
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  PaintApi api;
  api.fill_rect = Function<FillRectFn>(base, offsets, "gfx::Canvas::FillRect");
  api.flags_ctor = Function<PaintFlagsCtorFn>(base, offsets, "cc::PaintFlags::PaintFlags");
  api.draw_round_rect = Function<DrawRoundRectFn>(base, offsets, "gfx::Canvas::DrawRoundRect");
  api.draw_line = Function<DrawLineFn>(base, offsets, "gfx::Canvas::DrawLine");
  api.color_provider =
      Function<GetColorProviderFn>(base, offsets, "views::View::GetColorProvider");
  api.get_color = Function<GetColorFn>(base, offsets, "ui::ColorProvider::GetColor");
  api.default_font =
      Function<DefaultFontListFn>(base, offsets, "views::Label::GetDefaultFontList");
  api.derive_font = Function<DeriveFontFn>(base, offsets, "gfx::FontList::DeriveWithSizeDelta");
  api.font_height = Function<FontHeightFn>(base, offsets, "gfx::FontList::GetHeight");
  api.font_baseline = Function<FontHeightFn>(base, offsets, "gfx::FontList::GetBaseline");
  api.font_cap_height = Function<FontHeightFn>(base, offsets, "gfx::FontList::GetCapHeight");
  api.string_width = Function<StringWidthFn>(base, offsets, "gfx::GetStringWidth");
  api.draw_string = Function<DrawStringRectFn>(base, offsets, "gfx::Canvas::DrawStringRect");
  api.draw_string_flags =
      Function<DrawStringRectFlagsFn>(base, offsets, "gfx::Canvas::DrawStringRectWithFlags");
  api.save_layer_alpha = Function<SaveLayerAlphaFn>(base, offsets, "gfx::Canvas::SaveLayerAlpha");
  api.restore = Function<RestoreFn>(base, offsets, "gfx::Canvas::Restore");

  const auto field = [&offsets](const char* name, uint32_t& out) {
    const auto found = offsets.fields.find(name);
    if (found == offsets.fields.end()) {
      return false;
    }
    out = found->second;
    return true;
  };
  const auto bits = [&offsets](const char* name, BitField& out) {
    const auto found = offsets.bitfields.find(name);
    if (found == offsets.bitfields.end() || !found->second.width) {
      return false;
    }
    out = found->second;
    return true;
  };
  const auto size = offsets.sizes.find("cc::PaintFlags");
  api.flags_size = size == offsets.sizes.end() ? 0 : size->second;
  api.shapes = api.flags_ctor && api.draw_round_rect && api.draw_line && api.flags_size &&
               api.flags_size <= kMaxPaintFlagsSize &&
               field("cc::CorePaintFlags.color_", api.off_color) &&
               field("cc::CorePaintFlags.width_", api.off_width) &&
               field("cc::CorePaintFlags.bitfields_", api.off_bits) &&
               bits("cc::PaintFlags.antialias_", api.antialias) &&
               bits("cc::PaintFlags.cap_type_", api.cap) &&
               bits("cc::PaintFlags.style_", api.style);

  const auto constant = [&offsets](const char* name) {
    const auto found = offsets.constants.find(name);
    return found == offsets.constants.end() ? -1 : found->second;
  };
  api.color_primary = constant("ui::kColorSysPrimary");
  api.color_on_surface = constant("ui::kColorSysOnSurface");
  api.color_on_surface_subtle = constant("ui::kColorSysOnSurfaceSubtle");
  api.color_tonal = constant("ui::kColorSysTonalContainer");

  g_api = api;
  ARB_INFO("paint: shapes=%s text=%s theme=%s layers=%s", api.shapes ? "on" : "off",
           api.draw_string && api.string_width && api.default_font ? "on" : "off",
           api.color_provider && api.get_color && api.color_primary >= 0 ? "on" : "off",
           LayersAvailable() ? "on" : "off");
  return api.fill_rect != nullptr;
}

bool PaintIsRich() { return g_api.shapes; }

bool LayersAvailable() { return g_api.save_layer_alpha && g_api.restore; }

Ink ThemeInk(const void* view) {
  Ink ink = kFallbackInk;
  if (!view || !g_api.color_provider || !g_api.get_color) {
    return ink;
  }
  const void* provider = g_api.color_provider(view);
  if (!provider) {
    return ink;
  }
  const auto get = [provider](int id, uint32_t fallback) {
    return id >= 0 ? g_api.get_color(provider, id) : fallback;
  };
  ink.primary = get(g_api.color_primary, ink.primary);
  ink.on_surface = get(g_api.color_on_surface, ink.on_surface);
  ink.on_surface_subtle = get(g_api.color_on_surface_subtle, ink.on_surface_subtle);
  ink.tonal = get(g_api.color_tonal, ink.tonal);
  return ink;
}

uint32_t AccentColor(const Ink& ink) {
  const uint32_t configured = GlobalState().Config().highlight_color;
  // Only the hue of a configured colour is used; the painters choose their own opacity.
  return configured ? (configured | 0xFF000000u) : ink.primary;
}

uint32_t Fade(uint32_t color, float opacity) {
  if (opacity <= 0.f) {
    return color & 0x00FFFFFFu;
  }
  if (opacity >= 1.f) {
    return color;
  }
  const auto alpha = static_cast<uint32_t>(static_cast<float>(color >> 24) * opacity + 0.5f);
  return (alpha << 24) | (color & 0x00FFFFFFu);
}

uint32_t WithAlpha(uint32_t color, uint8_t alpha) {
  return (static_cast<uint32_t>(alpha) << 24) | (color & 0x00FFFFFFu);
}

void FillRect(void* canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color) {
  if (!g_api.fill_rect || width <= 0 || height <= 0 || !(color >> 24)) {
    return;
  }
  const int32_t rect[4] = {x, y, width, height};
  g_api.fill_rect(canvas, rect, color);
}

void FillRoundRect(void* canvas, float x, float y, float width, float height, float radius,
                   uint32_t color) {
  if (width <= 0.f || height <= 0.f || !(color >> 24)) {
    return;
  }
  if (!g_api.shapes) {
    FillRect(canvas, static_cast<int32_t>(x), static_cast<int32_t>(y),
             static_cast<int32_t>(width), static_cast<int32_t>(height), color);
    return;
  }
  alignas(16) uint8_t storage[kMaxPaintFlagsSize];
  const RectF rect{x, y, width, height};
  g_api.draw_round_rect(canvas, &rect, radius, MakeFlags(storage, color, false, 0.f));
}

void StrokeRoundRect(void* canvas, float x, float y, float width, float height, float radius,
                     float stroke, uint32_t color) {
  if (width <= 0.f || height <= 0.f || !(color >> 24)) {
    return;
  }
  if (!g_api.shapes) {
    const auto ix = static_cast<int32_t>(x), iy = static_cast<int32_t>(y);
    const auto iw = static_cast<int32_t>(width), ih = static_cast<int32_t>(height);
    const auto t = stroke < 1.f ? 1 : static_cast<int32_t>(stroke + 0.5f);
    FillRect(canvas, ix, iy, iw, t, color);
    FillRect(canvas, ix, iy + ih - t, iw, t, color);
    FillRect(canvas, ix, iy, t, ih, color);
    FillRect(canvas, ix + iw - t, iy, t, ih, color);
    return;
  }
  // Skia centres the stroke on the outline: inset by half the width to stay inside.
  alignas(16) uint8_t storage[kMaxPaintFlagsSize];
  const float half = stroke / 2.f;
  const RectF rect{x + half, y + half, width - stroke, height - stroke};
  g_api.draw_round_rect(canvas, &rect, radius > half ? radius - half : 0.f,
                        MakeFlags(storage, color, true, stroke));
}

void StrokeLine(void* canvas, float x1, float y1, float x2, float y2, float stroke,
                uint32_t color) {
  if (!(color >> 24)) {
    return;
  }
  if (!g_api.shapes) {
    // Axis-aligned fallback only.
    const auto t = stroke < 1.f ? 1 : static_cast<int32_t>(stroke + 0.5f);
    if (y1 == y2) {
      const float left = x1 < x2 ? x1 : x2;
      FillRect(canvas, static_cast<int32_t>(left), static_cast<int32_t>(y1 - stroke / 2.f),
               static_cast<int32_t>(x1 < x2 ? x2 - x1 : x1 - x2), t, color);
    } else if (x1 == x2) {
      const float top = y1 < y2 ? y1 : y2;
      FillRect(canvas, static_cast<int32_t>(x1 - stroke / 2.f), static_cast<int32_t>(top), t,
               static_cast<int32_t>(y1 < y2 ? y2 - y1 : y1 - y2), color);
    }
    return;
  }
  alignas(16) uint8_t storage[kMaxPaintFlagsSize];
  const PointF from{x1, y1};
  const PointF to{x2, y2};
  g_api.draw_line(canvas, &from, &to, MakeFlags(storage, color, true, stroke));
}

int32_t DefaultTextWidth(std::u16string_view text) {
  const void* font = g_api.string_width && g_api.default_font ? g_api.default_font() : nullptr;
  if (!font) {
    return 0;
  }
  const U16View view{text.data(), text.size()};
  return g_api.string_width(&view, font);
}

int32_t SmallTextWidth(std::u16string_view text) {
  const void* font = g_api.string_width ? SmallFont() : nullptr;
  if (!font) {
    return 0;
  }
  const U16View view{text.data(), text.size()};
  return g_api.string_width(&view, font);
}

int32_t SmallTextHeight() {
  const void* font = g_api.font_height ? SmallFont() : nullptr;
  return font ? g_api.font_height(font) : 0;
}

void DrawSmallText(void* canvas, std::u16string_view text, uint32_t color, int32_t x, int32_t y,
                   int32_t width, int32_t height) {
  const void* font = g_api.draw_string ? SmallFont() : nullptr;
  if (!font) {
    return;
  }
  const U16View view{text.data(), text.size()};
  const int32_t rect[4] = {x, y, width, height};
  g_api.draw_string(canvas, &view, font, color, rect);
}

// Centres the glyphs, not the text box: digits have no descenders, so a box centred on the
// font height leaves them visibly high. The cap band (baseline - cap height .. baseline) is
// centred instead, which is where digits are drawn.
void DrawSmallTextCentered(void* canvas, std::u16string_view text, uint32_t color, int32_t x,
                           int32_t y, int32_t width, int32_t height) {
  // gfx::Canvas::TEXT_ALIGN_CENTER
  constexpr int kTextAlignCenter = 1 << 1;
  const void* font = SmallFont();
  if (!font || !g_api.font_height) {
    return;
  }
  const int32_t text_height = g_api.font_height(font);
  int32_t top = y + (height - text_height) / 2;
  if (g_api.font_baseline && g_api.font_cap_height) {
    const int32_t baseline = g_api.font_baseline(font);
    const int32_t cap = g_api.font_cap_height(font);
    if (cap > 0 && baseline >= cap) {
      // 2 * baseline_y = 2 * y + height + cap, rounded to the nearest pixel.
      const int32_t baseline_y = y + (height + cap + 1) / 2;
      top = baseline_y - baseline;
    }
  }
  const U16View view{text.data(), text.size()};
  if (g_api.draw_string_flags) {
    const int32_t rect[4] = {x, top, width, text_height};
    g_api.draw_string_flags(canvas, &view, font, color, rect, kTextAlignCenter);
    return;
  }
  const int32_t text_width = SmallTextWidth(text);
  DrawSmallText(canvas, text, color, x + (width - text_width) / 2, top, text_width + 2,
                text_height);
}

void BeginLayerAlpha(void* canvas, uint8_t alpha) {
  if (LayersAvailable()) {
    g_api.save_layer_alpha(canvas, alpha);
  }
}

void EndLayer(void* canvas) {
  if (LayersAvailable()) {
    g_api.restore(canvas);
  }
}

}  // namespace arbortab
