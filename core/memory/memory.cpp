#include "core/memory/memory.h"

#include <cstring>

namespace zeebulator {

Memory::Page& Memory::MutablePage(uint32_t page_index) {
  auto it = pages_.find(page_index);
  if (it != pages_.end()) return *it->second;
  auto page = std::make_unique<Page>();
  page->fill(0);
  auto [inserted, _] = pages_.emplace(page_index, std::move(page));
  return *inserted->second;
}

const Memory::Page* Memory::FindPage(uint32_t page_index) const {
  auto it = pages_.find(page_index);
  return it == pages_.end() ? nullptr : it->second.get();
}

uint8_t Memory::Read8(uint32_t address) const {
  const Page* page = FindPage(address / kPageSize);
  return page ? (*page)[address & kPageMask] : 0;
}

uint16_t Memory::Read16(uint32_t address) const {
  return static_cast<uint16_t>(Read8(address)) |
         (static_cast<uint16_t>(Read8(address + 1)) << 8);
}

uint32_t Memory::Read32(uint32_t address) const {
  return static_cast<uint32_t>(Read8(address)) |
         (static_cast<uint32_t>(Read8(address + 1)) << 8) |
         (static_cast<uint32_t>(Read8(address + 2)) << 16) |
         (static_cast<uint32_t>(Read8(address + 3)) << 24);
}

void Memory::Write8(uint32_t address, uint8_t value) {
  MutablePage(address / kPageSize)[address & kPageMask] = value;
}

void Memory::Write16(uint32_t address, uint16_t value) {
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void Memory::Write32(uint32_t address, uint32_t value) {
  Write8(address, static_cast<uint8_t>(value));
  Write8(address + 1, static_cast<uint8_t>(value >> 8));
  Write8(address + 2, static_cast<uint8_t>(value >> 16));
  Write8(address + 3, static_cast<uint8_t>(value >> 24));
}

void Memory::Load(uint32_t address, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    Write8(address + static_cast<uint32_t>(i), data[i]);
  }
}

}  // namespace zeebulator
