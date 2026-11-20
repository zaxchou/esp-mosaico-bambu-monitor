# bambu_monitor 第二阶段（打印控制 + AMS）审核

> 历史里程碑记录：本文的 USB 可用性、槽数和测试计数描述对应最初第二阶段。当前功能和最新验证见 [Bambu Monitor：当前功能与验证](../../../docs/bambu-monitor.md)。第一阶段记录见 [phase1-review.md](phase1-review.md)。

## 新增功能

### 打印控制页

- 状态页底部新增「打印控制」「料仓」两个入口按钮（离线横幅改为覆盖进度卡）。
- 控制页：暂停 / 继续 / 停止三个命令按钮 + 开发者模式前提提示
  （"需在打印机开启局域网控制（开发者模式）"）。
- 命令路径：`bmb_ctrl_send` → `device/<序列号>/request`
  `{"print":{"command":"pause|resume|stop","sequence_id":"N"}}`，
  序列号自动递增；离线 / 忙碌时拒绝发送。
- 回执：解析 report 中的 `print_ack` / `command` / `result`，显示
  "打印机已确认" 或 "被拒绝，请检查开发者模式"。
- 停止为破坏性动作：第一次按下只进入确认状态（按钮文案变为
  "再按一次确认停止"），5 秒窗口内再次按下才发送；窗口过后自动复位。
- 8 秒无回执超时显示 "无响应，请检查打印机"。

### AMS 料仓页

- 解析 `print.ams`：直接字段（X1 风格）与多单元 `ams[]` 数组两种布局，
  湿度 / 温度 / `tray[].remain`（-1 表示空槽，与打印机约定一致）。
- 界面：湿度 / 温度卡 + 4 个料槽卡（余量百分比或"空"）+ 说明行。
- 无 `ams` 对象的报文不清空快照（保持上次读数）。

## 模拟器验证（截图 docs/screens/20–23）

| 流程 | 结果 | 截图 |
| --- | --- | --- |
| 状态页导航按钮 / 离线横幅避让 | 通过 | 20 |
| 打印控制 → 暂停 → 回执"打印机已确认" | 通过 | 22（trace: action=4, ctrl-sent） |
| 停止二次确认文案切换 | 通过 | 21 |
| AMS 页湿度 / 温度 / 料槽余量与"空" | 通过 | 23 |
| 事件路由（on_event 解码 action 记录） | 通过 | bmb_evt.txt trace |

## 原生回归

`pc/tests/test_bmb_report.c` 扩到 11 组断言（新增 ams-direct、
ams-multi-unit、ctrl-ack），全部通过。执行命令同第一阶段。

## 过程中发现并修复的问题

1. **JSON 扫描器数组迭代死循环**：skip_value 对 `,` 不前进，手写数组循环
   必须显式跳过逗号；walk/find 消耗游标后复用 span 前要重新锚定。
2. **GSP 动态文本 charset**：按钮子标签经 `button()` 生成时 font=None，
   不走 16px 自动 charset 规则；运行时 set_text 中文显示 `?`。需显式
   `font_charset_file`。动态 CJK 标签必须固定 16px——18px 全量字库
   烘焙会把 ui_apps 顶爆（实测 2.59 MB > 1.5 MiB 分区）。
3. **host 回放 ack 用绝对时间戳**：跨回放 loop 边界会延迟或丢失；改为
   倒计时。
4. **stale sim 进程**：修改场景后旧 bambu_backend.exe 仍持有端口和旧
   场景，验证前必须清理（再次踩中）。

## 已知限制

1. 模拟器帧时钟慢于墙钟，回放时间戳被拉长（host-only，真机 FreeRTOS
   timer 为准）；观察到的 "ack 延迟" 是该时钟差，不是逻辑错误。
2. AMS 多单元时湿度/温度取首个见到的单元值；槽位按单元顺序合并，仅
   支持总槽位数 ≤ 4 的显示。
3. 控制命令依赖打印机开启局域网控制（开发者模式）；未开启时表现取决于
   打印机固件（可能静默丢弃 → 8 秒超时提示）。
4. 尚未真机验证：WiFi/MQTT 真实链路、真实 report 解析、控制命令端到端、
   AMS 真实数据、蜂鸣。

## 真机部署状态

- 设备固件 `idf.py build` 通过（bundle 1.2 MiB / 1.5 MiB 分区）。
- **设备 USB 未枚举**：Windows 无任何 VID_303A 设备（ESP-Iris CDC 需要
  VID_303A:4002 "USB Highspeed"）；12:00 前后无新 USB 枚举事件。上次成功
  连接记录为 usb:location=1-6:x.0（2026-11-01，wordquest）。
- 待办：设备接入后走 `iris system-update` 安装并按 phase1 清单做真机验证。
