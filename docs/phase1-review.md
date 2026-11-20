# WordQuest 姊妹项目：bambu_monitor 第一阶段（方案 A 只读监视）审核

> 状态：模拟器闭环完成，等待真机验证（设备未接 USB / 或未升级 0.2 基础固件）。
> 本阶段按 [调研方案](../../docs/bambu-monitor-research.md) 的方案 A 实施。

## 已完成

- 项目脚手架：`projects/bambu_monitor`（官方 hello_world 模板 + GSP 1.5.1）。
- 共享 C（设备/模拟器同源）：
  - `bmb_state` 状态模型（互斥锁保护，传输任务写、UI 读）。
  - `bmb_report` 手写 JSON 扫描器（无第三方依赖），识别 `gcode_state` /
    `mc_percent` / 温度 / 层数 / `subtask_name`，嵌套对象安全跳过。
  - `bmb_config` 配对信息（NVS namespace `bambu`）+ 读取 Vibe Mode 配置的
    WiFi 凭据（`sysmeta` 分区 namespace `wifi`）。
- 设备传输 `bmb_mqtt`：esp_wifi STA 自动重连 + esp-mqtt over TLS（8883，
  `bblp` + 访问码，自签证书跳过校验），通配订阅 `device/+/report`（免输序列号）。
- 模拟器传输 `pc/bmb_host_link`：脚本化回放（空闲 → 预热 → 打印 → 完成 →
  掉线 → 重连循环），FINISH 只在首轮演示一次。
- GSP 界面（126 对象，由 `tools/gen_ui.py` 生成）：
  - 状态页：顶栏（连接状态 + 重连 + 设置）、进度卡（大百分比 + 进度条）、
    任务卡（任务名 + 层数）、温度卡（喷嘴/热床/腔体）、离线横幅。
  - 完成弹窗（FINISH 跳变时出现 + 蜂鸣，可关闭）。
  - 设置页：IP / 访问码显示与修改、保存并连接、返回。
  - 软键盘：数字 + 小写字母 + 小数点 + 删除/清空/确定/取消，IP 格式校验。
- 完成提醒：BSP 蜂鸣器短音（320 ms 8 kHz）。

## 模拟器验证（截图在 docs/screens/）

| 流程 | 结果 | 截图 |
| --- | --- | --- |
| 回放驱动状态更新（在线 / 打印中 / 进度 / 任务 / 层数） | 通过 | 02/03 |
| FINISH 跳变弹窗 + 关闭 | 通过 | tmp-finish-* / 事件追踪 |
| 设置页打开 / 返回 | 通过 | 05 / 11 |
| 软键盘输入 IP（含小数点） | 通过 | 06 / 07 |
| 无效 IP 校验拒绝 | 通过（原生断言 + 界面路径） | 13/14（模拟器点击丢拍，原生覆盖） |
| 保存并连接 | 界面路径通过 | 10 |

## 原生回归

`pc/tests/test_bmb_report.c`：7 组断言（空闲、运行、完成标志、percent 回退、
无 print 对象忽略、PAUSE/ERROR、转义任务名），全部通过：

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 -I main \
    pc/tests/test_bmb_report.c main/bmb_report.c main/bmb_state.c -o t && ./t
```

## 已知问题 / 限制

1. 模拟器点击有随机丢拍（约 10–20%），自动化验证时用双击兜底；真机触摸不受影响。
2. 任务名包含语料外汉字会显示 `?`（字库是 GB2312 一级 3755 字 + ASCII）。
3. 尚未真机验证：WiFi 凭据读取、MQTT 连接、报告解析的真实数据、蜂鸣。
4. 尚未实现（第二阶段）：暂停/继续/停止控制、AMS 页、打印机开发者模式前提提示。

## 第二阶段计划

已完成，见 [phase2-review.md](phase2-review.md)。
