#!/bin/sh
# ocd_flash.sh — flash emu.bin through the already-running openocd.
#
# st-flash cannot open the ST-Link while openocd holds it ("another process
# has device opened for exclusive access"), so go through openocd's telnet
# port instead. Writes, verifies, then resets into the new image.
#
#   tools/ocd_flash.sh [image] [addr]
set -e
IMG=${1:-emu.bin}
ADDR=${2:-0x08000000}
python3 - "$IMG" "$ADDR" <<'PY'
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)) if "__file__" in dir() else "tools"))
sys.path.insert(0, "tools")
from swd import Ocd
img, addr = os.path.abspath(sys.argv[1]), sys.argv[2]
o = Ocd()
for line in ("reset halt",
             "flash write_image erase %s %s" % (img, addr),
             "verify_image %s %s" % (img, addr),
             "reset run"):
    o.cmd(line)
    time.sleep(1.5)
    for l in o._read_until_prompt().splitlines():
        l = l.strip()
        if l and l != ">" and not l.startswith(line.split()[0]):
            print(l)
PY
