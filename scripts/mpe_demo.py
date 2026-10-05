#!/usr/bin/env python3
"""Play an MPE test stream into the Ableton Move over USB (Windows, no installs).

Lets you hear Fizzik's MPE without owning an MPE controller.

Move setup first:
  1. Fizzik loaded in a Schwung slot, slot Receive channel = All, Forward = Auto/Thru
  2. Fizzik > MPE page: MPE = On, Zone = Lower, Bend Range = 48 (defaults)
  3. A sustaining-ish preset helps (CaveStrings, BowedGlass, StarlightPad)

Usage:
  python scripts/mpe_demo.py probe          # plays a phrase on each Move port: note which one sounds
  python scripts/mpe_demo.py demo --port 2  # run the MPE demo on that port (1-4)
  python scripts/mpe_demo.py demo --port 2 --only slide,bow   # just some sections

Sections: plain, slide, gliss, vibrato, pressure, timbre, bow
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes

winmm = ctypes.WinDLL("winmm")


class MIDIOUTCAPS(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", ctypes.c_wchar * 32), ("wTechnology", wintypes.WORD), ("wVoices", wintypes.WORD),
                ("wNotes", wintypes.WORD), ("wChannelMask", wintypes.WORD), ("dwSupport", wintypes.DWORD)]


def move_ports():
    """[(device_index, name)] for the Move's MIDI outputs, in port order 1..4."""
    out = []
    for i in range(winmm.midiOutGetNumDevs()):
        c = MIDIOUTCAPS()
        winmm.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c))
        if "Ableton Move" in c.szPname:
            out.append((i, c.szPname))
    return out


class Out:
    BEND_RANGE = 48.0  # semitones — must match Fizzik's "Bend Range"

    def __init__(self, dev, label=""):
        # midiOutOpen can block forever inside the Windows MIDI service on some
        # ports. A blocked call can't be cancelled, so open on a helper thread
        # and bail out of the whole process with a clear message on timeout.
        import os
        import threading
        self.h = wintypes.HANDLE()
        res = {}

        def _open():
            res["rc"] = winmm.midiOutOpen(ctypes.byref(self.h), dev, 0, 0, 0)

        t = threading.Thread(target=_open, daemon=True)
        t.start()
        t.join(8.0)
        if t.is_alive():
            print(f"\nSTALLED opening {label or f'device {dev}'}: the Windows MIDI service did not answer.\n"
                  "That port is unusable from this PC right now, and the service may now be stuck.\n"
                  "Fix: unplug/replug the Move's USB cable, or in an ADMIN terminal run\n"
                  "  net stop midisrv   then   net start midisrv\n"
                  "then retry with a different --port.", flush=True)
            os._exit(3)
        if res.get("rc", 1) != 0:
            raise OSError(f"midiOutOpen failed (code {res.get('rc')}) — is another app holding the port?")

    def close(self):
        winmm.midiOutReset(self.h)
        winmm.midiOutClose(self.h)

    def send(self, status, d1=0, d2=0):
        winmm.midiOutShortMsg(self.h, status | (d1 << 8) | (d2 << 16))

    # ch is 0-based: 0 = MIDI channel 1 (the MPE lower-zone master)
    def on(self, ch, note, vel=100):
        self.send(0x90 | ch, note, vel)

    def off(self, ch, note):
        self.send(0x80 | ch, note, 0)

    def bend(self, ch, semis):
        v = int(round(8192 + 8191 * max(-1.0, min(1.0, semis / self.BEND_RANGE))))
        self.send(0xE0 | ch, v & 0x7F, (v >> 7) & 0x7F)

    def pressure(self, ch, amt):          # 0..1
        self.send(0xD0 | ch, int(round(127 * max(0.0, min(1.0, amt)))))

    def timbre(self, ch, amt):            # 0..1, 0.5 = centre
        self.send(0xB0 | ch, 74, int(round(127 * max(0.0, min(1.0, amt)))))

    def neutral(self):
        for ch in range(16):
            self.bend(ch, 0.0)
            self.pressure(ch, 0.0)
            self.timbre(ch, 0.5)
            self.send(0xB0 | ch, 123, 0)  # all notes off


def ramp(fn, a, b, secs, step=0.005):
    n = max(1, int(secs / step))
    for k in range(n + 1):
        fn(a + (b - a) * k / n)
        time.sleep(step)


def say(msg):
    print(msg, flush=True)


CHORD = [(1, 48), (2, 55), (3, 64)]   # (member channel, note): C3 G3 E4


def chord_on(o):
    for ch, n in CHORD:
        o.on(ch, n)
        time.sleep(0.03)


def chord_off(o):
    for ch, n in CHORD:
        o.off(ch, n)


def sec_plain(o):
    say("plain     : a chord with no expression (reference)")
    chord_on(o); time.sleep(2.0); chord_off(o); time.sleep(1.0)


def sec_slide(o):
    say("slide     : chord held; ONLY the middle note bends up a whole tone and back, twice")
    chord_on(o); time.sleep(0.8)
    for _ in range(2):
        ramp(lambda s: o.bend(2, s), 0, 2, 0.5); time.sleep(0.4)
        ramp(lambda s: o.bend(2, s), 2, 0, 0.5); time.sleep(0.4)
    chord_off(o); time.sleep(1.0)


