#pragma once
#include <cstdint>

namespace kws::detail {

// One acoustic frame: 80 stacked int16 mel values in, Q31 posteriors out.
// Session owns one per stream and resets it on the engine's ResetClock.
class Acoustic {
public:
  virtual ~Acoustic() = default;
  virtual void reset() = 0;
  virtual void step(const int16_t *mel80, int32_t *post) = 0;
};

} // namespace kws::detail
