# Bambu Monitor 社区 0.2 发布包

社区上传请选择根工作区 `firmware/bambu_monitor-v0.2-system-update.irisfw`。

| 项目 | 值 |
| --- | --- |
| 文件大小 | 1,950,862 字节 |
| SHA-256 | `ee7b2030bdfcbc565dc1c5777668a70d51514ebdb6aed47b40e11f23cb94bc5d` |
| schema | `esp-iris-system-update/0.2` |
| release | `1.0.0` |
| 最低 Vibe Mode | `0.2.0` |
| 契约 | `esp-mosaico/v2` / `mosaico-retained-test-2m-v2` / ABI 2 |
| utils revision | `be151ae047014e79a9c7ae0092dd203844499003` |
| ESP-IDF revision | `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` |
| BSP revision | `a2c985ad698b3f04a21c63e006b43e7e948bc1d7` + `evidence/bsp-existing-key-fix.patch` |

独立构建源：`C:/Users/zeroz/esp/work/bambu-v02-release`。原 0.1 开发工作区、0.1 设备与所有在线服务保持不变。迁移遵循官方 0.2 说明（utils `mosaico-tools/docs/migration-0.2.md`、`esp-mosaico-vibe-mode/docs/migration-0.2.md`，对应 `be151ae` 检出），重新编译应用，没有仅改旧 ZIP 的 schema。

**设备与账号声明：本次发布全程只在本机进行。用户的 Mosaico 设备仍在 0.1 固件上执行真实打印任务，从未连接、烧录或更新；未执行任何社区上传或服务登录。**

## 迁移内容

按 WordQuest 0.2 已验证流程逐文件适配，仅契约/构建级变更：

- `CMakeLists.txt`：`esp-mosaico-recovery` 路径改为 0.2 的 `esp-mosaico-vibe-mode`（`esp_mosaico_app_recovery` 组件目录与 `mosaico_idf_project.cmake` 包含路径），`system_update.cmake` 不变。
- `partitions.csv`：`factory/app/factory` 改为 `vibe_mode/app/test`，`ota_0` 改名为 `main_app`（`app/ota_0`，仍从 `0x210000` 开始）；其余偏移与大小不变。
- `sdkconfig.defaults`：新增 `CONFIG_TINYUSB_CDC_COUNT=2`（0.2 双 CDC 契约）；QIO、UART console 等既有契约行保持。应用自身的 `CONFIG_ESP_TLS_INSECURE*` 行属于应用行为，保持原样。
- 应用代码不做 0.2 改动：`main.c` 仍通过 `esp_mosaico_app_recovery` 调用 `iris_ota_support_start()`，OTA writer 只在保留固件。最新源码包含 MQTT 生命周期（`bmb_mqtt.c`、`bmb_transport_policy.c`）、任务哈希（`task_hash`）与放大后的键盘场景。
- `ui/main.json` 在副本中用 `tools/gen_ui.py` 重新生成，与权威场景逐字节一致（211 个对象）；`ui_apps.bin` 由 gspc 0.7.0 重新烘焙。
- 构建使用的 `managed_components`/`dependencies.lock` 从同一依赖清单的既有 bambu_monitor IDF 构建副本播种；`esp_iris` 由固定 utils 检出本地解析为 0.2.0（见 `migrated-inputs/dependencies.lock`）。

## 校验结果

- 官方 `esp_iris.py bundle --json inspect` 与 Ideas `validate_firmware()` 均通过（`evidence/new-bundle-inspect.json`、`evidence/validation.json`）。
- 三个组件与 SHA-256 核对通过：分区表 4,096 字节（写入 `0x8000`）；应用 2,000,016 字节（写入 `main_app`/`0x210000`）；UI 数据 1,333,200 字节（写入 `0xE80000`）。
- 包内只有 `manifest.json`、`partition-table.bin`、`ota_0.bin`、`ui_apps.bin`；没有 Bootloader、Vibe Mode 固件或 NVS 镜像，无 `manifest.sig`。
- 有效 sdkconfig 符合官方 v2 契约、ABI 2、双 CDC、QIO，应用不含 OTA writer（`effective-sdkconfig`）。
- 46 个 `main/ui/pc` 源文件与构建前快照哈希一致（`evidence/source-snapshot.json`）；构建前与构建后全目录 grep 两台测试打印机的真实 LAN 访问码字面量均为 0 处（本 README 亦不记录该字面量），`BMB_DEFAULT_IP_STR`/`BMB_DEFAULT_CODE_STR` 保持为空。
- 便携原生回归 4 组全部通过（report / command / rx / transport_policy，含新 task_hash 与传输策略用例；`evidence/native-test.log`）。
- 完整 low-noise build 成功，657.7 秒，runner 计数 1 warning：IDF 将应用与较小的 `vibe_mode`/test 分区比较产生的尺寸提示（应用实际写入容量充足的 `main_app`，官方产品校验通过）。其他提示为上游固有：IDF `esp_wifi`/`wpa_supplicant` 私有 include 检查、上游 `gsp_bundle.cmake` 的 CMP0175 dev 提示（见 `idf-low-noise-build` 记录）。

**验证范围：本地构建与官方校验通过；尚未在 Vibe Mode 0.2 设备上实测，也没有执行社区上传。** 旧 0.1 设备迁移到 0.2 基础固件属于单独操作，本应用包不负责升级基础固件；安装本包要求设备已在 0.2.0 及以上 Vibe Mode。

## 证据

本目录保留迁移输入（`migrated-inputs/`）、源码快照、依赖 lock、有效 sdkconfig、bin/ELF/map（见 `idf-low-noise-build/` 与 `evidence/`）、构建日志、官方 inspect/校验结果与复现脚本（`release_*.py`、`reproduction.json`、`bambu-v02-build-source.zip`）。主要结果见 `evidence/validation.json`、`evidence/new-bundle-inspect.json`。
