"""Analyse a WiiCompiled tvOS sampling profile.

usage: python3 analyze-profile.py PROFILE_BASE MACHO [--thread NAME] [--top N]
  PROFILE_BASE  path without extension (profile-<pid>; .bin and .txt beside it)
  MACHO         the app's main executable (Payload/WiiCompiled.app/WiiCompiled)
"""
import bisect
import collections
import os
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

N_STAB, N_TYPE, N_SECT = 0xE0, 0x0E, 0x0E


def load_macho_symbols(path):
    data = open(path, "rb").read()
    magic, cputype, _, _, ncmds, _, _ = struct.unpack_from("<IiiIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("not a thin 64-bit Mach-O: %s" % path)
    off = 32
    text_vmaddr = text_vmsize = None
    sections = []
    symtab = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off + 8:off + 24].rstrip(b"\0").decode()
            vmaddr, vmsize = struct.unpack_from("<QQ", data, off + 24)
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            if segname == "__TEXT":
                text_vmaddr, text_vmsize = vmaddr, vmsize
            for i in range(nsects):
                s = off + 72 + i * 80
                sect = data[s:s + 16].rstrip(b"\0").decode()
                addr, size = struct.unpack_from("<QQ", data, s + 32)
                sections.append((addr, addr + size, segname + "," + sect))
        elif cmd == 0x2:  # LC_SYMTAB
            symtab = struct.unpack_from("<IIII", data, off + 8)
        off += cmdsize
    syms = []
    if symtab:
        symoff, nsyms, stroff, _ = symtab
        for i in range(nsyms):
            strx, ntype, nsect, _, value = struct.unpack_from("<IBBHQ", data, symoff + i * 16)
            if ntype & N_STAB or (ntype & N_TYPE) != N_SECT or value == 0:
                continue
            end = data.index(b"\0", stroff + strx)
            syms.append((value, data[stroff + strx:end].decode("utf-8", "replace")))
    syms.sort()
    addrs = [a for a, _ in syms]
    return text_vmaddr, text_vmsize, addrs, syms


def load_summary(path):
    threads, images = {}, []
    header = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "wall_us":
            header = dict(zip(parts[0::2], parts[1::2]))
        elif parts[0] == "thread":
            slot = int(parts[1])
            threads[slot] = {
                "id": parts[2], "cpu_us": int(parts[3]), "cpu_pct": float(parts[4]),
                "samples": int(parts[5]), "base": parts[6], "cur": parts[7],
                "name": " ".join(parts[8:]),
            }
        elif parts[0] == "image":
            images.append((int(parts[1], 16), int(parts[2], 16), " ".join(parts[3:])))
    return header, threads, images


def read_samples(path):
    data = open(path, "rb").read()
    if data[:8] != b"MKWPROF1":
        raise SystemExit("bad profile magic")
    off = 8
    out = []
    while off + 8 <= len(data):
        tick, word = struct.unpack_from("<II", data, off)
        off += 8
        slot, depth = word & 0xFFFF, word >> 16
        if off + depth * 8 > len(data):
            break
        frames = struct.unpack_from("<%dQ" % depth, data, off)
        off += depth * 8
        out.append((tick, slot, frames))
    return out


class Symbolizer:
    def __init__(self, macho, images):
        self.text_vmaddr, self.text_vmsize, self.addrs, self.syms = load_macho_symbols(macho)
        main = [im for im in images if im[2].endswith("/WiiCompiled")]
        self.main_header = main[0][0] if main else images[0][0]
        others = sorted((h, os.path.basename(n)) for h, _, n in images if h != self.main_header)
        self.other_starts = [h for h, _ in others]
        self.others = others
        self.cache = {}

    def __call__(self, pc):
        hit = self.cache.get(pc)
        if hit is not None:
            return hit
        rel = pc - self.main_header
        if 0 <= rel < self.text_vmsize:
            vm = self.text_vmaddr + rel
            i = bisect.bisect_right(self.addrs, vm) - 1
            name = self.syms[i][1] if i >= 0 else "?main+0x%x" % rel
        else:
            i = bisect.bisect_right(self.other_starts, pc) - 1
            name = "[%s]" % (self.others[i][1] if i >= 0 else "?")
        self.cache[pc] = name
        return name


def short(name, width=110):
    if name.startswith("_"):
        name = name[1:]
    return name if len(name) <= width else name[:width - 3] + "..."


def main():
    args = sys.argv[1:]
    top = 40
    only = None
    if "--top" in args:
        i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
    if "--thread" in args:
        i = args.index("--thread"); only = args[i + 1]; del args[i:i + 2]
    base, macho = args
    header, threads, images = load_summary(base + ".txt")
    sym = Symbolizer(macho, images)
    samples = read_samples(base + ".bin")
    wall = int(header.get("wall_us", "0"))
    print("wall %.1f s, %s ticks, %d records" % (wall / 1e6, header.get("ticks"), len(samples)))
    print("\n== threads by CPU ==")
    for slot, t in sorted(threads.items(), key=lambda kv: -kv[1]["cpu_us"]):
        if t["cpu_us"] < wall * 0.005:
            continue
        print("  slot %3d %6.1f%% cpu  %7d samples  prio %s/%s  %s" % (
            slot, t["cpu_pct"], t["samples"], t["base"], t["cur"], t["name"]))

    by_slot = collections.defaultdict(list)
    for tick, slot, frames in samples:
        by_slot[slot].append(frames)
    for slot, t in sorted(threads.items(), key=lambda kv: -kv[1]["cpu_us"]):
        if t["cpu_us"] < wall * 0.03:
            continue
        if only and only not in t["name"] and only != str(slot):
            continue
        rows = by_slot.get(slot, [])
        if not rows:
            continue
        self_counts = collections.Counter()
        incl_counts = collections.Counter()
        image_counts = collections.Counter()
        for frames in rows:
            names = [sym(frames[0])]
            if len(frames) > 1:
                lr = sym(frames[1])
                nxt = sym(frames[2]) if len(frames) > 2 else None
                if lr != names[0] and lr != nxt:
                    names.append(lr)
            names.extend(sym(pc) for pc in frames[2:])
            self_counts[names[0]] += 1
            image_counts[names[0] if names[0].startswith("[") else "WiiCompiled"] += 1
            for n in set(names):
                incl_counts[n] += 1
        total = len(rows)
        print("\n== slot %d %s: %d samples (%.1f%% cpu) ==" % (slot, t["name"], total, t["cpu_pct"]))
        print("  -- by image --")
        for n, c in image_counts.most_common(12):
            print("  %5.1f%%  %s" % (100.0 * c / total, n))
        print("  -- self --")
        for n, c in self_counts.most_common(top):
            print("  %5.1f%%  %s" % (100.0 * c / total, short(n)))
        print("  -- inclusive --")
        for n, c in incl_counts.most_common(top):
            print("  %5.1f%%  %s" % (100.0 * c / total, short(n)))


if __name__ == "__main__":
    main()
