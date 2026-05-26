#!/usr/bin/env python3
import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert binary file to C uint8_t array header")
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--var", default="kBundleBlob", help="array variable name")
    args = parser.parse_args()

    data = args.input.read_bytes()
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append(f"static const uint8_t {args.var}[] = {{")

    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hexes},")

    lines.append("};")
    lines.append(f"static const size_t {args.var}Size = sizeof({args.var});")
    lines.append("")

    args.output.write_text("\n".join(lines))
    print(f"wrote {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
