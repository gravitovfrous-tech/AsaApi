#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# SpawnGuard helper: print names from the AsaApi offset cache.
#
# ArkApi keeps the hook names of the running game build in
#   <server>/ArkApi/Cache/.../cached_offsets.cache
# as a binary serialized unordered_map<string, intptr_t>:
#
#   repeat: [uint64 key_size][key bytes][int64 value]
#
# When ArkApi.log says
#   [critical] Failed to get the offset of 'APrimalStructure.IsAllowedToBuild(...)'
# this script shows what the cache actually contains for that method (the
# parameter list depends on the game build).
#
# Usage:
#   python dump_offset_name.py
#   python dump_offset_name.py IsAllowedToBuild
#   python dump_offset_name.py "APrimalStructure." --all
#   python dump_offset_name.py --cache-dir "D:\ARK\ShooterGame\Binaries\Win64\ArkApi"
# ---------------------------------------------------------------------------

import argparse
import os
import struct
import sys


def iter_entries(path):
    with open(path, "rb") as handle:
        data = handle.read()

    offset, size = 0, len(data)
    while offset + 8 <= size:
        (key_size,) = struct.unpack_from("<Q", data, offset)
        offset += 8
        if key_size == 0 or key_size > 1 << 20 or offset + key_size + 8 > size:
            break
        key = data[offset:offset + key_size].decode("utf-8", "replace")
        offset += key_size
        (value,) = struct.unpack_from("<q", data, offset)
        offset += 8
        yield key, value


def find_cache_files(start):
    root = os.path.abspath(start)
    while True:
        for candidate in (os.path.join(root, "ArkApi", "Cache"), os.path.join(root, "ArkApi")):
            if os.path.isdir(candidate):
                found = []
                for dirpath, _dirnames, filenames in os.walk(candidate):
                    if "cached_offsets.cache" in filenames:
                        found.append(os.path.join(dirpath, "cached_offsets.cache"))
                if found:
                    return sorted(found, key=os.path.getmtime, reverse=True)
        parent = os.path.dirname(root)
        if parent == root:
            return []
        root = parent


def main():
    parser = argparse.ArgumentParser(description="Print names from the AsaApi offset cache")
    parser.add_argument("pattern", nargs="?", default="IsAllowedToBuild",
                        help="substring to search for (default: IsAllowedToBuild)")
    parser.add_argument("--all", action="store_true", help="print every name in the cache")
    parser.add_argument("--cache-dir", default="", help="folder that contains ArkApi (or ArkApi itself)")
    args = parser.parse_args()

    files = find_cache_files(args.cache_dir or os.getcwd())
    if not files:
        print("error: cached_offsets.cache not found. Pass --cache-dir <...\\ArkApi>", file=sys.stderr)
        return 1

    needle = "" if args.all else args.pattern.lower()

    for path in files:
        print("=== %s" % path)
        total = matched = 0
        for key, value in iter_entries(path):
            total += 1
            if args.all or needle in key.lower():
                print(key)
                matched += 1
        print("entries: %d, matched: %d" % (total, matched))
    return 0


if __name__ == "__main__":
    sys.exit(main())
