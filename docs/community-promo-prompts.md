# Bambu Monitor 社区素材：封面与宣传图提示词

投稿需要 1 张封面 + 若干详情图。`docs/submission/` 已放 7 张可直接用的界面截图（1 张真机打印中 + 6 张模拟器界面，与真机同资源同渲染器）。封面建议用 AI 生成场景图后，把真机截图或界面元素叠上去。

**重要：不要让模型画文字**（中文和界面文字几乎必崩），标题和标注最后自己叠加。

---

## 0. 统一风格锚（每条提示词都带上）

```
Dark modern industrial product render, deep charcoal background #0B0F14, matte dark panels #151C24 with subtle hairline borders #232E38, warm amber accent #F5A524 as the only strong color, soft mint green #2FBF71 secondary, cream white #F2F6F3 text panels. Clean isometric / 2.5D composition, soft studio lighting with one warm key light, shallow depth of field on background, crisp edges, high detail. No text anywhere.
```

中文版（支持中文提示词的模型）：

```
深色现代工业产品渲染：近黑背景 #0B0F14、哑光深灰面板 #151C24 带细边框 #232E38，唯一的强色是暖琥珀橙 #F5A524，辅助色薄荷绿 #2FBF71、奶白 #F2F6F3。等距 2.5D 构图，柔和棚灯加一盏暖主光，背景浅景深，边缘锐利、细节丰富。画面中不要出现任何文字。
```

负面提示（所有图通用）：

```
text, letters, numbers, brand logo, Bambu Lab logo, watermark, pixel art, anime, low contrast, muddy colors, oversaturated, blurry, jpeg artifacts, cluttered composition
```

---

## 1. 封面（方图 1080×1080，最重要）

**方向 A：打印机旁的状态屏（产品感最强，推荐）**

```
Square 1:1 dark product render. A small square smart display with a rounded 480x480-like screen stands on a workbench beside a modern enclosed 3D printer (generic rectangular frame, glass door, no brand marks). The screen shows an abstract dark dashboard: a large circular progress ring in warm amber, small stat panels with bars and gauges, tiny color swatches grid. The printer's interior glows softly. Warm amber light spills from the screen onto the desk. Upper third of the image stays dark and calm for a title overlay. Crisp, high detail, no text.
```

**方向 B：打印进行时特写（有故事感）**

```
Square 1:1 dark product render, close-up angle: in the foreground a square smart display tilted slightly toward the viewer, its dark dashboard glowing with an amber progress ring and green temperature panels; behind it, out of focus, a 3D printer mid-print with a small filament pattern emerging on the build plate, warm interior light. A few filament spools in muted colors (orange, teal, gray) on the shelf behind. Strong contrast between the glowing screen and the dark room. No text.
```

**方向 C：夜灯氛围（情绪感）**

```
Square 1:1 dark scene: a maker's desk at night, the only light sources are a small square smart display glowing amber and the faint blue light inside a 3D printer across the desk. The display shows an abstract dark dashboard with a progress ring and alert icon glowing green. Dust motes in the light beam, cozy dark workshop mood, cinematic. No text.
```

> 叠字建议：标题「Bambu Monitor」用仓库字体 `ui/fonts/DejaVuSans-Bold.ttf`，副标题「拓竹打印机监视屏」用 `ui/fonts/DengBmb.ttf`（等线），主色 #F5A524，2px 深色 #0B0F14 描边，放在画面上方的深色区。

---

## 2. 横版头图（1600×900，项目页顶部）

```
Wide 16:9 dark product banner. Left 40%: calm dark space reserved for a title block, subtle amber rim light on the edge. Right 60%: the square smart display on a workbench showing the abstract dark dashboard (amber ring, green panels, filament color swatches), a generic modern 3D printer behind it with glowing interior, two filament spools nearby. Warm key light from the screen, soft shadows, cinematic depth. Crisp, no text.
```

---

## 3. 详情图（1080×1080，配合功能介绍）

**3.1 料仓 / AMS（颜色丰富，适合展示）**

```
Dark product render detail shot: a multi-slot filament system (AMS-like unit with 4 visible spool slots) beside the square smart display; the display shows an abstract grid of eight color swatch cards with small percentage bars (amber, teal, orange, gray tones). Colorful filament spools echo the swatch colors. Isometric angle, soft studio light. No text.
```

**3.2 控制面板（功能感）**

```
Dark render detail: the square smart display close-up showing an abstract touch control layout — a large green circular button, three small speed preset pills with the middle one highlighted amber, a light-bulb toggle glowing warm, and a red stop button in the corner. Clean flat panels with hairline borders. No readable text, iconography only.
```

**3.3 完成/出错提醒（情绪钩子）**

```
Dark scene: the square smart display on a desk at night showing an abstract alert card — a warm amber banner with a check icon glowing green, subtle sound-wave arcs radiating from a small speaker grille at the device edge, suggesting a beep notification. Behind, a finished 3D print (generic geometric vase) on the printer bed. Cozy, calm, no text.
```

---

## 4. 尺寸与提交

- 封面 1080×1080（社区主图）；横版头图 1600×900；详情图 1080×1080。
- 叠字后导出 PNG，文件放到 `docs/submission/`：`cover.png`、`promo-dashboard.png`、`promo-ams.png`、`promo-alert.png`，与 `mosaico-ideas.json` 的 covers 列表对应。
- AI 图只做背景/氛围，界面截图（`docs/submission/01…07`）可以直接作为详情图上传，无需重绘。
