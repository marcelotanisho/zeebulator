#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace zeebulator {

// Flat 32-bit address space backed by on-demand-allocated pages, so we
// don't pay for the full 4GB range up front. All accesses are
// little-endian, matching the ARM1136J-S as configured in the Zeebo
// (ARCHITECTURE.md 3.2).
class Memory {
 public:
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint32_t kPageMask = kPageSize - 1;

  uint8_t Read8(uint32_t address) const;
  uint16_t Read16(uint32_t address) const;
  uint32_t Read32(uint32_t address) const;

  void Write8(uint32_t address, uint8_t value);
  void Write16(uint32_t address, uint16_t value);
  void Write32(uint32_t address, uint32_t value);

  // Copies `size` bytes from `data` into the address space starting at
  // `address`. Used by the loader to map code/data segments in.
  void Load(uint32_t address, const uint8_t* data, size_t size);

 private:
  using Page = std::array<uint8_t, kPageSize>;

  Page& MutablePage(uint32_t page_index);
  const Page* FindPage(uint32_t page_index) const;

  std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
};

}  // namespace zeebulator
