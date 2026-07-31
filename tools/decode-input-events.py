#!/usr/bin/env python3
"""Decode a raw Linux evdev capture (a dump of /dev/input/eventN).

The device has no evtest/od/xxd, so the workflow is to capture on the
Zaurus and decode on the host:

    # on device (blocks until 8 events arrive -- tap while it runs)
    dd if=/dev/input/event2 of=/tmp/ts.bin bs=16 count=8
    # on host
    ssh zaurus 'cat /tmp/ts.bin' > ts.bin
    tools/decode-input-events.py ts.bin

With --calibrate it instead reports the min/max of ABS_X/ABS_Y, which is
what the touchscreen calibration constants in
modules/x11/xserver-kdrive-evdev-absolute.patch are derived from. Samples
with pressure 0 are ignored there: pen-up carries stale coordinates and
would skew the range. See docs/HOWTO-X11-TOUCHSCREEN.md.

Note the 16-byte struct input_event below is the 32-bit ARM layout
(two 32-bit timeval fields). A capture taken on a 64-bit host would use
a 24-byte struct instead.
"""

import argparse
import struct
import sys

EVENT_SIZE = 16
EVENT_FMT = "<IIHHi"  # tv_sec, tv_usec, type, code, value (32-bit ARM)

EV_SYN, EV_KEY, EV_ABS, EV_REL = 0x00, 0x01, 0x03, 0x02

TYPES = {EV_SYN: "EV_SYN", EV_KEY: "EV_KEY", EV_REL: "EV_REL", EV_ABS: "EV_ABS"}

CODES = {
    (EV_SYN, 0): "SYN_REPORT",
    (EV_KEY, 0x14a): "BTN_TOUCH",
    (EV_KEY, 0x110): "BTN_LEFT",
    (EV_KEY, 0x111): "BTN_RIGHT",
    (EV_KEY, 0x112): "BTN_MIDDLE",
    (EV_ABS, 0): "ABS_X",
    (EV_ABS, 1): "ABS_Y",
    (EV_ABS, 24): "ABS_PRESSURE",
    (EV_REL, 0): "REL_X",
    (EV_REL, 1): "REL_Y",
}

ABS_X, ABS_Y, ABS_PRESSURE = 0, 1, 24


def read_events(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) % EVENT_SIZE:
        print(f"warning: {len(data)} bytes is not a multiple of {EVENT_SIZE}; "
              f"ignoring trailing {len(data) % EVENT_SIZE} byte(s)",
              file=sys.stderr)
    for off in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE):
        yield struct.unpack(EVENT_FMT, data[off:off + EVENT_SIZE])


def cmd_dump(events):
    n = 0
    for sec, usec, typ, code, val in events:
        tname = TYPES.get(typ, f"type{typ}")
        cname = CODES.get((typ, code), f"code{code}")
        print(f"{sec}.{usec:06d}  {tname:7} {cname:13} value={val}")
        n += 1
    print(f"\n{n} event(s)")


def cmd_calibrate(events):
    """Track a whole frame at a time so pressure gates the sample properly."""
    xs, ys = [], []
    cur = {}
    kept = dropped = 0

    for _sec, _usec, typ, code, val in events:
        if typ == EV_ABS:
            cur[code] = val
        elif typ == EV_SYN:
            if ABS_X in cur and ABS_Y in cur:
                # Pen-up frames repeat the last coordinates -- skip them.
                if cur.get(ABS_PRESSURE, 1) > 0:
                    xs.append(cur[ABS_X])
                    ys.append(cur[ABS_Y])
                    kept += 1
                else:
                    dropped += 1
            cur = {}

    if not xs:
        print("no frames with pressure > 0 -- did the capture include real "
              "touches?", file=sys.stderr)
        return 1

    print(f"frames kept: {kept}   dropped (pressure 0): {dropped}")
    print(f"X: min={min(xs)} max={max(xs)}  span={max(xs) - min(xs)}")
    print(f"Y: min={min(ys)} max={max(ys)}  span={max(ys) - min(ys)}")
    print()
    print("Calibration constants for hw/kdrive/linux/evdev.c:")
    print(f"    #define EVDEV_ABS_CAL_XMIN {min(xs)}")
    print(f"    #define EVDEV_ABS_CAL_XMAX {max(xs)}")
    print(f"    #define EVDEV_ABS_CAL_YMIN {min(ys)}")
    print(f"    #define EVDEV_ABS_CAL_YMAX {max(ys)}")
    print()
    print("Touch opposite corners hard when capturing, or the range will be")
    print("too small and the cursor will not reach the screen edges.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="raw dump of /dev/input/eventN")
    ap.add_argument("--calibrate", action="store_true",
                    help="report ABS_X/ABS_Y min/max instead of dumping events")
    args = ap.parse_args()

    events = list(read_events(args.capture))
    if args.calibrate:
        return cmd_calibrate(events)
    cmd_dump(events)
    return 0


if __name__ == "__main__":
    sys.exit(main())
