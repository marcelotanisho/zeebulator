#pragma once

#include <cstdint>
#include <vector>

namespace zeebulator {

struct BarEntry {
  uint32_t offset = 0;
  uint32_t size = 0;
};

// Reads a real Zeebo/PopCap ".bar" resource archive (`resources.bar`,
// Phase 2's originally-deferred "BAR file parser" task -- deferred
// because Double Dragon's own dump doesn't have one; Peggle's does).
// No public spec exists; reverse-engineered directly from Peggle's own
// real `resources.bar` (PHASE8_LOG.md has the full derivation). Real
// code opens this file through the generic `ISHELL_LoadResDataEx`
// BREW API rather than any custom `.mod`-embedded parser, so -- unlike
// GGZ or `.pkg` -- there is no real ARM code to trace showing how it's
// walked; every field below was derived and cross-checked purely from
// the raw bytes (self-consistent header arithmetic, and, most
// convincingly, real embedded `RIFF`/`ID3`/PNG file signatures landing
// exactly on every offset this parser computes independently).
//
// Layout, confirmed against the one real sample examined so far:
//
//   offset 0: uint32 LE, meaning unconfirmed
//   offset 4: uint32 LE, meaning unconfirmed
//   offset 8: uint32 LE -- a first sub-table's start offset (real
//             value 32, i.e. immediately after this 32-byte header;
//             not otherwise used by this parser, since that sub-
//             table's own contents are a separate, deferred question --
//             see below)
//   offset 12: uint32 LE -- that sub-table's byte length
//   offset 16: uint32 LE -- (offset 8 + offset 12): the *real* offset
//              table's start (see below) -- confirmed by direct
//              observation, not just arithmetic
//   offset 20: uint32 LE -- the real offset table's entry count
//   offset 24: uint32 LE -- where real resource data begins (equals
//              the real offset table's own first entry; kept only as
//              a redundant cross-check during parsing, not otherwise
//              needed)
//   offset 28: uint32 LE -- total size of the resource-data region
//              (file size minus offset 24; also a redundant check)
//
// At offset 32: a first sub-table (`offset 12` bytes long, real length
// 496 in the one sample seen) whose own internal structure -- a 16-byte
// sub-header followed by 60 real, 8-byte records with a monotonically
// increasing final field that looks ID-like -- was *not* resolved this
// pass (plausibly a string/UI-resource lookup table, distinct from the
// binary asset table below; not needed to extract raw resource bytes
// by index, so deliberately left unparsed rather than guessed at).
//
// At the real offset table (`offset 16`): `offset 20` consecutive
// uint32 LE *absolute file offsets*, strictly increasing, one per real
// resource, immediately followed by one more uint32 LE sentinel value
// equal to the total file size -- confirmed directly against this
// project's own real sample. Entry `i`'s size is `offsets[i+1] -
// offsets[i]` (the sentinel providing the last entry's own bound).
//
// Real resource content, verified by inspection (not decoded further
// by this class -- that's separate, per-format follow-on work, the
// same layering GgzArchive/PkgArchive already use for their own
// contained `.obm1`/`.wav`/zlib-stream payloads): most entries are
// standard files stored with **no container-level compression** --
// raw `RIFF`/WAVE audio, raw `ID3`-tagged MP3, and (for images) a real
// PNG file preceded by a tiny `[uint16 LE header length incl. itself]
// [null-terminated ASCII MIME string, e.g. "image/png"]` wrapper this
// class does not strip. A minority of entries are fixed-size (32768
// bytes in every case but the last of a run) raw blocks with no
// recognizable magic at all (very likely a proprietary uncompressed
// texture/tile format) and a handful of trailing entries are real,
// legible `^`-delimited UI/localization strings -- none of these are
// interpreted further here either.
class BarArchive {
 public:
  // Takes ownership of `data`. Throws std::runtime_error on a malformed
  // archive, including if the header's own redundant cross-check
  // fields (offset table start/size, data start/size) don't agree with
  // each other or with the file's actual size.
  static BarArchive Parse(std::vector<uint8_t> data);

  const std::vector<BarEntry>& Entries() const { return entries_; }

  // Returns entry's raw bytes, verbatim -- no decompression (there is
  // none at this layer) and no MIME-wrapper stripping.
  std::vector<uint8_t> Extract(const BarEntry& entry) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<BarEntry> entries_;
};

}  // namespace zeebulator
