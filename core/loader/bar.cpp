#include "core/loader/bar.h"

#include <stdexcept>
#include <utility>

namespace zeebulator {

namespace {

constexpr uint32_t kHeaderSize = 32;

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

BarArchive BarArchive::Parse(std::vector<uint8_t> data) {
  BarArchive archive;
  archive.data_ = std::move(data);
  const uint8_t* d = archive.data_.data();
  size_t size = archive.data_.size();

  if (size < kHeaderSize) throw std::runtime_error("BAR: file too small for a header");

  uint32_t table1_start = ReadU32LE(d + 8);
  uint32_t table1_size = ReadU32LE(d + 12);
  uint32_t table_start = ReadU32LE(d + 16);
  uint32_t entry_count = ReadU32LE(d + 20);
  uint32_t data_start = ReadU32LE(d + 24);
  uint32_t data_size = ReadU32LE(d + 28);

  if (table1_start != kHeaderSize || table_start != table1_start + table1_size) {
    throw std::runtime_error("BAR: inconsistent header sub-table offsets");
  }

  // entry_count real offsets, plus one trailing sentinel equal to the
  // file's own total size.
  uint64_t table_bytes = (static_cast<uint64_t>(entry_count) + 1) * 4;
  uint64_t table_end = static_cast<uint64_t>(table_start) + table_bytes;
  if (table_end > size) throw std::runtime_error("BAR: offset table runs past end of file");
  if (table_end != data_start) {
    throw std::runtime_error("BAR: offset table doesn't end where the header says data starts");
  }
  if (static_cast<uint64_t>(data_start) + data_size != size) {
    throw std::runtime_error("BAR: header data-size field doesn't match the real file size");
  }

  std::vector<uint32_t> offsets(entry_count + 1);
  for (uint32_t i = 0; i <= entry_count; ++i) {
    offsets[i] = ReadU32LE(d + table_start + i * 4);
    if (i > 0 && offsets[i] <= offsets[i - 1]) {
      throw std::runtime_error("BAR: offset table is not strictly increasing");
    }
  }
  if (offsets[0] != data_start) {
    throw std::runtime_error("BAR: first resource offset doesn't match the header's data start");
  }
  if (offsets[entry_count] != size) {
    throw std::runtime_error("BAR: final offset-table sentinel doesn't match the file size");
  }

  archive.entries_.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    archive.entries_.push_back(BarEntry{offsets[i], offsets[i + 1] - offsets[i]});
  }
  return archive;
}

std::vector<uint8_t> BarArchive::Extract(const BarEntry& entry) const {
  return std::vector<uint8_t>(data_.begin() + entry.offset,
                               data_.begin() + entry.offset + entry.size);
}

}  // namespace zeebulator
