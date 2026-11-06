# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow", "numpy"]
# ///
import math
from pathlib import Path

import numpy as np
from PIL import Image

ASSETS = Path(__file__).parent.parent / "assets"
OVERSAMPLE = 3


def sphere(radius, phase, lines=64):
    size = radius * 2 * OVERSAMPLE
    axis = (np.arange(size) + 0.5) / (size / 2) - 1
    x, y = np.meshgrid(axis, axis)
    r2 = x * x + y * y
    z = np.sqrt(np.clip(1 - r2, 0, 1))
    angle = phase * math.pi
    light = np.array([math.sin(angle) * 0.95, -0.28, math.cos(angle)])
    light /= np.linalg.norm(light)
    lit = (x * light[0] + y * light[1] + z * light[2]) / np.sqrt(x * x + y * y + z * z + 1e-6)
    tone = np.clip(lit * 1.25 + 0.03, 0, 1) ** 0.75
    latitude = np.arcsin(np.clip(y, -1, 1)) + 0.16 * x * x
    stripe = np.abs((latitude / math.pi * lines) % 1 - 0.5) * 2
    coverage = np.clip((tone * 0.98 - stripe) * (size / lines / 2.4) + 0.5, 0, 1)
    rim = np.clip((r2 - 0.988) * 90, 0, 1) * 0.4
    rgba = np.zeros((size, size, 4), np.uint8)
    rgba[..., :3] = 255
    rgba[..., 3] = (np.where(r2 < 1, np.maximum(coverage, rim), 0) * 255).astype(np.uint8)
    return Image.fromarray(rgba, "RGBA").resize((radius * 2, radius * 2), Image.LANCZOS)


sphere(180, 0.6).save(ASSETS / "orb-idle.png")
sphere(150, 0.08).save(ASSETS / "orb-listening.png")
sphere(46, 0.45).save(ASSETS / "orb-small.png")
