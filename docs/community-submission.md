# 社区投稿：文件清单与上传步骤

> 2026-11-21 状态：投稿素材与介绍已就绪；**固件附件必须使用 0.2 格式包**（社区已拒绝 `esp-iris-system-update/v1`），0.2 重建进行中，完成后本文件的固件一节会补上最终哈希。源码默认配对值已清空，不含任何真实访问码；截图中的打印机序列号已遮盖。

## 1. 元数据与介绍

| 文件 | 用途 |
| --- | --- |
| `mosaico-ideas.json` | 上传元数据（标题、分类、标签、许可、封面、changelog） |
| `README.md` | 详情页富文本（功能、操作、限制） |
| `docs/community-promo-prompts.md` | 封面/宣传图的 AI 提示词（记录生成方式） |
| `docs/USAGE.md` | 使用教程（GitHub 版；社区详情页可摘录） |
| `docs/submission/cover.png` | 方形封面（成品） |
| `docs/submission/promo-dashboard.png` | 详情图：实时进度（成品） |
| `docs/submission/promo-ams.png` | 详情图：料仓余量（成品） |
| `docs/submission/promo-control.png` | 详情图：触控操作（成品） |
| `docs/submission/promo-alert.png` | 详情图：完成/出错提醒（成品） |
| `docs/submission/promo-banner.png` | 横版头图（成品） |
| `docs/submission/xiaohongshu-cover.png` | 小红书竖版封面 1080×1440（站外宣传用） |
| `docs/submission/01…07*.png` | 界面截图（真机 1 + 模拟器 6，被成品图引用，也可作补充详情图） |

封面与详情图已合成完毕：拓竹官网 H2D / AMS 2 Pro 产品图（图内已标注来源）+ 嵌入遮盖序列号后的真机/模拟器截图。

标题：**Bambu Monitor · 拓竹打印机监视屏**
分类：`display`；标签：3D打印、拓竹、Bambu Lab、H2D、AMS、监视屏、MQTT、局域网、进度提醒、ESP-GSP

### 简介（可直接粘贴，三种长度）

- **一句话（约 40 字）**：给拓竹打印机加一块 480×480 的状态屏：进度、层数、温度、剩余时间一眼看完，完成和出错自动提醒。
- **短简介（约 70 字）**：Bambu Monitor 通过局域网直连拓竹打印机：实时进度、层数、喷嘴/热床温度、剩余时间与预计完成时刻，AMS 料仓颜色材质与余量，完成/出错蜂鸣提醒，还能直接暂停、调灯、控速，全程不依赖云端。
- **中简介（约 130 字）**：把 ESP-Mosaico 的 480×480 触摸屏变成拓竹打印机的监视屏。首次使用在设置页输入打印机的局域网 IP 和访问码即可配对（存 NVS 断电不丢）。打印中显示任务名、进度、层数、实时温度、剩余时间和预计完成时刻；打印完成或出错时全屏提醒并蜂鸣，出错带完整 32 位错误码；AMS 料仓显示每个槽位的颜色、材质与余量。打印中底部出现快捷条：暂停/继续、腔室灯开关、料仓、控制页；控制页支持三档速度、双灯控制和带二次确认的停止（绑定设备/任务/会话，任务变了确认自动作废）。离线自动重连，时钟经 SNTP 同步为本地时间。

## 2. 功能亮点（详情页可展开）

- **状态一览**：任务名、百分比大字与进度条、层数、喷嘴/热床目标与实时温度、剩余时间倒计时与"预计 HH:MM"完成时刻（SNTP 校时，分钟级 ETA 每次上报自动校准）。
- **AMS 料仓**：双单元 8 槽的颜色色块、材质名（含 PLA-AERO/ASA-AERO 等长名称）、余量百分比；湿度与温度。
- **提醒不漏**：完成/出错全屏弹窗 + 蜂鸣；出错码完整显示；重连/换任务后旧提醒自动作废，不会误确认新会话的提醒。
- **打印中快捷控制**：暂停/继续、灯开/关、料仓、控制页；停止需 5 秒内二次确认，且确认绑定当前任务——任务切换后不会误停新任务。
- **健壮连接**：局域网 MQTT 直连（TLS 加密）；断线后按 5 秒→300 秒指数退避自动重建；60 秒一条连接统计日志便于诊断。
- **工程可靠性**：有界 JSON 解析（64 KiB/32 层，畸形报文不崩溃）、分片重组、命令 8 秒超时与逐条 ACK 匹配；75 组原生回归 + 29 组 UI 故障注入 + 106 项模拟器交互回归。