def sec_gliss(o):
    say("gliss     : one note re-struck while it slides up a full octave, then back down")
    o.on(1, 48); ramp(lambda s: o.bend(1, s), 0, 12, 1.5)
    o.off(1, 48); o.on(1, 48)            # re-strike while bent: must start AT the bent pitch
    time.sleep(0.6)
    ramp(lambda s: o.bend(1, s), 12, 0, 1.5); time.sleep(0.5)
    o.off(1, 48); time.sleep(1.0)


def sec_vibrato(o):
    import math
    say("vibrato   : chord held; only the TOP note gets finger vibrato (growing, then fading)")
    chord_on(o); time.sleep(0.6)
    t0 = time.time()
    while (t := time.time() - t0) < 3.0:
        depth = 0.5 * math.sin(math.pi * t / 3.0)            # semitones, swells then fades
        o.bend(3, depth * math.sin(2 * math.pi * 5.5 * t))
        time.sleep(0.004)
    o.bend(3, 0); chord_off(o); time.sleep(1.0)


def sec_pressure(o):
    say("pressure  : chord held; pressure swells on the BOTTOM note only (uses your AT preset)")
    chord_on(o); time.sleep(0.8)
    for _ in range(2):
        ramp(lambda p: o.pressure(1, p), 0, 1, 1.0); time.sleep(0.5)
        ramp(lambda p: o.pressure(1, p), 1, 0, 0.8); time.sleep(0.3)
    chord_off(o); time.sleep(1.0)


def sec_timbre(o):
    say("timbre    : chord held; the Y axis (CC74) sweeps on the middle note only")
    say("            (Timbre Tgt = Bright: that one voice darkens then brightens)")
    for ch, _ in CHORD:
        o.timbre(ch, 0.5)
    chord_on(o); time.sleep(0.6)
    ramp(lambda t: o.timbre(2, t), 0.5, 0.0, 0.8)
    ramp(lambda t: o.timbre(2, t), 0.0, 1.0, 1.6)
    ramp(lambda t: o.timbre(2, t), 1.0, 0.5, 0.8)
    chord_off(o); time.sleep(1.0)


def sec_bow(o):
    say("bow       : SET 'Timbre Tgt' = Bow on the Move first. Each note of the chord is")
    say("            bowed in turn by its own Y axis, then all three together")
    for ch, _ in CHORD:
        o.timbre(ch, 0.0)
    chord_on(o); time.sleep(1.2)
    for ch, _ in CHORD:
        ramp(lambda t, c=ch: o.timbre(c, t), 0, 1, 0.7); time.sleep(0.8)
        ramp(lambda t, c=ch: o.timbre(c, t), 1, 0, 0.5); time.sleep(0.3)
    for k in range(141):
        for ch, _ in CHORD:
            o.timbre(ch, k / 140.0)
        time.sleep(0.005)
    time.sleep(1.5)
    chord_off(o); time.sleep(1.5)


SECTIONS = {"plain": sec_plain, "slide": sec_slide, "gliss": sec_gliss, "vibrato": sec_vibrato,
            "pressure": sec_pressure, "timbre": sec_timbre, "bow": sec_bow}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["probe", "demo", "list"])
    ap.add_argument("--port", type=int, default=0, help="Move port number 1-4 (from 'probe')")
    ap.add_argument("--only", default="", help="comma-separated sections to play")
    a = ap.parse_args()

    ports = move_ports()
    if not ports:
        sys.exit("No 'Ableton Move' MIDI output found — is the Move connected over USB?")
    if a.mode == "list":
        for k, (dev, name) in enumerate(ports, 1):
            say(f"port {k}: {name}")
        return

    if a.mode == "probe":
        say("Playing a rising 3-note phrase on each Move port. Note which port NUMBER makes Fizzik sound.")
        for k, (dev, name) in enumerate(ports, 1):
            say(f"  port {k}: {name}")
            try:
                o = Out(dev, f"port {k} ({name})")
            except OSError as e:
                say(f"    (skipped: {e})"); continue
            try:
                for note in (60, 64, 67):
                    o.on(1, note); time.sleep(0.25); o.off(1, note); time.sleep(0.05)
                time.sleep(1.5)
            finally:
                o.neutral(); o.close()
        say("Done. Run:  python scripts/mpe_demo.py demo --port <number that sounded>")
        return

    if not 1 <= a.port <= len(ports):
        sys.exit(f"--port must be 1..{len(ports)} (run 'probe' to find the one that sounds)")
    names = [s.strip() for s in a.only.split(",") if s.strip()] or list(SECTIONS)
    bad = [n for n in names if n not in SECTIONS]
    if bad:
        sys.exit(f"unknown section(s): {', '.join(bad)} — choose from {', '.join(SECTIONS)}")
    dev, name = ports[a.port - 1]
    o = Out(dev, f"port {a.port} ({name})")
    say(f"MPE demo -> {name}   (Fizzik: MPE On, Zone Lower, Bend Range 48)")
    try:
        o.neutral(); time.sleep(0.2)
        for n in names:
            SECTIONS[n](o)
    except KeyboardInterrupt:
        say("interrupted")
    finally:
        o.neutral(); o.close()
    say("done")


if __name__ == "__main__":
    main()
