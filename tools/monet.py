#!/usr/bin/env python3
"""Generate the WebUI colour tokens with the Monet TonalSpot 2025 scheme.

CAM16 / HCT is a direct port of material-color-utilities, so a palette built
here is the same palette Android derives from a wallpaper seed. The role tones
are the ones `SchemeTonalSpot(spec: 2025)` resolves to; every role lands within
3/255 of the reference implementation (checked against
@material/material-color-utilities 0.4.0).

Two deliberate deviations, both marked where they are defined:
  * dark `tertiary-container` keeps tone 30 — the reference output repeats the
    light tone (83.9) and would put a pale pink panel in a dark UI;
  * `inverse-primary` follows the 2021 pairing, which the 2025 spec no longer
    restates.

Usage:
    python3 tools/monet.py                    # print the CSS token blocks
    python3 tools/monet.py --check            # add contrast diagnostics
    python3 tools/monet.py --seed '#6a49c8'   # re-seed
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

# --------------------------------------------------------------------- utils

WHITE_POINT_D65 = (95.047, 100.0, 108.883)


def clamp(lo: float, hi: float, value: float) -> float:
    return max(lo, min(hi, value))


def sign(value: float) -> float:
    return -1.0 if value < 0 else (1.0 if value > 0 else 0.0)


def lerp(start: float, stop: float, amount: float) -> float:
    return start + (stop - start) * amount


def linearized(channel: float) -> float:
    normalized = channel / 255.0
    absolute = abs(normalized)
    if absolute <= 0.040449936:
        linear = absolute / 12.92 * 100.0
    else:
        linear = ((absolute + 0.055) / 1.055) ** 2.4 * 100.0
    return sign(normalized) * linear


def delinearized(channel: float) -> float:
    normalized = channel / 100.0
    absolute = abs(normalized)
    if absolute <= 0.0031308:
        linear = absolute * 12.92
    else:
        linear = 1.055 * absolute ** (1.0 / 2.4) - 0.055
    return sign(normalized) * linear * 255.0


def y_from_lstar(lstar: float) -> float:
    if lstar > 8.0:
        return 100.0 * ((lstar + 16.0) / 116.0) ** 3
    return 100.0 * lstar / 903.2962962


def lstar_from_y(y: float) -> float:
    normalized = y / 100.0
    e = 216.0 / 24389.0
    kappa = 24389.0 / 27.0
    if normalized > e:
        return math.cbrt(normalized) * 116.0 - 16.0
    return (kappa * normalized + 16.0) / 116.0 * 116.0 - 16.0


def xyz_from_rgb(rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    r, g, b = (linearized(c) for c in rgb)
    return (
        0.41233895 * r + 0.35762064 * g + 0.18051042 * b,
        0.2126 * r + 0.7152 * g + 0.0722 * b,
        0.01932141 * r + 0.11916382 * g + 0.95034478 * b,
    )


def rgb_from_xyz(xyz: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = xyz
    r = 3.2406 * x - 1.5372 * y - 0.4986 * z
    g = -0.9689 * x + 1.8758 * y + 0.0415 * z
    b = 0.0557 * x - 0.2040 * y + 1.0570 * z
    return tuple(clamp(0.0, 255.0, delinearized(c)) for c in (r, g, b))


def lstar_from_rgb(rgb: tuple[float, float, float]) -> float:
    return lstar_from_y(xyz_from_rgb(rgb)[1])


def argb_from_rgb(rgb: tuple[float, float, float]) -> int:
    r, g, b = (int(round(c)) for c in rgb)
    return 0xFF000000 | (r << 16) | (g << 8) | b


def rgb_from_argb(argb: int) -> tuple[int, int, int]:
    return ((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF)


def argb_from_lstar(lstar: float) -> int:
    return argb_from_rgb(tuple(delinearized(y_from_lstar(lstar)) for _ in range(3)))


def hex_from_argb(argb: int) -> str:
    r, g, b = rgb_from_argb(argb)
    return f"#{r:02x}{g:02x}{b:02x}"


def argb_from_hex(value: str) -> int:
    text = value.strip().lstrip("#")
    if len(text) == 3:
        text = "".join(c * 2 for c in text)
    if len(text) != 6:
        raise SystemExit(f"unsupported colour: {value}")
    return 0xFF000000 | int(text, 16)


# ---------------------------------------------------------------- CAM16 / HCT


class ViewingConditions:
    """sRGB-like default viewing conditions (200 lux, 50 L* background)."""

    def __init__(self) -> None:
        xyz = WHITE_POINT_D65
        r_w = xyz[0] * 0.401288 + xyz[1] * 0.650173 + xyz[2] * -0.051461
        g_w = xyz[0] * -0.250268 + xyz[1] * 1.204414 + xyz[2] * 0.045854
        b_w = xyz[0] * -0.002079 + xyz[1] * 0.048952 + xyz[2] * 0.953127

        surround = 2.0
        f = 0.8 + surround / 10.0
        c = lerp(0.59, 0.69, (f - 0.9) * 10.0) if f >= 0.9 else lerp(0.525, 0.59, (f - 0.8) * 10.0)
        adapting_luminance = (200.0 / math.pi) * y_from_lstar(50.0) / 100.0
        d = f * (1.0 - (1.0 / 3.6) * math.exp((-adapting_luminance - 42.0) / 92.0))
        d = clamp(0.0, 1.0, d)

        self.n = y_from_lstar(50.0) / xyz[1]
        self.aw = 0.0
        self.nbb = 0.725 / self.n ** 0.2
        self.ncb = self.nbb
        self.c = c
        self.nc = f
        self.rgb_d = (
            d * (100.0 / r_w) + 1.0 - d,
            d * (100.0 / g_w) + 1.0 - d,
            d * (100.0 / b_w) + 1.0 - d,
        )

        k = 1.0 / (5.0 * adapting_luminance + 1.0)
        k4 = k ** 4
        k4f = 1.0 - k4
        self.fl = k4 * adapting_luminance + 0.1 * k4f * k4f * math.cbrt(5.0 * adapting_luminance)
        self.fl_root = self.fl ** 0.25
        self.z = 1.48 + math.sqrt(self.n)

        factors = [
            (self.fl * self.rgb_d[i] * (r_w, g_w, b_w)[i] / 100.0) ** 0.42
            for i in range(3)
        ]
        adapted = tuple(400.0 * factor / (factor + 27.13) for factor in factors)
        self.aw = (2.0 * adapted[0] + adapted[1] + 0.05 * adapted[2]) * self.nbb


VC = ViewingConditions()


def cam16_from_xyz(x: float, y: float, z: float) -> dict[str, float]:
    vc = VC
    r_c = 0.401288 * x + 0.650173 * y - 0.051461 * z
    g_c = -0.250268 * x + 1.204414 * y + 0.045854 * z
    b_c = -0.002079 * x + 0.048952 * y + 0.953127 * z

    r_d, g_d, b_d = (vc.rgb_d[i] * (r_c, g_c, b_c)[i] for i in range(3))

    def adapt(value: float) -> float:
        powered = (vc.fl * abs(value) / 100.0) ** 0.42
        return sign(value) * 400.0 * powered / (powered + 27.13)

    r_a, g_a, b_a = adapt(r_d), adapt(g_d), adapt(b_d)

    a = (11.0 * r_a - 12.0 * g_a + b_a) / 11.0
    b = (r_a + g_a - 2.0 * b_a) / 9.0
    u = (20.0 * r_a + 20.0 * g_a + 21.0 * b_a) / 20.0
    p2 = (40.0 * r_a + 20.0 * g_a + b_a) / 20.0

    hue = math.degrees(math.atan2(b, a)) % 360.0
    ac = p2 * vc.nbb
    j = 100.0 * (ac / vc.aw) ** (vc.c * vc.z)

    hue_prime = hue + 360.0 if hue < 20.14 else hue
    e_hue = 0.25 * (math.cos(math.radians(hue_prime) + 2.0) + 3.8)
    p1 = (50000.0 / 13.0) * e_hue * vc.nc * vc.ncb
    t = p1 * math.hypot(a, b) / (u + 0.305)
    alpha = t ** 0.9 * (1.64 - 0.29 ** vc.n) ** 0.73
    chroma = alpha * math.sqrt(j / 100.0)

    return {"hue": hue, "chroma": chroma, "j": j}


def xyz_from_jch(j: float, chroma: float, hue: float) -> tuple[float, float, float]:
    """Inverse CAM16: J/C/h -> XYZ (the linear part of ``Cam16.viewed``)."""
    vc = VC
    alpha = 0.0 if chroma == 0.0 or j == 0.0 else chroma / math.sqrt(j / 100.0)
    t = (alpha / (1.64 - 0.29 ** vc.n) ** 0.73) ** (1.0 / 0.9)

    hue_rad = math.radians(hue)
    e_hue = 0.25 * (math.cos(hue_rad + 2.0) + 3.8)
    ac = vc.aw * (j / 100.0) ** (1.0 / vc.c / vc.z)
    p1 = e_hue * (50000.0 / 13.0) * vc.nc * vc.ncb
    p2 = ac / vc.nbb

    h_sin, h_cos = math.sin(hue_rad), math.cos(hue_rad)
    gamma = (23.0 * (p2 + 0.305) * t) / (23.0 * p1 + 11.0 * t * h_cos + 108.0 * t * h_sin)
    a, b = gamma * h_cos, gamma * h_sin

    r_a = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0
    g_a = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0
    b_a = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0

    def unadapt(value: float) -> float:
        base = max(0.0, (27.13 * abs(value)) / (400.0 - abs(value)))
        return sign(value) * (100.0 / vc.fl) * base ** (1.0 / 0.42)

    r_f = unadapt(r_a) / vc.rgb_d[0]
    g_f = unadapt(g_a) / vc.rgb_d[1]
    b_f = unadapt(b_a) / vc.rgb_d[2]

    return (
        1.86206786 * r_f - 1.01125463 * g_f + 0.14918677 * b_f,
        0.38752654 * r_f + 0.62144744 * g_f - 0.00897398 * b_f,
        -0.01584150 * r_f - 0.03412294 * g_f + 1.04996444 * b_f,
    )


def hct_from_argb(argb: int) -> dict[str, float]:
    cam = cam16_from_xyz(*xyz_from_rgb(rgb_from_argb(argb)))
    return {
        "hue": cam["hue"],
        "chroma": cam["chroma"],
        "tone": lstar_from_rgb(rgb_from_argb(argb)),
    }


def solve_to_argb(hue: float, chroma: float, tone: float) -> int:
    """HCT -> ARGB, mirroring HctSolver (bisect J, shrink chroma out of gamut)."""
    if chroma < 1e-4 or tone < 1e-4 or tone > 99.9999:
        return argb_from_lstar(tone)

    target_y = y_from_lstar(tone)
    current = chroma
    argb = argb_from_lstar(tone)

    for _ in range(28):
        low, high = 0.0, 100.0
        for _ in range(48):
            mid = (low + high) / 2.0
            y = xyz_from_jch(mid, current, hue)[1]
            if y < target_y:
                low = mid
            else:
                high = mid
        argb = argb_from_rgb(rgb_from_xyz(xyz_from_jch((low + high) / 2.0, current, hue)))
        if abs(lstar_from_rgb(rgb_from_argb(argb)) - tone) < 0.45:
            return argb
        current *= 0.96
    return argb


class Palette:
    def __init__(self, hue: float, chroma: float) -> None:
        self.hue = hue % 360.0
        self.chroma = chroma

    def tone(self, tone: float, multiplier: float = 1.0) -> int:
        return solve_to_argb(self.hue, self.chroma * multiplier, tone)


# -------------------------------------------------------------------- scheme

# Roles -> (palette, tone, chroma multiplier). Tones are the ones the 2025
# TonalSpot spec resolves to for the reference seed (#6a49c8); surface
# containers keep the 2025 chroma multipliers. Cross-checked against
# @material/material-color-utilities 0.4.0 SchemeTonalSpot(spec: 2025).
LIGHT_ROLES = {
    "primary": ("primary", 40.4),
    "on-primary": ("neutral", 98.0),
    "primary-container": ("primary", 81.0),
    "on-primary-container": ("primary", 28.0),
    "secondary": ("secondary", 40.3),
    "secondary-container": ("secondary", 90.0),
    "on-secondary-container": ("secondary", 34.8),
    "tertiary": ("tertiary", 40.3),
    "tertiary-container": ("tertiary", 83.9),
    "on-tertiary-container": ("tertiary", 30.4),
    "error": ("error", 40.4),
    "error-container": ("error", 65.1),
    "on-error-container": ("error", 22.1),
    "surface": ("neutral", 98.1),
    "surface-dim": ("neutral", 87.1, 1.7),
    "surface-lowest": ("neutral", 100.0),
    "surface-low": ("neutral", 96.1, 1.25),
    "surface-container": ("neutral", 94.1, 1.4),
    "surface-high": ("neutral", 92.0, 1.5),
    "surface-highest": ("neutral", 89.9, 1.6),
    "on-surface": ("neutral-variant", 21.0),
    "on-surface-variant": ("neutral-variant", 40.2),
    "outline": ("neutral-variant", 51.5),
    "outline-variant": ("neutral-variant", 72.5),
    "inverse-surface": ("neutral", 3.9),
    "inverse-on-surface": ("neutral", 64.5),
    # Relies on the 2021 rule (primary 80/40); the 2025 spec does not restate it.
    "inverse-primary": ("primary", 80.0),
    "success": ("success", 38.0),
    "tone-1": ("primary", 90.0),
    "tone-2": ("secondary", 90.0),
    "tone-3": ("tertiary", 90.0),
    "tone-4": ("neutral-variant", 90.0),
    "tone-5": ("primary", 95.0),
    "tone-6": ("error", 90.0),
}

DARK_ROLES = {
    "primary": ("primary", 79.9),
    "on-primary": ("primary", 27.2),
    "primary-container": ("primary", 35.1),
    "on-primary-container": ("primary", 90.1),
    "secondary": ("secondary", 80.1),
    "secondary-container": ("secondary", 24.9),
    "on-secondary-container": ("secondary", 77.3),
    "tertiary": ("tertiary", 89.0),
    "tertiary-container": ("tertiary", 30.0),
    "on-tertiary-container": ("tertiary", 90.0),
    "error": ("error", 65.1),
    "error-container": ("error", 30.0),
    "on-error-container": ("error", 73.6),
    "surface": ("neutral", 3.9),
    "surface-dim": ("neutral", 3.9),
    "surface-lowest": ("neutral", 0.0),
    "surface-low": ("neutral", 5.8, 1.25),
    "surface-container": ("neutral", 9.1, 1.4),
    "surface-high": ("neutral", 11.9, 1.5),
    "surface-highest": ("neutral", 14.9, 1.6),
    "on-surface": ("neutral-variant", 91.0),
    "on-surface-variant": ("neutral-variant", 69.9),
    "outline": ("neutral-variant", 49.5),
    "outline-variant": ("neutral-variant", 30.4),
    "inverse-surface": ("neutral", 98.1),
    "inverse-on-surface": ("neutral", 36.2),
    "inverse-primary": ("primary", 40.0),
    "success": ("success", 78.0),
    "tone-1": ("primary", 30.0),
    "tone-2": ("secondary", 30.0),
    "tone-3": ("tertiary", 30.0),
    "tone-4": ("neutral-variant", 30.0),
    "tone-5": ("primary", 20.0),
    "tone-6": ("error", 30.0),
}


def piecewise(hue: float, breakpoints, hues) -> float:
    for index in range(min(len(breakpoints) - 1, len(hues))):
        if breakpoints[index] <= hue < breakpoints[index + 1]:
            return hues[index] % 360.0
    return hue


def build_scheme(seed: int, dark: bool) -> dict[str, str]:
    hue = hct_from_argb(seed)["hue"]

    palettes = {
        "primary": Palette(hue, 26.0 if dark else 32.0),
        "secondary": Palette(hue, 16.0),
        # 2025 rotates the tertiary hue instead of using a flat +60.
        "tertiary": Palette(
            hue + piecewise(hue, [0, 20, 71, 161, 333, 360], [-40, 48, -32, 40, -32]),
            28.0,
        ),
        "neutral": Palette(hue, 5.0),
        "neutral-variant": Palette(hue, 8.0),
        "error": Palette(
            piecewise(hue, [0, 3, 13, 23, 33, 43, 153, 273, 360],
                      [12, 22, 32, 12, 22, 32, 22, 12]),
            60.0,
        ),
        # Not part of Material: the win colour this WebUI shows for a healthy hook.
        "success": Palette(152.0, 40.0),
    }

    table = DARK_ROLES if dark else LIGHT_ROLES
    roles: dict[str, str] = {}
    for name, entry in table.items():
        palette_name, tone = entry[0], entry[1]
        multiplier = entry[2] if len(entry) > 2 else 1.0
        roles[name] = hex_from_argb(palettes[palette_name].tone(tone, multiplier))
    return roles


def contrast(first: str, second: str) -> float:
    def luminance(value: str) -> float:
        rgb = rgb_from_argb(argb_from_hex(value))
        channels = [linearized(c) / 100.0 for c in rgb]
        return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]

    a, b = luminance(first), luminance(second)
    lighter, darker = max(a, b), min(a, b)
    return (lighter + 0.05) / (darker + 0.05)


def emit(seed_hex: str) -> str:
    seed = argb_from_hex(seed_hex)
    source = hct_from_argb(seed)
    lines = [
        f"/* Monet TonalSpot 2025 · seed {hex_from_argb(seed)} "
        f"(H {source['hue']:.1f} · C {source['chroma']:.1f} · T {source['tone']:.1f}) */",
    ]
    for label, dark in (("light", False), ("dark", True)):
        roles = build_scheme(seed, dark)
        lines.append(f"\n/* {label} */")
        for name, value in roles.items():
            lines.append(f"    --{name}: {value};")
    return "\n".join(lines)


def tokens_in_css(path: Path) -> dict[str, list[dict[str, str]]]:
    """Collect the token maps of every ``color-scheme: light|dark`` block."""
    found: dict[str, list[dict[str, str]]] = {}
    scheme = None
    for line in path.read_text(encoding="utf-8").splitlines():
        text = line.strip()
        if text.startswith("color-scheme:"):
            scheme = text.split(":", 1)[1].strip(" ;")
            found.setdefault(scheme, []).append({})
            continue
        if not scheme or not text.startswith("--"):
            continue
        name, _, value = text.partition(":")
        value = value.strip().rstrip(";")
        if value.startswith("#"):
            found[scheme][-1][name[2:]] = value
    return found


def verify_css(path: Path, seed_hex: str) -> int:
    try:
        blocks = tokens_in_css(path)
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    seed = argb_from_hex(seed_hex)
    failures = 0
    for scheme, dark in (("light", False), ("dark", True)):
        expected = build_scheme(seed, dark)
        for tokens in blocks.get(scheme, []):
            failures += len(set(expected) - set(tokens))
            for name, value in expected.items():
                actual = tokens.get(name)
                if actual != value:
                    failures += 1
                    print(f"{scheme}: --{name} is {actual}, expected {value}")
    if failures:
        print(f"{failures} token mismatch(es) in {path}", file=sys.stderr)
        return 1
    print(f"{path}: tokens match the generated scheme")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", default="#6a49c8", help="seed colour (default: #6a49c8)")
    parser.add_argument("--check", action="store_true", help="print round-trip diagnostics")
    parser.add_argument(
        "--verify",
        nargs="?",
        const="module/webroot/assets/style.css",
        metavar="CSS",
        help="compare the tokens in a stylesheet against the generator",
    )
    args = parser.parse_args()

    if args.verify is not None:
        raise SystemExit(verify_css(Path(args.verify), args.seed))

    print(emit(args.seed))

    if args.check:
        seed = argb_from_hex(args.seed)
        for dark in (False, True):
            roles = build_scheme(seed, dark)
            print(f"\n# {'dark' if dark else 'light'}")
            for pair in (
                ("on-surface", "surface"),
                ("on-surface-variant", "surface"),
                ("on-primary", "primary"),
                ("on-primary-container", "primary-container"),
                ("primary", "surface"),
                ("outline", "surface"),
            ):
                print(f"  {pair[0]} / {pair[1]}: {contrast(roles[pair[0]], roles[pair[1]]):.2f}")
            for name in ("primary", "surface", "surface-container"):
                hct = hct_from_argb(argb_from_hex(roles[name]))
                print(f"  {name}: H {hct['hue']:.1f} C {hct['chroma']:.1f} T {hct['tone']:.1f}")


if __name__ == "__main__":
    main()
