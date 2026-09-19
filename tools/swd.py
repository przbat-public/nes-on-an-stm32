#!/usr/bin/env python3
"""swd.py — read the emulator's debug counters over SWD while it runs.

Uses the openocd telnet interface (default 127.0.0.1:4444) to read the
uint32 counters out of .bss. Addresses come from emu.map, so the tool
survives a rebuild that moves them. Contiguous counters go out in one
`mdw`, so a full sample is three round trips (~50 ms).

    python3 tools/swd.py list                  # symbol -> address
    python3 tools/swd.py read                  # one full sample
    python3 tools/swd.py track 300 600 900     # sample at emulated frames
    python3 tools/swd.py budget 5              # per-frame breakdown
    python3 tools/swd.py watch 30              # a sample every 2 s

Counters that are *per frame* (dbg_cyc_cpu/ppu/frame/...) hold the last
frame's value; counters that accumulate since boot (dbg_cyc_flush,
dbg_cyc_band, dbg_bands_sent, ...) are differentiated between samples and
divided by the number of frames in between.

`track` is the comparison mode: it waits until dbg_frames reaches each
given emulated frame number and records the counters there. Two builds
measured at the same frame numbers are looking at the same scene, which
matters because Prince of Persia's attract mode changes scene and the
frame cost with it.
"""
import re
import socket
import sys
import time

import os

HOST, PORT = "127.0.0.1", 4444
MAP = os.environ.get("SWD_MAP", "emu.map")   # e.g. a saved map from another build

# Counters that accumulate since boot and therefore need differencing.
# Everything else in .bss is published at the frame boundary and is already
# a "last complete frame" value (see lcd_dbg_frame/ppu_dbg_frame/nes_run_frame).
CUMULATIVE = {
    "dbg_frames", "dbg_bands", "dbg_irq_count", "dbg_frames_skipped",
}

# what a full sample reads
DEFAULT = [
    "dbg_fps", "dbg_frames", "dbg_bands",
    "dbg_cyc_frame", "dbg_cyc_cpu", "dbg_cyc_ppu", "dbg_cyc_hook",
    "dbg_cyc_loop",
    "dbg_cyc_fill", "dbg_cyc_bg", "dbg_cyc_spr",
    "dbg_cyc_flush", "dbg_cyc_copy", "dbg_cyc_diff", "dbg_cyc_wait",
    "dbg_cyc_conv", "dbg_cyc_setwin", "dbg_cyc_band", "dbg_cyc_frameend",
    "dbg_bands_sent", "dbg_bands_skipped", "dbg_lcd_conv_ok",
    "dbg_lcd_band_ok", "dbg_lcd_band_checked",
]

# the budget table, in the order the frame spends it


def symbols():
    """symbol -> address from the linker map"""
    out = {}
    pat = re.compile(r"^\s+0x([0-9a-f]{8})\s+(\S+)\s*$")
    with open(MAP) as f:
        for line in f:
            m = pat.match(line.rstrip("\n"))
            if m:
                out[m.group(2)] = int(m.group(1), 16)
    return out


class Ocd:
    def __init__(self, host=HOST, port=PORT):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(5)
        self._read_until_prompt()

    @staticmethod
    def _clean(buf):
        out = bytearray()
        i = 0
        while i < len(buf):
            if buf[i] == 0xFF:              # telnet IAC
                i += 2
                if i <= len(buf) and buf[i - 1] in (0xFB, 0xFC, 0xFD, 0xFE):
                    i += 1
                continue
            out.append(buf[i])
            i += 1
        return bytes(out).decode("ascii", "replace")

    def _read_until_prompt(self):
        buf = b""
        while not buf.endswith(b"> "):
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                break
            if not b:
                break
            buf += b
        return self._clean(buf)

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        return self._read_until_prompt()

    def read_block(self, addr, n):
        vals = []
        while len(vals) < n:
            k = min(n - len(vals), 64)
            txt = self.cmd("mdw 0x%08x %d" % (addr + 4 * len(vals), k))
            got = 0
            for line in txt.splitlines():
                line = line.lstrip("\x00 \t\r")
                m = re.match(r"^0x([0-9a-fA-F]{8}):\s*(.*)$", line)
                if not m:
                    continue
                for w in m.group(2).split():
                    if re.fullmatch(r"[0-9a-fA-F]{8}", w):
                        vals.append(int(w, 16))
                        got += 1
            if got == 0:
                raise SystemExit("mdw returned nothing for 0x%08x" % addr)
        return vals[:n]


