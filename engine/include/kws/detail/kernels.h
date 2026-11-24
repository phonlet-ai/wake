#pragma once
#include <cstdint>
namespace kws::detail {
using Dot = int32_t (*)(const int16_t *, const int16_t *, int);
int32_t dot_portable(const int16_t *, const int16_t *, int);
Dot selected_dot();
const char *selected_kernel();
} // namespace kws::detail
