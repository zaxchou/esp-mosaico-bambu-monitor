"""Generate ui/main.json (the GSP scene) for bambu_monitor.

Pages are full-screen containers with a `visible` bind; children follow the
verified wordquest pattern. Font policy for the 480x480 rounded screen:

- Dynamic CJK text with a bounded vocabulary (status words, control status,
  temps, IP fields) declares an inline `font_charset` and can go up to 20px;
  gspc bakes exactly those glyphs, so bigger text stays affordable.
- Unbounded dynamic text (printer task names) stays 16px with the shared
  corpus file (charset_cjk.txt) — a corpus-level atlas above 16px overflowed
  the 1.5 MiB ui_apps partition in testing.
- Static text bakes only its own glyphs and may use display sizes freely.

Run:  python tools/gen_ui.py
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Palette: dark neutral with hairline borders and one warm accent.
BG = "#0B0F14"
PANEL = "#151C24"
PANEL_2 = "#1A232D"
BORDER = "#232E38"
TOPBAR = "#0D1218"
ACCENT = "#F5A524"
ACCENT_2 = "#C97E14"
CREAM = "#F2F6F3"
DIM = "#97A6B2"
GREEN = "#2FBF71"
GREEN_DARK = "#17342A"
RED = "#E05252"
RED_DARK = "#3A2320"
BTN = "#1C2632"
BTN_GREEN = "#1B3A2C"
DARK = "#101010"

CJK = "fonts/DengBmb.ttf"          # 16px corpus atlas (shared, unbounded text)
BIG = "fonts/DejaVuSans-Bold.ttf"  # digits-only display font

# Bounded vocabularies for dynamic labels (every glyph the setter can send).
CH_LINK = "在线连接中离 " + "".join(chr(code) for code in range(32, 127))
CH_STATE = "状态 空闲准备中切片打印已暂停完成出错未知"
CH_TEMP = "喷嘴热床腔体0123456789°/- "
CH_LAYER = "层剩余预计完成0123456789/-hm: "
# The reason text from rejected commands is printer-defined ASCII, so the
# whole letter/digit set is baked alongside the fixed phrases.
CH_CTRL = ("就绪发送中打印机已确认被拒绝，请检查开发者模式无响应失败已切换速度灯开关请检查格式未知原因连接断已取消 "
           + "".join(chr(code) for code in range(32, 127)))
CH_PCT = "0123456789%"
CH_HUMID = "湿度0123456789- "
CH_AMS_TEMP = "温度0123456789°- "
CH_REMAIN = "空0123456789%"
CH_SET_FIELD = "IP访问码0123456789. abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
CH_SET_STATUS = "未已保存，正在连接…失败IP 无效访问码不能为空"
CH_PAD_ERROR = "IP 无效访问码不能为空"
CH_PAD_TITLE = "输入打印机访问码 IP"
CH_PAD_FIELD = "0123456789.abcdefghijklmnopqrstuvwxyz"
CH_STOP = "停止打印再按一次确认"

objects: list[dict] = []


def add(obj: dict) -> dict:
    objects.append(obj)
    return obj


def page(name: str, hidden: bool, bg: str = BG) -> dict:
    return add({
        "type": "container", "parent": -1, "x": 0, "y": 0, "w": 480, "h": 480,
        "bg_color": bg, "hidden": hidden, "name": name,
        "bind": name, "bind_target": "visible",
    })


def child(parent: str, obj: dict) -> dict:
    obj["parent_name"] = parent
    return add(obj)


def label(parent: str, name: str, x: int, y: int, w: int, h: int, text: str,
          size: int = 18, color: str = CREAM, align: str = "left",
          font: str = CJK, bind_text: bool = False, charset: str | None = None):
    obj = {"type": "label", "x": x, "y": y, "w": w, "h": h, "text": text,
           "font_size": size, "fg_color": color, "text_align": align,
           "name": name}
    if font:
        obj["font"] = font
    if charset:
        obj["font_charset"] = charset
    elif font == CJK and size == 16:
        # Runtime-bound CJK text must declare the corpus, or gspc bakes only
        # printable ASCII and the text setter gets dropped.
        obj["font_charset_file"] = "charset_cjk.txt"
    if bind_text:
        obj["bind"] = name + "_t"
        obj["bind_target"] = "text"
    return child(parent, obj)


def button(parent: str, name: str, cb: str, x: int, y: int, w: int, h: int,
           text: str, color: str = BTN, border: str | None = BORDER,
           text_color: str = CREAM, size: int = 18, text_charset: str | None = None):
    btn = {"type": "button", "x": x, "y": y, "w": w, "h": h, "radius": 12,
           "bg_color": color, "callback": cb, "name": name}
    if border:
        btn["border_color"] = border
        btn["border_width"] = 1
    child(parent, btn)
    lbl = label(parent, name + "_lbl", x, y, w, h, text, size, text_color,
                "center", font=None, charset=text_charset)
    return lbl


def panel(parent: str, name: str, x: int, y: int, w: int, h: int,
          bg: str = PANEL, radius: int = 14, border: str | None = BORDER):
    return child(parent, {"type": "container", "x": x, "y": y, "w": w, "h": h,
                          "bg_color": bg, "radius": radius, "name": name,
                          **({"border_color": border, "border_width": 1} if border else {})})


# ---------------------------------------------------------------- status ----
page("status_page", hidden=False)

child("status_page", {"type": "container", "x": 0, "y": 0, "w": 480, "h": 56,
                      "bg_color": TOPBAR, "name": "top_bar",
                      "border_color": BORDER, "border_width": 1,
                      "border_side": "bottom"})
label("status_page", "link_lbl", 44, 14, 272, 28, "连接中", 18, CREAM,
      bind_text=True, charset=CH_LINK)
label("status_page", "model_lbl", 176, 14, 140, 28, "", 18, DIM,
      bind_text=True, charset=CH_LINK)
button("status_page", "set_btn", "set_open", 408, 8, 64, 40, "设置")
button("status_page", "link_rst_btn", "link_rst", 332, 8, 68, 40, "重连")

panel("status_page", "prog_card", 24, 76, 432, 168)
add({"type": "rect", "parent_name": "prog_card", "x": 0, "y": 0, "w": 432,
     "h": 168, "radius": 14, "name": "prog_bg", "bg_color": PANEL,
     "bg_gradient": "#101820", "gradient_dir": "vertical"})
label("prog_card", "state_lbl", 24, 14, 384, 30, "空闲", 20, ACCENT,
      bind_text=True, charset=CH_STATE)
label("prog_card", "pct_lbl", 24, 46, 384, 68, "0%", 56, CREAM, "center",
      font=BIG, bind_text=True, charset=CH_PCT)
add({"type": "progress", "parent_name": "prog_card", "x": 24, "y": 120,
     "w": 384, "h": 20, "min": 0, "max": 100, "value": 0, "name": "prog",
     "bind": "prog", "bind_target": "value", "fg_color": ACCENT,
     "bg_color": "#0E141B"})
label("prog_card", "prog_hint", 24, 144, 384, 24, "打印进度", 18, DIM,
      bind_text=True, charset=CH_CTRL + "打印进度:")

panel("status_page", "task_card", 24, 252, 432, 64, radius=8)
label("task_card", "task2_lbl", 20, 8, 392, 26, "暂无任务", 16, CREAM,
      bind_text=True)
label("task_card", "layer_lbl", 20, 34, 392, 24, "层 - / -", 18, DIM,
      bind_text=True, charset=CH_LAYER)

panel("status_page", "temp_card", 24, 324, 432, 72, radius=8)
label("temp_card", "nozzle_lbl", 14, 22, 132, 28, "喷嘴 --", 18, CREAM,
      bind_text=True, charset=CH_TEMP)
label("temp_card", "bed_lbl", 150, 22, 132, 28, "热床 --", 18, CREAM,
      bind_text=True, charset=CH_TEMP)
label("temp_card", "chamber_lbl", 286, 22, 132, 28, "腔体 --", 18, CREAM,
      bind_text=True, charset=CH_TEMP)

# Offline banner overlays the progress card: the nav row owns the bottom.
child("status_page", {"type": "container", "x": 24, "y": 76, "w": 432, "h": 48,
                      "bg_color": RED_DARK, "radius": 12, "name": "offline_bar",
                      "border_color": RED, "border_width": 1,
                      "bind": "offline_bar", "bind_target": "visible",
                      "hidden": True})
label("offline_bar", "offline_lbl", 16, 11, 400, 26, "打印机离线 · 重连中…",
      18, CREAM)

# Bottom nav is contextual: idle shows 打印控制, printing shows direct
# pause/resume + light + AMS. Two containers toggled from refresh().
child("status_page", {"type": "container", "x": 24, "y": 404, "w": 432, "h": 48,
                      "name": "idle_bar", "bind": "idle_bar",
                      "bind_target": "visible"})
button("idle_bar", "ctrl_open_btn", "ctrl_open", 0, 0, 208, 48, "打印控制",
       color=BTN_GREEN, border=GREEN, size=20)
button("idle_bar", "ams_open_btn", "ams_open", 224, 0, 208, 48, "料仓",
       size=20)

child("status_page", {"type": "container", "x": 24, "y": 404, "w": 432, "h": 48,
                      "name": "print_bar", "bind": "print_bar",
                      "bind_target": "visible", "hidden": True})
# 18px bounded charset: the label toggles 暂停/继续 at runtime.
button("print_bar", "qk_pause_btn", "qk_pause", 0, 0, 102, 48, "暂停",
       color=BTN_GREEN, border=GREEN, size=18,
       text_charset="暂停继续")
qk_lbl = objects[-1]
qk_lbl["bind"] = "qk_pause_btn_lbl_t"
qk_lbl["bind_target"] = "text"
button("print_bar", "qk_light_btn", "qk_light", 110, 0, 102, 48, "灯 开",
       size=18, text_charset="灯 开关")
qk_light_lbl = objects[-1]
qk_light_lbl["bind"] = "qk_light_btn_lbl_t"
qk_light_lbl["bind_target"] = "text"
button("print_bar", "qk_ams_btn", "ams_open", 220, 0, 102, 48, "料仓",
       size=18)
button("print_bar", "qk_ctrl_btn", "ctrl_open", 330, 0, 102, 48, "控制",
       size=18)

# ---------------------------------------------------------------- finish ----
page("finish_page", hidden=True, bg="#0A0E12")
panel("finish_page", "finish_card", 84, 140, 312, 200, radius=12, border=ACCENT)
label("finish_card", "finish_title", 24, 28, 264, 40, "打印完成", 32, ACCENT,
      "center", bind_text=True, charset="打印完成出错")
label("finish_card", "finish_task", 24, 76, 264, 26, "", 16, CREAM,
      bind_text=True)
label("finish_card", "finish_sub", 24, 108, 264, 24, "可以取件了", 18, DIM,
      bind_text=True,
      charset="可以取件了请检查打印机错误码0123456789ABCDEFx ")
button("finish_card", "finish_ok", "finish_ok", 66, 140, 180, 44, "好的",
       color=ACCENT, border=None, text_color=DARK, size=20)

# ------------------------------------------------------------- control ----
page("ctrl_page", hidden=True)
label("ctrl_page", "ctrl_title", 24, 24, 432, 40, "打印控制", 32, CREAM,
      "center", charset="打印控制")
label("ctrl_page", "ctrl_hint", 24, 80, 432, 26,
      "需在打印机开启局域网控制（开发者模式）", 18, DIM)
label("ctrl_page", "ctrl_status_lbl", 24, 116, 432, 28, "就绪", 20, ACCENT,
      bind_text=True, charset=CH_CTRL)
button("ctrl_page", "ctrl_pause_btn", "ctrl_pause", 24, 148, 208, 52, "暂停",
       color=BTN_GREEN, border=GREEN, size=20)
button("ctrl_page", "ctrl_resume_btn", "ctrl_resume", 248, 148, 208, 52, "继续",
       color=BTN_GREEN, border=GREEN, size=20)
# 18px with a bounded charset: the confirm text is runtime-bound, and a
# corpus-level atlas at this size would overflow the ui_apps partition.
button("ctrl_page", "ctrl_stop_btn", "ctrl_stop", 24, 208, 432, 52, "停止打印",
       color=RED_DARK, border=RED, size=18, text_charset=CH_STOP)
stop_lbl = objects[-1]
stop_lbl["bind"] = "ctrl_stop_btn_lbl_t"
stop_lbl["bind_target"] = "text"

# Quick controls: print speed presets and the chamber light.
SPEEDS = [("静音", 1), ("标准", 2), ("运动", 3)]
for i, (text, lvl) in enumerate(SPEEDS):
    button("ctrl_page", "ctrl_spd_btn_" + str(lvl), "ctrl_spd" + str(lvl),
           24 + i * 148, 272, 140, 48, text, size=18)
button("ctrl_page", "ctrl_light_btn", "ctrl_light", 24, 328, 208, 44, "灯 开",
       size=18, text_charset="灯 开关")
light_lbl = objects[-1]
light_lbl["bind"] = "ctrl_light_btn_lbl_t"
light_lbl["bind_target"] = "text"
button("ctrl_page", "ctrl_back_btn", "ctrl_back", 248, 328, 208, 44, "返回",
       size=20)


# ------------------------------------------------------------------ ams ----
page("ams_page", hidden=True)
label("ams_page", "ams_title", 24, 24, 432, 40, "料仓", 32, CREAM,
      "center", charset="料仓")
panel("ams_page", "ams_env_card", 24, 88, 432, 64, radius=8)
label("ams_env_card", "ams_humidity_lbl", 14, 22, 200, 28, "湿度 --", 18,
      CREAM, bind_text=True, charset=CH_HUMID)
label("ams_env_card", "ams_temp_lbl", 220, 22, 198, 28, "温度 --", 18,
      CREAM, bind_text=True, charset=CH_AMS_TEMP)
# 8 slots = 2 AMS units x 4 trays, 2 rows of 4. Each card: slot id, filament
# color swatch, type and remain.
SLOT_ROWS = [176, 278]
for slot in range(8):
    row, col = divmod(slot, 4)
    card = "ams_tray_card_" + str(slot)
    panel("ams_page", card, 24 + col * 108, SLOT_ROWS[row], 100, 94, radius=14)
    label(card, "ams_slot_lbl_" + str(slot), 0, 6, 100, 24,
          "槽 " + str(slot + 1), 16, DIM, "center")
    # Filament color swatch: bg_color is bound at runtime from tray_color.
    child(card, {"type": "rect", "x": 20, "y": 34, "w": 60, "h": 12,
                 "radius": 6, "bg_color": "#2A3743",
                 "name": "ams_swatch_" + str(slot),
                 "bind": "ams_swatch_" + str(slot), "bind_target": "color"})
    label(card, "ams_type_lbl_" + str(slot), 0, 50, 100, 22, "", 16, DIM,
          "center", bind_text=True)
    label(card, "ams_remain_lbl_" + str(slot), 0, 70, 100, 22, "--", 16,
          CREAM, "center", bind_text=True, charset=CH_REMAIN)
button("ams_page", "ams_back_btn", "ams_back", 24, 392, 432, 44, "返回",
       size=20)

# -------------------------------------------------------------- settings ----
page("set_page", hidden=True)
label("set_page", "set_title", 24, 24, 432, 40, "打印机配对", 32, CREAM,
      "center", charset="打印机配对")
label("set_page", "set_ip_lbl", 24, 90, 432, 30, "IP  --", 18, CREAM,
      bind_text=True, charset=CH_SET_FIELD)
button("set_page", "set_ip_btn", "set_ip", 24, 126, 204, 46, "修改 IP", size=20)
label("set_page", "set_code_lbl", 24, 188, 432, 30, "访问码  --", 18, CREAM,
      bind_text=True, charset=CH_SET_FIELD)
button("set_page", "set_code_btn", "set_code", 24, 224, 204, 46, "修改访问码",
       size=20)
button("set_page", "set_save_btn", "set_save", 24, 282, 204, 46, "保存并连接",
       color=BTN_GREEN, border=GREEN, size=20)
label("set_page", "set_status_lbl", 24, 342, 432, 26, "未保存", 18, DIM,
      bind_text=True, charset=CH_SET_STATUS)
button("set_page", "set_back_btn", "set_back", 24, 392, 432, 44, "返回",
       size=20)

# ---------------------------------------------------------------- keypad ----
page("pad_page", hidden=True)
label("pad_page", "pad_title", 24, 12, 432, 34, "输入", 20, CREAM, "center",
      bind_text=True, charset=CH_PAD_TITLE)
child("pad_page", {"type": "container", "x": 24, "y": 56, "w": 432, "h": 64,
                   "bg_color": PANEL, "radius": 8, "name": "pad_box",
                   "border_color": BORDER, "border_width": 2})
label("pad_box", "pad_field", 16, 16, 400, 32, "", 28, CREAM, bind_text=True,
      charset=CH_PAD_FIELD)
label("pad_page", "pad_error", 24, 124, 432, 24, "", 20, "#E08A7A",
      bind_text=True, charset=CH_PAD_ERROR)

KEY_ROWS = [
    ["1", "2", "3", "4", "5", "6"],
    ["7", "8", "9", "0", "dot", "del"],
    ["a", "b", "c", "d", "e", "f"],
    ["g", "h", "i", "j", "k", "l"],
    ["m", "n", "o", "p", "q", "r"],
    ["s", "t", "u", "v", "w", "x"],
    ["y", "z", "clr", "ok", "cancel"],
]
KEY_LABELS = {"dot": ".", "del": "删除", "clr": "清空", "ok": "确定",
              "cancel": "取消"}
KEY_COLORS = {"del": "#3A2A22", "clr": "#3A2A22", "ok": "#22402F",
              "cancel": "#3A3030"}
x0, y0, kw, kh, gx, gy = 27, 152, 66, 40, 6, 4
for row_i, row in enumerate(KEY_ROWS):
    for col_i, key in enumerate(row):
        x = x0 + col_i * (kw + gx)
        y = y0 + row_i * (kh + gy)
        cb = "pad_" + key
        text = KEY_LABELS.get(key, key)
        button("pad_page", "pad_k_" + key, cb, x, y, kw, kh, text,
               color=KEY_COLORS.get(key, "#22303C"), border=BORDER, size=20)

scene = {
    "screen": "bmb",
    "w": 480,
    "h": 480,
    "screen_bg": BG,
    "font": "fonts/DengBmb.ttf",
    "default_font_size": 18,
    "swipe": False,
    "objects": objects,
}

out = ROOT / "ui/main.json"
out.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n",
               encoding="utf-8")
print("scene written:", out, len(objects), "objects")
