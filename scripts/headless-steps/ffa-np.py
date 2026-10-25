#!/usr/bin/env python3
"""Nameplate/lock/damage readout for an ffa-cal.sh frame (2026-10-24).
Finds the dummy's health bar (widest saturated horizontal run near screen
centre), then reports the bar colour, the name text colour (bright pixels
just above it), module slots 1-4 lock state (green vs blue) and the
colour of any damage numbers around the reticle."""
import sys, subprocess, colorsys
def load(p):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", p, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True, check=True).stdout
    return lambda x, y: raw[(y * 1280 + x) * 3:(y * 1280 + x) * 3 + 3]
def hsv(c):
    h, s, v = colorsys.rgb_to_hsv(*(b / 255 for b in c)); return h * 360, s, v
def med(px):
    if not px: return None
    return tuple(sorted(p[i] for p in px)[len(px) // 2] for i in range(3))
for p in sys.argv[1:]:
    g = load(p)
    bar = None
    for y in range(200, 420):
        run = best = 0
        for x in range(450, 830):
            h, s, v = hsv(g(x, y))
            run = run + 1 if s > .35 and v > .6 else 0
            best = max(best, run)
        if best >= 90:
            bar = y; break
    out = {}
    if bar is not None:
        out["bar_y"] = bar
        out["bar"] = med([g(x, bar + 2) for x in range(560, 720)])
        bh, bs, _ = hsv(out["bar"])
        def near(c):  # the name is drawn in the bar's hue (white for team 0/8)
            h, s, v = hsv(c)
            if bs < .2: return s < .15 and v > .8
            return s > .3 and v > .6 and min(abs(h - bh), 360 - abs(h - bh)) < 15
        txt = [g(x, y) for y in range(bar - 40, bar - 22) for x in range(560, 720) if near(g(x, y))]
        out["name"] = med(txt); out["name_px"] = len(txt)
    slots = []
    for cx in (499, 593, 686, 780):
        r, gg, b = med([g(x, y) for x in range(cx - 25, cx + 25) for y in (26, 48)])
        # green slot hue ~150-160, blue ~190-200 (dimmer against a bright sky)
        slots.append("G" if hsv((r, gg, b))[0] < 175 else "b")
    out["lock"] = "".join(slots)
    dmg = [g(x, y) for y in range(250, 470) for x in range(520, 760)
           if 40 < hsv(g(x, y))[0] < 65 and hsv(g(x, y))[1] > .5 and hsv(g(x, y))[2] > .8]
    out["yellow_px"] = len(dmg)
    print(p.rsplit("/", 1)[-1], out)
