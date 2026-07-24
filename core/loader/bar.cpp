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

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

constexpr uint32_t kResourceIdSubHeaderSize = 16;
constexpr uint32_t kResourceIdRecordSize = 8;

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

  // The resource-ID directory: a fixed 16-byte sub-header (unconfirmed,
  // not needed) followed by 8-byte records.
  if (table1_size < kResourceIdSubHeaderSize ||
      (table1_size - kResourceIdSubHeaderSize) % kResourceIdRecordSize != 0) {
    throw std::runtime_error("BAR: resource-ID sub-table size doesn't fit whole records");
  }
  uint32_t resource_id_count =
      (table1_size - kResourceIdSubHeaderSize) / kResourceIdRecordSize;
  archive.resource_ids_.reserve(resource_id_count);
  for (uint32_t i = 0; i < resource_id_count; ++i) {
    const uint8_t* rec = d + table1_start + kResourceIdSubHeaderSize + i * kResourceIdRecordSize;
    BarResourceId res_id;
    res_id.type = ReadU16LE(rec + 0);
    res_id.requested_id = ReadU16LE(rec + 2);
    res_id.unknown = ReadU16LE(rec + 4);
    res_id.entry_index = ReadU16LE(rec + 6);
    if (res_id.entry_index >= entry_count) {
      throw std::runtime_error("BAR: resource-ID directory entry_index out of bounds");
    }
    archive.resource_ids_.push_back(res_id);
  }
  return archive;
}

std::vector<uint8_t> BarArchive::Extract(const BarEntry& entry) const {
  return std::vector<uint8_t>(data_.begin() + entry.offset,
                               data_.begin() + entry.offset + entry.size);
}

const BarEntry* BarArchive::Find(uint16_t type, uint16_t id) const {
  for (const BarResourceId& res_id : resource_ids_) {
    if (res_id.type == type && res_id.requested_id == id) {
      return &entries_[res_id.entry_index];
    }
  }
  return nullptr;
}

}  // namespace zeebulator
