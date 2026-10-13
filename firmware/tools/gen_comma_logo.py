#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pre-render the Comma logo, as the app's logo animation leaves it at rest, for the screen.

Usage: gen_comma_logo.py [SIZE]   (screen: 160, the default, or 466 round; WxH, e.g. 135x240)

Geometry, keyframe matrices and easing are those of the Comma app's logo
animation (defaults: easeInOut, zoom 2 s, hold 1.4 s, nested delay 1 %). The
CSS timeline is evaluated on the host, so the device only has to blit one frame.
The logo is a Comma trademark; see THIRD_PARTY_NOTICES.md.

Output:
  ui/<SIZE>/comma_logo.bin           the screen, 4 bpp, high nibble = left pixel, raster order (ui/comma_logo.h)
  build-preview/comma-logo-<SIZE>.png  for eyeballing

Needs: pip install skia-python pillow numpy
"""

import pathlib
import sys

import numpy as np
import skia
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent

SCREEN_NAME = sys.argv[1] if len(sys.argv) > 1 else "160"
# A square screen is "N"; a rectangular one "WxH".
WIDTH, HEIGHT = (int(v) for v in SCREEN_NAME.split("x")) if "x" in SCREEN_NAME else (int(SCREEN_NAME),) * 2
LOGO_PX = round(100 * WIDTH / 160)  # logo diameter: 100 px on the 160 px round glass, scaled by width
ZOOM_S = 2.0
NESTED_DELAY_PCT = 1.0
BEZIER = (0.77, 0.0, 0.175, 1.0)  # easeInOut

LOGO = "M365.205 0C566.902 0 730.411 163.508 730.411 365.205C730.411 392.364 727.389 418.813 721.737 444.273C719.941 452.362 708.534 452.973 704.742 445.605C688.107 413.29 662.342 385.203 628.529 365.681C534.639 311.479 414.537 343.676 360.331 437.565C306.144 531.45 338.338 651.516 432.215 705.722C433.08 706.222 433.949 706.715 434.82 707.198C442.064 711.222 441.112 722.559 432.968 724.093C411.011 728.229 388.362 730.411 365.205 730.411C163.51 730.411 0.00300803 566.9 0 365.205C0.000195125 163.508 163.508 0.000175857 365.205 0Z"
MASK = "M365 0A365 365 0 0 1 730 365V395A335 335 0 0 1 395 730H365A365 365 0 0 1 0 365A365 365 0 0 1 365 0Z"
COMMA = "M429.061 468.826C465.24 406.162 545.367 384.693 608.03 420.871C668.125 455.568 690.333 530.683 660.141 592.052C660.216 592.125 660.291 592.197 660.366 592.27C659.322 594.203 658.133 596.254 656.801 598.406C656.532 598.885 656.261 599.364 655.986 599.84C649.213 611.571 640.898 621.858 631.448 630.594C600.921 662.613 550.308 698.191 480.175 708.083C477.447 708.468 475.903 705.159 477.946 703.31C490.729 691.748 505.726 678.045 520.57 663.529C505.64 660.994 490.898 655.812 477.015 647.796C414.351 611.617 392.882 531.489 429.061 468.826Z"

ZOOM_END = (2.7756653992, -1140.7984790875, -1118.5931558935)  # scale, tx, ty
NESTED_ORIGIN = (606.44863, 595.56644)
APERTURE = (530.35544005, 535.69550225, 196.33236187)


def bezier(p1x, p1y, p2x, p2y):
    """CSS cubic-bezier timing function: x -> y."""
    def coord(t, a, b):
        return 3 * a * t * (1 - t) ** 2 + 3 * b * t * t * (1 - t) + t ** 3

    def ease(x):
        if x <= 0:
            return 0.0
        if x >= 1:
            return 1.0
        lo, hi = 0.0, 1.0
        for _ in range(60):
            mid = (lo + hi) / 2
            if coord(mid, p1x, p2x) < x:
                lo = mid
            else:
                hi = mid
        return coord((lo + hi) / 2, p1y, p2y)

    return ease


def render(ease, t):
    """Frame at time t seconds into the cycle -> float gray array (0 bg .. 1 fg)."""
    zoom = ease(min(t / ZOOM_S, 1.0))
    nested_start = ZOOM_S * NESTED_DELAY_PCT / 100
    nested = ease(max(0.0, min((t - nested_start) / (ZOOM_S - nested_start), 1.0)))

    # CSS interpolates the matrix() keyframes as decomposed scale + translate.
    s = 1 + (ZOOM_END[0] - 1) * zoom
    tx, ty = ZOOM_END[1] * zoom, ZOOM_END[2] * zoom
    ox, oy = NESTED_ORIGIN
    cx, cy, r = APERTURE
    margin_x = (WIDTH - LOGO_PX) / 2
    margin_y = (HEIGHT - LOGO_PX) / 2
    k = LOGO_PX / 730

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">
  <rect width="{WIDTH}" height="{HEIGHT}" fill="#000"/>
  <defs><clipPath id="b"><path d="{MASK}"/></clipPath></defs>
  <g transform="translate({margin_x} {margin_y}) scale({k})">
    <g clip-path="url(#b)">
      <g transform="matrix({s} 0 0 {s} {tx} {ty})">
        <path fill="#fff" d="{LOGO}"/>
        <path fill="#fff" d="{COMMA}"/>
        <g transform="translate({ox} {oy}) scale({nested}) translate({-ox} {-oy})">
          <g transform="translate(606.44863 595.56644) scale(0.3602739726) translate(-542.5 -534.5)">
            <circle fill="#000" cx="{cx}" cy="{cy}" r="{r}"/>
            <path fill="#fff" d="{COMMA}"/>
          </g>
        </g>
      </g>
    </g>
  </g>
</svg>"""
    dom = skia.SVGDOM.MakeFromStream(skia.MemoryStream(svg.encode(), True))
    surface = skia.Surface(WIDTH, HEIGHT)
    canvas = surface.getCanvas()
    canvas.clear(skia.ColorBLACK)
    dom.render(canvas)
    rgba = surface.makeImageSnapshot().toarray()
    return rgba[:, :, 0].astype(np.float32) / 255.0


def main():
    # The screen only shows the logo at rest: the last frame of the zoom (the device used to play
    # the whole zoom during calls; that was dropped to save power).
    still = render(bezier(*BEZIER), ZOOM_S)
    q = np.clip(np.rint(still * 15), 0, 15).astype(np.uint8)
    # Pairs in raster order: the same bytes as row by row for an even width, and an odd width
    # (135) still packs.
    flat = q.reshape(-1)
    blob = ((flat[0::2] << 4) | flat[1::2]).tobytes()

    (ROOT / "ui" / SCREEN_NAME).mkdir(exist_ok=True)
    (ROOT / "ui" / SCREEN_NAME / "comma_logo.bin").write_bytes(blob)

    preview = ROOT / "build-preview"
    preview.mkdir(exist_ok=True)
    Image.fromarray(q * 17).save(preview / f"comma-logo-{SCREEN_NAME}.png")
    print(f"{len(blob)} bytes")


if __name__ == "__main__":
    main()
