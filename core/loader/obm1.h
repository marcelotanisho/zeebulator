#pragma once

#include <cstdint>
#include <vector>

namespace zeebulator {

// A decoded, palette-resolved image: width*height RGB888 triples,
// row-major, top-to-bottom, left-to-right.
struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;  // width * height * 3 bytes
};

// Decodes a real Zeebo/BREW ".obm1" sprite/texture asset. Reverse-
// engineered from Double Dragon's real `data.ggz` (TASKS.md Phase 8):
// all 89 real assets in that archive share this exact layout, cross-
// checked byte-for-byte against each file's real size --
//
//   offset 0: magic "OI" (2 bytes)
//   offset 2: 0x04 in every real sample seen so far -- meaning
//             unconfirmed (possibly a format version); validated as a
//             constant, not assumed to vary
//   offset 3: bits per pixel -- only 4 and 8 confirmed (16- and
//             256-entry palettes respectively); every real asset in
//             this game's archive uses one or the other
//   offset 4: width, uint16 little-endian
//   offset 6: height, uint16 little-endian
//   offset 8: palette, (1 << bpp) entries, 2 bytes each, RGB565
//             little-endian (confirmed by decoding real assets to
//             images and visually verifying recognizable, correctly-
//             colored content -- a readable ASCII font sheet among
//             them, the strongest possible confirmation of both the
//             pixel unpacking order and the color channel layout)
//   after palette: packed palette-index pixel data, `bpp` bits per
//             pixel, most-significant-bits-first within each byte,
//             row-major
//
// Many real sprite assets use a distinctly magenta (near-0xFF00FF)
// palette entry as an apparent background/transparency color-key, but
// the exact convention (which index, exact color-key value, or
// whether it's even a fixed convention vs. per-asset) isn't confirmed
// -- this decoder returns raw RGB888 with no alpha/transparency
// handling; that's a separate, not-yet-investigated concern.
class Obm1Image {
 public:
  // Throws std::runtime_error on a malformed image (bad magic, an
  // unsupported bpp, a file too short for its own declared
  // width/height/bpp).
  static DecodedImage Decode(const std::vector<uint8_t>& data);
};

}  // namespace zeebulator
