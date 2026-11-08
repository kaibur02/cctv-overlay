#!/usr/bin/env python3
"""Synthetic, CCTV-looking test scenes for the demo/screenshots (no real camera footage).

Prints the ffmpeg lavfi source and the video filter chain for one scene, for tests/tools/rtsp_loopback.py:

    python3 -I tools/demo/scenes.py front     ->  two lines: <lavfi source>  /  <-vf filter chain>

Each scene is drawn from flat boxes (buildings, doors, fences, a moving car or person), then gets sensor
noise and an NVR-style on-screen display (camera name bottom-left, date/time top-right). Night scenes are
greyscale with heavier noise, like infrared footage.
"""
import sys

W, H = 960, 540


def box(x, y, w, h, color, t="fill"):
    return f"drawbox=x={x}:y={y}:w={w}:h={h}:color={color}:t={t}"


def sky(c0, c1, horizon):
    return f"gradients=s={W}x{H}:r=25:c0={c0}:c1={c1}:x0=0:y0=0:x1=0:y1={horizon}:speed=0.0001:n=2"


def osd(name, n):
    font = "fontcolor=white:fontsize=22:shadowx=2:shadowy=2:shadowcolor=black@0.7"
    return [
        f"drawtext=text='CH{n:02d}  {name.upper()}':x=18:y=h-th-18:{font}",
        f"drawtext=text='%{{localtime\\:%Y-%m-%d %X}}':x=w-tw-18:y=18:{font}",
    ]


def finish(parts, name, n, night=False):
    if night:
        parts += ["hue=s=0", "eq=brightness=0.02:contrast=1.15", "noise=alls=16:allf=t+u"]
    else:
        parts += ["noise=alls=5:allf=t+u"]
    parts += osd(name, n)
    return ",".join(parts)


def front():
    p = [
        box(0, 300, W, H - 300, "0x6e7a5a"),            # lawn
        box(140, 90, 680, 300, "0xd9cbb0"),             # house wall
        box(120, 70, 720, 28, "0x5a4a42"),              # roof edge
        box(430, 200, 100, 190, "0x6b3e26"),            # door
        box(510, 290, 10, 10, "0xd8b04a"),              # handle
        box(200, 160, 150, 110, "0x9fc3d8"), box(200, 160, 150, 110, "0xffffff", 6),
        box(610, 160, 150, 110, "0x9fc3d8"), box(610, 160, 150, 110, "0xffffff", 6),
        box(400, 390, 160, 26, "0x9a9a96"),             # step
        box(380, 416, 200, 124, "0xb8b3a8"),            # path
        # a visitor walking up and down the path
        "drawbox=x='455+130*sin(t/2.2)':y='330-20*abs(sin(t/2.2))':w=46:h=120:color=0x2d3b55:t=fill",
        "drawbox=x='463+130*sin(t/2.2)':y='300-20*abs(sin(t/2.2))':w=30:h=32:color=0xe0b89a:t=fill",
    ]
    return sky("0x8fb8de", "0xe5edf2", 300), finish(p, "Front Door", 1)


def driveway():
    p = [
        box(0, 250, W, H - 250, "0x7d8a63"),            # verge
        box(0, 250, W, 90, "0x3d3f42"),                 # road
        box(0, 292, W, 6, "0xe8e2c8"),                  # road marking
        box(330, 340, 330, 200, "0xa9a69f"),            # driveway
        box(560, 120, 300, 130, "0xcfc7b6"),            # garage
        box(590, 150, 240, 100, "0xf2f0ea"), box(590, 175, 240, 3, "0xbab6aa"), box(590, 205, 240, 3, "0xbab6aa"),
        # passing car
        "drawbox=x='mod(t*170\\,iw+320)-320':y=262:w=230:h=60:color=0xb3262b:t=fill",
        "drawbox=x='mod(t*170\\,iw+320)-270':y=246:w=120:h=26:color=0x2a3540:t=fill",
        "drawbox=x='mod(t*170\\,iw+320)-290':y=312:w=36:h=20:color=0x111111:t=fill",
        "drawbox=x='mod(t*170\\,iw+320)-150':y=312:w=36:h=20:color=0x111111:t=fill",
        # parked car
        box(380, 380, 240, 110, "0xe9ecef"), box(410, 350, 170, 45, "0x3a4a5a"),
    ]
    return sky("0x9cc2e0", "0xdde7ee", 250), finish(p, "Driveway", 2)


def backyard():
    p = [box(0, 280, W, H - 280, "0x3c3c3c")]
    p += [box(0, 150, W, 14, "0x8a8a8a"), box(0, 250, W, 14, "0x8a8a8a")]   # fence rails
    p += [box(x, 130, 34, 160, "0x9c9c9c") for x in range(10, W, 70)]        # fence posts
    p += [
        box(90, 330, 220, 120, "0x5a5a5a"),             # outdoor table
        box(700, 300, 160, 150, "0x2a2a2a"),            # shrub
        # a cat strolling along the lawn
        "drawbox=x='mod(t*55\\,iw+120)-120':y=470:w=70:h=26:color=0xdddddd:t=fill",
        "drawbox=x='mod(t*55\\,iw+120)-60':y=455:w=22:h=22:color=0xdddddd:t=fill",
    ]
    return sky("0x1a1a1a", "0x444444", 280), finish(p, "Backyard", 3, night=True)


def garage():
    p = [
        box(0, 380, W, H - 380, "0x8b8a85"),            # floor
        box(40, 60, 260, 12, "0x6a5038"), box(40, 140, 260, 12, "0x6a5038"), box(40, 220, 260, 12, "0x6a5038"),
        box(60, 30, 40, 30, "0x2f6fa8"), box(120, 110, 60, 30, "0xc0802a"), box(200, 190, 50, 30, "0x3a8a4a"),
        box(640, 250, 280, 130, "0x6a5038"),            # workbench
        box(330, 260, 300, 150, "0x2b2f36"), box(370, 210, 210, 60, "0x48607a"),  # parked car
        # fluorescent light flicker
        "eq=brightness='0.03*sin(t*3)':eval=frame",
    ]
    return sky("0xb9b5aa", "0x8e8a80", 380), finish(p, "Garage", 4)


def alley():
    p = [
        box(0, 0, 260, H, "0x2b2b2b"), box(700, 0, 260, H, "0x262626"),       # walls
        box(260, 330, 440, H - 330, "0x4a4a4a"),        # path
        box(420, 40, 120, 30, "0xf5f5f5"),              # security light
        box(130, 380, 60, 80, "0x3a3a3a"), box(760, 360, 70, 100, "0x3a3a3a"),  # bins
        # someone walking through
        "drawbox=x='480+90*sin(t/3)':y='300+60*sin(t/1.5)^2':w=40:h=110:color=0x101010:t=fill",
    ]
    return sky("0x202020", "0x3a3a3a", 330), finish(p, "Side Alley", 5, night=True)


SCENES = {"front": front, "driveway": driveway, "backyard": backyard, "garage": garage, "alley": alley}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in SCENES:
        sys.exit(f"usage: scenes.py {{{'|'.join(SCENES)}}}")
    src, vf = SCENES[sys.argv[1]]()
    print(src)
    print(vf)
