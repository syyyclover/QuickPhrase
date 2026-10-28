"""Package the committed image-model artwork as multi-resolution Windows icons."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

def package(name):
    source = ROOT / "res/icons" / (name + "-master.png")
    master = Image.open(source).convert("RGBA")
    alpha = master.getchannel("A")
    if alpha.getextrema()[0] != 0 or alpha.getbbox() is None:
        raise ValueError(f"{source}: transparent master required")
    # Normalize transparent padding while retaining the original AI artwork.
    art = master.crop(alpha.getbbox())
    side = max(art.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.alpha_composite(art, ((side - art.width) // 2, (side - art.height) // 2))
    frames = []
    for size in SIZES:
        inset = max(1, round(size * 0.09))
        frame = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        frame.alpha_composite(canvas.resize((size - inset * 2, size - inset * 2), Image.Resampling.LANCZOS), (inset, inset))
        frames.append(frame)
    target = ROOT / "res" / (name + ".ico")
    frames[-1].save(target, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    with Image.open(target) as check:
        if check.ico.sizes() != {(s, s) for s in SIZES}:
            raise ValueError(f"{target}: missing icon resolutions")
    print(f"{target.relative_to(ROOT)}: {len(SIZES)} resolutions")

if __name__ == "__main__":
    for name in ("app", "setup"):
        package(name)