class Reader:
    """batches the named counters into as few mdw round trips as possible"""

    def __init__(self, ocd, syms, names):
        self.ocd = ocd
        self.names = [n for n in names if n in syms]
        missing = [n for n in names if n not in syms]
        if missing:
            print("(not in map: %s)" % " ".join(missing), file=sys.stderr)
        pairs = sorted((syms[n], n) for n in self.names)
        self.runs = []                      # (base, span_words, [(word, name)])
        cur = None
        for a, n in pairs:
            if cur and (a - cur[0]) // 4 <= cur[1] + 1:
                cur[1] = (a - cur[0]) // 4
                cur[2].append(((a - cur[0]) // 4, n))
            else:
                cur = [a, 0, [(0, n)]]
                self.runs.append(cur)

    def read(self):
        out = {}
        for base, span, items in self.runs:
            vals = self.ocd.read_block(base, span + 1)
            for w, nm in items:
                out[nm] = vals[w]
        return out


# Builds before the per-frame publishing (no dbg_cyc_hook in the map) kept
# the band counters as "since boot" totals, so they have to be differenced.
OLD_CUMULATIVE = {
    "dbg_cyc_flush", "dbg_cyc_band", "dbg_cyc_wait", "dbg_bands_sent",
    "dbg_bands_skipped", "dbg_frames", "dbg_bands", "dbg_irq_count",
    "dbg_cyc_diff", "dbg_cyc_conv", "dbg_cyc_setwin", "dbg_cyc_copy",
    "dbg_cyc_frameend",
}
CUM = set(CUMULATIVE)


def per_frame(name, a, b, nframes):
    if name in CUM:
        return ((b[name] - a[name]) & 0xFFFFFFFF) // max(nframes, 1)
    return b[name]


def stats(rd, k, secs):
    """k samples, secs apart: mean/min/max of the per-frame counters"""
    names = [n for n in rd.names if n not in CUMULATIVE
             and n not in ("dbg_fps", "dbg_lcd_conv_ok")]
    acc = {n: [] for n in names}
    fps = []
    for i in range(k):
        c = rd.read()
        for n in names:
            acc[n].append(c[n])
        fps.append(c["dbg_fps"])
        print("  sample %2d: frame=%7d (%.2f ms) fps=%2d cpu=%7d ppu=%7d "
              "sent=%2d skip=%2d"
              % (i + 1, c["dbg_cyc_frame"], c["dbg_cyc_frame"] / 80000.0,
                 c["dbg_fps"], c["dbg_cyc_cpu"], c["dbg_cyc_ppu"],
                 c["dbg_bands_sent"], c["dbg_bands_skipped"]))
        if i + 1 < k:
            time.sleep(secs)
    print()
    print("%-18s %10s %10s %10s %8s" % ("counter", "mean", "min", "max", "ms(mean)"))
    for n in names:
        v = acc[n]
        print("%-18s %10d %10d %10d %8.2f"
              % (n, sum(v) // len(v), min(v), max(v), sum(v) / len(v) / 80000.0))
    print("%-18s %10.1f %10d %10d" % ("dbg_fps", sum(fps) / len(fps), min(fps),
                                      max(fps)))


def show_budget(rd, secs):
    a = rd.read()
    t0 = time.time()
    time.sleep(secs)
    b = rd.read()
    dt = time.time() - t0
    n = (b["dbg_frames"] - a["dbg_frames"]) & 0xFFFFFFFF
    if n == 0:
        print("no frames advanced in %.1f s" % dt)
        return
    tot = b["dbg_cyc_frame"]
    print("frames: %u in %.2f s   wall-clock fps: %.2f   dbg_fps: %u"
          % (n, dt, n / dt, b["dbg_fps"]))
    def pf(nm):
        return per_frame(nm, a, b, n) if nm in b else 0
    print("bands/frame: sent %d  skipped %d"
          % (pf("dbg_bands_sent"), pf("dbg_bands_skipped")))
    print()
    print("%-20s %12s %9s %8s" % ("counter", "per frame", "ms", "% frame"))
    # builds before the per-frame publishing have one big "hook" counter
    # (dbg_cyc_flush) and no breakdown inside it
    old = "dbg_cyc_hook" not in b
    groups = [
        ("dbg_cyc_cpu", []),
        ("dbg_cyc_ppu", ["dbg_cyc_fill", "dbg_cyc_bg", "dbg_cyc_spr"]),
        ("dbg_cyc_flush" if old else "dbg_cyc_hook",
         ["dbg_cyc_copy", "dbg_cyc_diff", "dbg_cyc_wait",
          "dbg_cyc_conv", "dbg_cyc_setwin"]),
        ("dbg_cyc_loop", ["dbg_irq_count"]),
    ]
    acc = 0
    for parent, kids in groups:
        if parent not in b:
            continue
        v = pf(parent)
        acc += v
        print("%-20s %12d %9.2f %7.1f%%"
              % (parent, v, v / 80000.0, 100.0 * v / max(tot, 1)))
        for k in kids:
            if k not in b or k == "dbg_irq_count":
                continue
            kv = pf(k)
            print("    %-16s %12d %9.2f %7.1f%%"
                  % (k, kv, kv / 80000.0, 100.0 * kv / max(tot, 1)))
    print("%-20s %12d %9.2f %7.1f%%" % ("dbg_cyc_frame", tot, tot / 80000.0, 100.0))
    print("%-20s %12d %9.2f %7.1f%%" % ("UNACCOUNTED", tot - acc,
                                        (tot - acc) / 80000.0,
                                        100.0 * (tot - acc) / max(tot, 1)))
    print()
    print("dbg_cyc_frameend (after the frame loop): %d (%.2f ms)"
          % (pf("dbg_cyc_frameend"), pf("dbg_cyc_frameend") / 80000.0))
    print("dbg_lcd_conv_ok = %u  dbg_lcd_band_ok = %u (%u bands checked)  "
          "dbg_bands=%u"
          % (b["dbg_lcd_conv_ok"], b.get("dbg_lcd_band_ok", -1),
             b.get("dbg_lcd_band_checked", 0), pf("dbg_bands")))


def track(rd, targets):
    """record the counters at given emulated frame numbers"""
    have_hook = "dbg_cyc_hook" in rd.names
    print("%9s %10s %9s %9s %9s %8s %6s" %
          ("frame", "cyc_frame", "cpu", "ppu", "hook", "sent/f", "fps"))
    prev = rd.read()
    tgt = list(targets)
    while tgt:
        cur = rd.read()
        f = cur["dbg_frames"]
        if (f - prev["dbg_frames"]) & 0xFFFFFFFF and f >= tgt[0]:
            n = (f - prev["dbg_frames"]) & 0xFFFFFFFF
            if "dbg_bands_sent" in CUM:
                sent = ((cur["dbg_bands_sent"] - prev["dbg_bands_sent"]) & 0xFFFFFFFF) / n
            else:
                sent = cur["dbg_bands_sent"]
            flush = ((cur["dbg_cyc_flush"] - prev["dbg_cyc_flush"]) & 0xFFFFFFFF) // n
            print("%9d %10d %9d %9d %9d %8.1f %6d" %
                  (f, cur["dbg_cyc_frame"], cur["dbg_cyc_cpu"], cur["dbg_cyc_ppu"],
                   cur.get("dbg_cyc_hook", flush), sent, cur["dbg_fps"]))
            tgt.pop(0)
        prev = cur
        time.sleep(0.01)


def main():
    syms = symbols()
    global CUM
    if "dbg_cyc_hook" not in syms:      # an older build: see OLD_CUMULATIVE
        CUM = CUM | OLD_CUMULATIVE
    args = sys.argv[1:]
    if not args:
        args = ["read"]
    mode = args[0]
    ocd = Ocd()
    if mode == "list":
        for nm in sorted(syms):
            if nm.startswith(("dbg_", "cpu", "lcd_dbg", "mmc3")):
                print("0x%08x  %s" % (syms[nm], nm))
        return
    rd = Reader(ocd, syms, DEFAULT)
    if mode == "read":
        for nm, v in rd.read().items():
            print("%-20s %10u" % (nm, v))
        return
    if mode == "watch":
        k = int(args[1]) if len(args) > 1 else 10
        prev = rd.read()
        for i in range(k):
            time.sleep(2.0)
            cur = rd.read()
            n = (cur["dbg_frames"] - prev["dbg_frames"]) & 0xFFFFFFFF
            print("t=%4.1fs frames=%8d fps=%2d frame=%7d(%.2fms) cpu=%7d ppu=%7d "
                  "sent/f=%4.1f skip/f=%4.1f"
                  % (2.0 * (i + 1), cur["dbg_frames"], cur["dbg_fps"],
                     cur["dbg_cyc_frame"], cur["dbg_cyc_frame"] / 80000.0,
                     cur["dbg_cyc_cpu"], cur["dbg_cyc_ppu"],
                     ((cur["dbg_bands_sent"] - prev["dbg_bands_sent"]) & 0xFFFFFFFF) / max(n, 1),
                     ((cur["dbg_bands_skipped"] - prev["dbg_bands_skipped"]) & 0xFFFFFFFF) / max(n, 1)))
            prev = cur
        return
    if mode == "track":
        track(rd, [int(x) for x in args[1:]] or [300, 600, 900, 1200])
        return
    if mode == "stats":
        stats(rd, int(args[1]) if len(args) > 1 else 10,
              float(args[2]) if len(args) > 2 else 1.0)
        return
    if mode == "budget":
        show_budget(rd, float(args[1]) if len(args) > 1 else 2.0)
        return
    raise SystemExit("unknown mode %s" % mode)


if __name__ == "__main__":
    main()
