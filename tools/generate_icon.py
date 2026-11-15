"""Vector source for dui's DOS terminal."""

import sys
from pathlib import Path

sys.dont_write_bytecode = True
from iconlib import Canvas, generate

ROOT = Path(__file__).resolve().parents[1]


def draw(canvas: Canvas) -> None:
    """A compact CRT with a green DOS prompt and cursor."""
    frame, dark, green = (154, 174, 193), (19, 34, 49), (103, 235, 143)
    canvas.polygon(((10, 12), (90, 12), (96, 18), (96, 74), (90, 80),
                    (10, 80), (4, 74), (4, 18)), frame)
    canvas.rect(12, 21, 76, 48, dark)
    canvas.line(((24, 34), (35, 44), (24, 54)), 5, green)
    canvas.line(((46, 54), (64, 54)), 5, green)
    canvas.circle(85, 75, 2, (255, 200, 74))
    canvas.rect(43, 80, 14, 8, frame)
    canvas.rect(27, 88, 46, 6, frame)


if __name__ == "__main__":
    raise SystemExit(generate(draw, ROOT / "app/res/dui.ico"))
