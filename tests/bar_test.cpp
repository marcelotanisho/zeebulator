// Tests use small, synthetic, hand-built ".bar" byte arrays -- the real
// format was reverse-engineered against Peggle's real `resources.bar`
// (TASKS.md Phase 2/8), but real game assets are never committed to
// this repo (see CONTRIBUTING.md's clean-room policy).

#include "core/loader/bar.h"

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>

using zeebulator::BarArchive;
using zeebulator::BarEntry;

namespace {

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

// Builds a well-formed synthetic BAR archive: real 32-byte header, a
// (here empty-content, real-length) first sub-table, the real offset
// table (entry_count offsets + one file-size sentinel), then the
// resources themselves back to back -- matching every structural
// element confirmed against the real file (core/loader/bar.h has the
// full layout writeup).
std::vector<uint8_t> BuildBar(const std::vector<std::vector<uint8_t>>& resources,
                               uint32_t table1_size = 16) {
  constexpr uint32_t kHeaderSize = 32;
  uint32_t table1_start = kHeaderSize;
  uint32_t table_start = table1_start + table1_size;
  uint32_t entry_count = static_cast<uint32_t>(resources.size());
  uint32_t data_start = table_start + (entry_count + 1) * 4;

  uint32_t cursor = data_start;
  std::vector<uint32_t> offsets;
  for (const auto& r : resources) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(r.size());
  }
  offsets.push_back(cursor);  // sentinel = final file size
  uint32_t data_size = cursor - data_start;

  std::vector<uint8_t> out;
  AppendU32LE(out, 0x00010011);  // offset 0, unconfirmed
  AppendU32LE(out, 0x003e0001);  // offset 4, unconfirmed
  AppendU32LE(out, table1_start);
  AppendU32LE(out, table1_size);
  AppendU32LE(out, table_start);
  AppendU32LE(out, entry_count);
  AppendU32LE(out, data_start);
  AppendU32LE(out, data_size);
  out.resize(table1_start + table1_size, 0);  // the real, unparsed first sub-table

  for (uint32_t off : offsets) AppendU32LE(out, off);
  for (const auto& r : resources) out.insert(out.end(), r.begin(), r.end());
  return out;
}

}  // namespace

TEST(Bar, ParsesTwoEntriesAndExtractsCorrectData) {
  std::vector<uint8_t> res0 = {'R', 'I', 'F', 'F', 1, 2, 3, 4};
  std::vector<uint8_t> res1(100, 0xAB);
  auto blob = BuildBar({res0, res1});
  auto archive = BarArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 2u);
  EXPECT_EQ(archive.Extract(archive.Entries()[0]), res0);
  EXPECT_EQ(archive.Extract(archive.Entries()[1]), res1);
}

TEST(Bar, EntrySizeIsDerivedFromConsecutiveOffsets) {
  auto blob = BuildBar({std::vector<uint8_t>(10, 1), std::vector<uint8_t>(20, 2),
                         std::vector<uint8_t>(5, 3)});
  auto archive = BarArchive::Parse(blob);

  ASSERT_EQ(archive.Entries().size(), 3u);
  EXPECT_EQ(archive.Entries()[0].size, 10u);
  EXPECT_EQ(archive.Entries()[1].size, 20u);
  EXPECT_EQ(archive.Entries()[2].size, 5u);
}

TEST(Bar, TooSmallFileIsRejected) {
  std::vector<uint8_t> tiny(16, 0);
  EXPECT_THROW(BarArchive::Parse(tiny), std::runtime_error);
}

TEST(Bar, WrongTable1StartOffsetIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[8] = 99;  // header offset 8 no longer says "32"
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, TableRunningPastEndOfFileIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[20] = 100;  // claim 100 entries; the real table only has room for 1
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, MismatchedDataSizeHeaderFieldIsRejected) {
  auto blob = BuildBar({{1, 2, 3}});
  blob[28] = 0xFF;  // corrupt the declared data-region size
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}

TEST(Bar, NonIncreasingOffsetTableIsRejected) {
  auto blob = BuildBar({{1, 2, 3, 4}, {5, 6, 7, 8}});
  // Overwrite the second offset-table entry with a copy of the first,
  // so it no longer strictly increases (table starts right after the
  // 32-byte header + 16-byte table1).
  size_t table_start = 32 + 16;
  std::memcpy(blob.data() + table_start + 4, blob.data() + table_start, 4);
  EXPECT_THROW(BarArchive::Parse(blob), std::runtime_error);
}
