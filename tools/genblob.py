#!/usr/bin/env python3
"""Bakes std/*.qela into srcql/std_blob.qela so the compiler carries the
standard library inside its own binary.

The sources are LZSS-compressed here and unpacked by srcql/blob.qela at
runtime: only the decoder ships, the compressor stays in this script. The
format is one flag byte per eight items, LSB first; a set bit is a literal
byte, a clear bit is a two-byte back reference, big-endian, holding a
12-bit distance (minus one) and a 4-bit length (minus three).

Modules are emitted in sorted order: the blob feeds compiler output, so its
layout has to be reproducible (see docs/BOOTSTRAP.md, "Determinism").
"""
import os
import sys

SRC_DIR = "std"
OUT = "srcql/std_blob.qela"

WINDOW = 4096
MIN_MATCH = 3
MAX_MATCH = 18
CHUNK = 3072


def compress(data: bytes) -> bytes:
    out = bytearray()
    items = bytearray()
    flags = 0
    nflags = 0
    heads: dict[bytes, list[int]] = {}
    i = 0
    n = len(data)
    while i < n:
        best_len = 0
        best_dist = 0
        if i + MIN_MATCH <= n:
            key = data[i:i + MIN_MATCH]
            for pos in reversed(heads.get(key, [])):
                dist = i - pos
                if dist > WINDOW:
                    break
                ln = MIN_MATCH
                limit = min(MAX_MATCH, n - i)
                while ln < limit and data[pos + ln] == data[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len = ln
                    best_dist = dist
                    if ln == MAX_MATCH:
                        break
        if best_len >= MIN_MATCH:
            code = ((best_dist - 1) << 4) | (best_len - MIN_MATCH)
            items.append((code >> 8) & 0xFF)
            items.append(code & 0xFF)
            step = best_len
        else:
            flags |= 1 << nflags
            items.append(data[i])
            step = 1
        nflags += 1
        if nflags == 8:
            out.append(flags)
            out += items
            items = bytearray()
            flags = 0
            nflags = 0
        for k in range(step):
            p = i + k
            if p + MIN_MATCH <= n:
                heads.setdefault(data[p:p + MIN_MATCH], []).append(p)
        i += step
    if nflags:
        out.append(flags)
        out += items
    return bytes(out)


def decompress(src: bytes, out_len: int) -> bytes:
    out = bytearray()
    i = 0
    while len(out) < out_len:
        flags = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= out_len:
                break
            if flags & (1 << bit):
                out.append(src[i])
                i += 1
            else:
                code = (src[i] << 8) | src[i + 1]
                i += 2
                dist = (code >> 4) + 1
                ln = (code & 15) + MIN_MATCH
                start = len(out) - dist
                for k in range(ln):
                    out.append(out[start + k])
    return bytes(out)


def chunks(data: bytes) -> list[bytes]:
    """Splits the byte stream so no chunk ends up holding a '$' directly
    followed by '{': that pair starts an interpolation inside the string
    literal the chunk becomes, and no escape for '$' exists."""
    out = []
    start = 0
    i = 0
    while i < len(data):
        cut = 0
        if i - start >= CHUNK:
            cut = 1
        elif data[i] == 0x24 and i + 1 < len(data) and data[i + 1] == 0x7B:
            cut = 2
        if cut:
            end = i + 1 if cut == 2 else i
            if end > start:
                out.append(data[start:end])
                start = end
        i += 1
    if start < len(data):
        out.append(data[start:])
    return out


def escape(chunk: bytes) -> str:
    out = []
    for b in chunk:
        if b == 0x5C:
            out.append("\\\\")
        elif b == 0x22:
            out.append('\\"')
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x0D:
            out.append("\\r")
        elif b == 0x00:
            out.append("\\0")
        else:
            out.append(chr(b))
    return "".join(out)


def main() -> int:
    names = sorted(n for n in os.listdir(SRC_DIR) if n.endswith(".qela"))
    if not names:
        print("no modules found in std/", file=sys.stderr)
        return 1

    bodies = []
    for name in names:
        with open(os.path.join(SRC_DIR, name), "rb") as f:
            body = f.read()
        if b"${" in body:
            print(
                f"error: std/{name} contains '$' + '{{', which the compiler's "
                "lexer would read as the start of a string interpolation "
                "inside the blob literal; rephrase the source",
                file=sys.stderr,
            )
            return 1
        bodies.append(body)

    joined = b"".join(bodies)
    packed = compress(joined)
    if decompress(packed, len(joined)) != joined:
        print("error: the packed blob does not round-trip", file=sys.stderr)
        return 1

    parts = chunks(packed)
    if b"".join(parts) != packed:
        print("error: chunking lost bytes", file=sys.stderr)
        return 1

    lines = [
        "// Generated by tools/genblob.py. Do not edit.",
        "// The standard library, LZSS-packed; srcql/blob.qela unpacks it.",
        "",
        f"fn std_blob_count() int {{ return {len(names)}; }}",
        "",
        f"fn std_blob_total() i64 {{ return {len(joined)}; }}",
        "",
        f"fn std_blob_packed_len() i64 {{ return {len(packed)}; }}",
        "",
        f"fn std_blob_nchunks() int {{ return {len(parts)}; }}",
        "",
        "fn std_blob_name(i int) str {",
    ]
    for idx, name in enumerate(names):
        lines.append(f'\tif (i == {idx}) {{ return "std/{name}"; }}')
    lines += ['\treturn "";', "}", "", "fn std_blob_off(i int) i64 {"]
    off = 0
    for idx, body in enumerate(bodies):
        lines.append(f"\tif (i == {idx}) {{ return {off}; }}")
        off += len(body)
    lines += ["\treturn -1;", "}", "", "fn std_blob_len(i int) i64 {"]
    for idx, body in enumerate(bodies):
        lines.append(f"\tif (i == {idx}) {{ return {len(body)}; }}")
    lines += ["\treturn 0;", "}", "", "fn std_blob_chunk(i int) str {"]
    for idx, part in enumerate(parts):
        lines.append(f'\tif (i == {idx}) {{ return "{escape(part)}"; }}')
    lines += ['\treturn "";', "}", ""]

    with open(OUT, "w", encoding="latin-1", newline="\n") as f:
        f.write("\n".join(lines))

    ratio = len(packed) * 100.0 / len(joined)
    print(
        f"{OUT}: {len(names)} modules, {len(joined)} bytes of source, "
        f"{len(packed)} packed ({ratio:.1f}%), {len(parts)} chunks"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
