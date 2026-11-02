#!/usr/bin/env python3
"""Generate engine/core/include/kfm/core/sin_table.inc.

The simulation uses a lookup table for trig (architecture §5.1) so that every
toolchain agrees bit-for-bit. The table is generated once, here, and checked
in; it is never computed at build time, because compile-time floating point
is not guaranteed to round identically across compilers.

  Entries : kSinTableSize samples of sin(2*pi*i / kSinTableSize)
  Format  : Q16.16 (Fixed::raw), rounded to nearest, ties away from zero

Usage: python3 tools/codegen/gen_sin_table.py > engine/core/include/kfm/core/sin_table.inc
"""
import math
import sys

TABLE_SIZE = 4096
ONE = 1 << 16


def q16(x: float) -> int:
    v = x * ONE
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


def main() -> None:
    out = sys.stdout
    out.write("// GENERATED FILE. Do not edit by hand.\n")
    out.write("// Regenerate with: python3 tools/codegen/gen_sin_table.py\n")
    out.write(f"// {TABLE_SIZE} samples of sin(2*pi*i/{TABLE_SIZE}) in Q16.16.\n")
    out.write("// clang-format off\n")
    out.write(f"inline constexpr int kSinTableSize = {TABLE_SIZE};\n")
    out.write(f"inline constexpr std::array<int32_t, kSinTableSize> kSinTable = {{{{\n")
    for row in range(0, TABLE_SIZE, 8):
        vals = [q16(math.sin(2.0 * math.pi * i / TABLE_SIZE)) for i in range(row, row + 8)]
        out.write("  " + ", ".join(f"{v:7d}" for v in vals) + ",\n")
    out.write("}};\n")
    out.write("// clang-format on\n")


if __name__ == "__main__":
    main()