## 3. 截图

| 文件 | 内容 | 来源 |
| --- | --- | --- |
| `01-status-real-printing.png` | 真机打印中：进度/温度/ETA/快捷条 | 真机（序列号已遮盖） |
| `02-status-running-controls.png` | 打印中状态页四快捷入口 | 模拟器 |
| `03-ams-materials.png` | AMS 料仓长材质名与双单元 | 模拟器 |
| `04-control-page.png` | 控制页：暂停/速度/灯/停止确认 | 模拟器 |
| `05-keypad-large-font.png` | 大字号配对键盘与输入反馈 | 模拟器 |
| `06-error-alert.png` | 出错弹窗（32 位错误码） | 模拟器 |
| `07-paused.png` | 已暂停状态 | 模拟器 |

模拟器与真机使用同一套 C 界面代码与烘焙资源，渲染一致。

## 4. 固件附件

**0.2 格式包已构建并通过官方校验**（schema `esp-iris-system-update/0.2`、契约 `esp-mosaico/v2`、Recovery ABI 2、最低 Vibe Mode `0.2.0`）：

- 文件：`firmware/bambu_monitor-v0.2-system-update.irisfw`
- 大小：1,950,862 字节
- SHA-256：`ee7b2030bdfcbc565dc1c5777668a70d51514ebdb6aed47b40e11f23cb94bc5d`
- 组件：分区表 4,096 B @ 0x8000；应用 2,000,016 B @ 0x21000（`main_app`）；`ui_apps.bin` 1,333,200 B @ 0xE80000；不含 bootloader / Vibe Mode 固件 / NVS 镜像
- 校验：Ideas `validate_firmware()` **PASS**；官方 `esp_iris.py bundle --json inspect` **PASS**（对交付文件复验 PASS）
- 构建环境：utils `be151ae04701…`（0.2 工具）、BSP 0.2.0、IDF `7b9cc1ac79f8…`、GSPC 0.7.0；完整证据与复现脚本见 [release-v02](release-v02/README.md)
- 原生回归 4/4 组通过；独立副本内源码经零访问码扫描

当前开发工作区的 v1 包（`b0a6b2f0…`）仅用于本机 0.1 设备日常使用，不能作为社区附件。

## 5. 提交前检查

- [x] 源码不含真实访问码（`bmb_config.h` 默认值已清空，全新刷机需在设置页配对）
- [x] 截图无打印机序列号
- [x] `mosaico-ideas.json` 元数据、封面与详情图就绪
- [x] 0.2 固件包构建与官方校验（validate_firmware / bundle inspect 均 PASS）
- [x] `LICENSE`（MIT）与仓库内容推送至 GitHub
- [ ] 用户手动完成社区上传（本机从未登录/上传过社区服务）

## 6. GitHub 仓库建议

- 展示名：**Bambu Monitor for ESP-Mosaico**（中文：拓竹打印机监视屏）
- 仓库地址名（推荐）：`esp-mosaico-bambu-monitor`
- 备选：`mosaico-bambu-monitor`、`bambu-status-screen`
- 建议以 `projects/bambu_monitor` 内容为仓库根（`main/ pc/ ui/ tools/ docs/ README.md LICENSE mosaico-ideas.json`），README 说明它属于 ESP-Mosaico 应用、需在 ESP-Mosaico 工作区内构建，并链接社区固件包。

## 7. 已知限制（如实写明）

- 已在拓竹 **H2D + AMS Pro** 上实测；其他型号（P1/A1/X1 系列）未验证，状态字段以打印机上报为准。
- 需要打印机开启**开发者模式/局域网服务**并提供访问码；「仅局域网模式」开关每次切换都会重置访问码，需要重新配对。
- 局域网 TLS 加密传输，但当前跳过打印机自签名证书校验（无法离线预置其证书）。
- 命令发送等待下一份完整状态报文（实测 H2D 约 1.3 秒）；无报文时 8 秒超时。
- H2D 状态报文暂不包含腔体温度，界面显示「--」。
- `FAILED` 等异常状态映射为错误弹窗；打印中的真实停止/暂停操作请在自己的打印机上确认后再依赖。
