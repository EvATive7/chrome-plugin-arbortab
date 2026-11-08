// Tiny animation toolkit for the strip decorations: eased tweens on the UI thread and a
// frame request that keeps Chrome relaying out / repainting while any tween runs.
//
// Everything here is UI-thread only (layout and paint hooks).
#pragma once

#include <windows.h>

#include <cstdint>

namespace arbortab {

// Durations in milliseconds.
constexpr uint32_t kMotionMs = 180;   // indent, expand / collapse
constexpr uint32_t kFadeMs = 140;     // chevron crossfade and rotation, drop indicator

// False when Windows "Show animations" is off or the extension disabled animations.
bool AnimationsEnabled();

struct Tween {
  float from = 0.f;
  float to = 0.f;
  ULONGLONG start = 0;
  uint32_t duration = 0;

  float At(ULONGLONG now) const;
  bool Running(ULONGLONG now) const { return duration && now < start + duration; }
  void Snap(float value);
  // Starts a tween from the current value to `target` unless it already heads there.
  void Retarget(float target, ULONGLONG now, uint32_t duration_ms);
};

// Asks for one more animation frame: on the next tick every tab strip is laid out and
// repainted again. Call it from layout / paint while a tween is still running.
void RequestAnimationFrame();

// Asks for `tab_view` (and its favicon) to be repainted, or relaid out, on the next tick.
// Used where the container repaint does not reach (paint caches, child-only state).
void RequestTabRepaint(void* tab_view);
void RequestTabRelayout(void* tab_view);

// Runs `callback` on the next frame tick, outside any layout or paint pass.
void RequestFrameCallback(void (*callback)());

// Runs the queued per-tab work; called by the UI-thread frame timer.
void RunPendingTabWork();

}  // namespace arbortab
