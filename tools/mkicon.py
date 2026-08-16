# -*- coding: utf-8 -*-
"""生成应用图标：蓝灰圆角方块 + 白色对话气泡（内含三条文本线）"""
from PIL import Image, ImageDraw

BG_TOP    = (74, 116, 165)
BG_BOTTOM = (46, 74, 110)
FG        = (255, 255, 255)
ACCENT    = (168, 202, 240)

S = 1024  # 超采样画布


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def draw_base():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (1, S))
    gp = grad.load()
    for y in range(S):
        t = y / (S - 1)
        gp[0, y] = (
            int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t),
            int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t),
            int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t),
            255,
        )
    grad = grad.resize((S, S))
    img.paste(grad, (0, 0), rounded_mask(S, int(S * 0.19)))
    return img


def draw_bubble(img, simple=False):
    d = ImageDraw.Draw(img)
    L, T, R, B = int(S * 0.17), int(S * 0.21), int(S * 0.83), int(S * 0.65)
    d.rounded_rectangle([L, T, R, B], radius=int(S * 0.10), fill=FG)
    d.polygon([(int(S * 0.30), B - int(S * 0.02)),
               (int(S * 0.30), int(S * 0.82)),
               (int(S * 0.50), B - int(S * 0.02))], fill=FG)
    if simple:
        return
    lh = int(S * 0.072)
    x0 = L + int(S * 0.09)
    ys = [T + int(S * 0.10), T + int(S * 0.10) + int(S * 0.125),
          T + int(S * 0.10) + int(S * 0.25)]
    widths = [0.54, 0.44, 0.30]
    for i, (y, w) in enumerate(zip(ys, widths)):
        d.rounded_rectangle([x0, y, x0 + int(S * w), y + lh],
                            radius=lh // 2,
                            fill=BG_BOTTOM if i < 2 else ACCENT)


def render(size):
    img = draw_base()
    draw_bubble(img, simple=(size <= 20))
    return img.resize((size, size), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [render(s) for s in sizes]
imgs[-1].save("/root/QuickPhrase/res/app.ico", format="ICO",
              sizes=[(s, s) for s in sizes])
render(256).save("/root/QuickPhrase/res/app.png")

# 安装程序图标：同底色，白色向下箭头 + 底座
inst = draw_base()
d = ImageDraw.Draw(inst)
cx = S // 2
d.rounded_rectangle([cx - int(S * 0.075), int(S * 0.20),
                     cx + int(S * 0.075), int(S * 0.56)],
                    radius=int(S * 0.03), fill=FG)
d.polygon([(cx - int(S * 0.20), int(S * 0.52)),
           (cx + int(S * 0.20), int(S * 0.52)),
           (cx, int(S * 0.76))], fill=FG)
d.rounded_rectangle([int(S * 0.22), int(S * 0.80),
                     int(S * 0.78), int(S * 0.86)],
                    radius=int(S * 0.03), fill=ACCENT)
inst.save("/root/QuickPhrase/res/setup.ico", format="ICO",
          sizes=[(s, s) for s in sizes])
print("ok")
