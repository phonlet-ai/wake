// Container identity shared by every model artifact.
//
// All model files start with the 11-byte magic "phonletwake", a u32 kind, a
// u32 version and a u32 header_len (offset of the weight body), so a loader
// can dispatch on kind without probing deeper:
//
//   kind 1  float acoustic container (any width, dense or low rank)
//   kind 2  quantized acoustic container
//
// Little-endian throughout.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace kws::detail {

inline constexpr char kContainerMagic[12] = "phonletwake";
inline constexpr size_t kContainerMagicSize = 11;
inline constexpr uint32_t kContainerKindFloat = 1;
inline constexpr uint32_t kContainerKindQuantized = 2;
inline constexpr uint32_t kContainerVersion = 1;
// magic + kind + version + header_len + frontend + 6 * u16, padded to 40.
inline constexpr uint32_t kContainerHeaderReserve = 40;

inline uint32_t container_kind(const std::vector<uint8_t> &bytes) {
  if (bytes.size() < kContainerMagicSize + 4 ||
      memcmp(bytes.data(), kContainerMagic, kContainerMagicSize) != 0)
    return 0;
  uint32_t kind;
  memcpy(&kind, bytes.data() + kContainerMagicSize, 4);
  return kind;
}

} // namespace kws::detail
