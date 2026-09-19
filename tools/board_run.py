#!/usr/bin/env python3
"""board_run.py — drive the board with the same pad script the host runs,
and dump the framebuffer at given emulated frames.

    python3 board_run.py TARGET [TARGET...] [--out PREFIX] [--auto 60]

The pad script is the firmware's dbg_pad_auto mode: START held for frames
20..22 of every `auto` frames, which is exactly the script the host
runners get. The phase comes from dbg_frames, so the board's frame N is
driven the same way as the host's frame N whatever moment the register is
armed at (it only has to be armed before frame 20).

The board is halted on a watchpoint on dbg_frames — which is written once
per frame, after the picture is complete — so the framebuffer read at that
halt is a whole frame, and dbg_frames says which one.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, "tools")
from swd import Ocd, symbols          # noqa: E402


def fb_address():
    """`fb` is a file-static in lcd.c, so the map carries it as the section
    .bss.fb rather than as a symbol line swd.py's parser picks up"""
    with open(os.environ.get("SWD_MAP", "emu.map")) as f:
        for line in f:
            m = re.match(r"\s*\.bss\.fb\s+0x([0-9a-f]{8})", line)
            if m:
                return int(m.group(1), 16)
    raise SystemExit("no .bss.fb in the map")


def main():
    argv = sys.argv[1:]
    targets, out, auto = [], "board", 60
    i = 0
    while i < len(argv):
        if argv[i] == "--out":
            out = argv[i + 1]; i += 2
        elif argv[i] == "--auto":
            auto = int(argv[i + 1]); i += 2
        else:
            targets.append(int(argv[i])); i += 1
    targets = sorted(targets)

    syms = symbols()
    o = Ocd()
    fb, fa = fb_address(), syms["dbg_frames"]
    print("fb 0x%08x  dbg_frames 0x%08x" % (fb, fa), flush=True)

    def rd(addr, n=1, tries=10):
        """a read straight after a watchpoint halt sometimes comes back
        empty; retry rather than lose the run"""
        for _ in range(tries):
            try:
                return o.read_block(addr, n)
            except SystemExit:
                time.sleep(0.05)
        raise SystemExit("board read of 0x%08x failed" % addr)

    # The C runtime clears .bss on the way into main, so a value written
    # right after `reset halt` is wiped. Reset, let the board get through
    # startup, then arm: the script's phase comes from the frame counter,
    # so arming at frame 3 drives frames 20..22 exactly as the host does.
    pa = syms["dbg_pad_auto"]
    o.cmd("reset run")
    while rd(fa)[0] < 1:
        time.sleep(0.01)
    o.cmd("halt")
    o.cmd("mww 0x%08x %d" % (pa, auto))
    got = rd(pa)[0]
    print("dbg_pad_auto =", got, "at frame", rd(fa)[0], flush=True)
    if got != auto:
        raise SystemExit("arming dbg_pad_auto did not stick")
    o.cmd("wp 0x%08x 4 w" % fa)
    o.cmd("resume")

    want = list(targets)
    f = 0
    t0 = time.time()
    while want and time.time() - t0 < 400:
        while "halted" not in o.cmd("").lower():
            time.sleep(0.001)
        f = rd(fa)[0]
        if f >= want[0]:
            path = "%s_%06d.raw" % (out, f)
            o.cmd("dump_image %s 0x%08x %d" % (path, fb, 61440))
            print("  frame %d -> %s" % (f, path), flush=True)
            while want and want[0] <= f:
                want.pop(0)
        o.cmd("resume")
    o.cmd("halt")
    o.cmd("rwp 0x%08x" % fa)
    o.cmd("resume")
    print("done at frame %d after %.1f s" % (f, time.time() - t0))


if __name__ == "__main__":
    main()
