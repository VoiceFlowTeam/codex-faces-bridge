# Codex FACES Bridge 使用手册

适用固件：`0.2.0-fire-faces`

适用平台：macOS + Codex 桌面应用

蓝牙设备名：`kbd-1.0-codex-micro`

## 1. 产品说明

Codex FACES Bridge 将以下硬件组合模拟为 Codex Micro 兼容的蓝牙 HID 设备：

- M5Stack FIRE（经典 ESP32 版本）
- M5Stack FACES II Bottom
- FACES GameBoy Panel

固件直接在 ESP32 上实现蓝牙 HID 和 Codex RPC 通信，不需要额外运行 macOS 后台桥接程序。

> 本项目是根据已观察到的设备行为完成的兼容实现，并非 OpenAI、Codex 或 Work Louder 官方产品。

## 2. 硬件安装

1. 关闭 M5Stack FIRE 电源并拔下 USB 线。
2. 如果 FIRE 底部安装了原装 M5GO Bottom，请先将其拆下。
3. 将 FACES II Bottom 安装到 FIRE 底部。
4. 将 FACES GameBoy Panel 安装到 FACES II。
5. 确认排针完全插入、没有错位，然后通过 USB-C 接通电源。

M5GO Bottom 和 FACES II Bottom 是两种替代底座，不能同时叠装。

## 3. 首次连接

### 3.1 蓝牙配对

1. 启动 FIRE，等待主界面出现。
2. 在 Mac 上打开“系统设置 → 蓝牙”。
3. 在附近设备中找到 `kbd-1.0-codex-micro`。
4. 点击“连接”，等待配对完成。
5. 启动 Codex 桌面应用。
6. 打开 Codex Micro 欢迎或设置界面，按提示完成引导。

如果该设备以前配对过，但无法重新连接：

1. 在蓝牙设置中选择“忽略此设备”。
2. 重启 FIRE。
3. 等待设备重新出现后再次配对。
4. 必要时退出并重新启动 Codex。

### 3.2 判断连接是否成功

连接成功后应观察到以下现象：

- Codex 能识别到 Codex Micro 兼容设备。
- FIRE 屏幕顶部显示连接状态颜色。
- FIRE 屏幕上的六个方块能够接收 Codex 下发的线程颜色。
- FACES II 的 LED 能显示对应的六组颜色。

仅在 macOS 蓝牙设置中显示“已连接”还不代表 RPC 已完成。六组颜色能够由 Codex 更新，才表示双向通信已经工作。

## 4. 屏幕与灯光

### 4.1 顶部状态条

- 蓝色：普通操作层。
- 橙色：Agent 选择层。
- 灰色：当前没有 Codex 主机连接。

右上角的四段电池图标显示 FIRE 的 IP5306 电量档位：

- 四段：100%
- 三段：75%
- 两段：50%
- 一段红色：25%
- 绿色：正在充电
- 红色短横线：暂时无法读取电源管理芯片

### 4.2 六个线程方块

屏幕中的 `0`～`5` 六个方块对应六个 Codex 线程或 Agent 槽位。颜色、亮度和效果由 Codex 下发。

FACES II 上前六颗 LED 显示同样的线程颜色，其余 LED 用于环境或连接状态显示。

## 5. 按键说明

| 硬件输入 | 普通层 | Agent 层 |
| --- | --- | --- |
| 十字键 | 径向输入 `v.oai.rad` | 选择 `AG00`～`AG03` |
| GameBoy A | `ACT06` | 选择 `AG04` |
| GameBoy B | `ACT07` | 选择 `AG05` |
| Start | `ACT08` | `ACT08` |
| Pause 短按 | `ACT09` | `ACT09` |
| Pause 长按约 0.7 秒 | 切换到 Agent 层 | 返回普通层 |
| Pause + 十字键上/下 | 推理旋钮顺时针/逆时针 | 同左 |
| FIRE A | `ACT10` | `ACT10` |
| FIRE B | 推理旋钮按压 | 同左 |
| FIRE C | `ACT12` | `ACT12` |

`ACTxx` 的最终功能由当前 Codex 版本和 Codex Micro 配置决定。

Codex 会保存命令键和摇杆映射，固件只发送原始设备语义：

- 按住 Pause 再按十字键上/下，可降低或提高当前推理强度。
- FIRE B 短按相当于旋钮确认；长按会打开 Codex Micro 设置。
- `ACT10` 与 `ACT11` 在 Codex 中属于同一个双宽按键槽位，因此固件只使用可触发该槽位的 `ACT10`。

## 6. 推荐操作流程

### 普通操作

1. 确认顶部状态条为蓝色。
2. 使用十字键进行方向或径向选择。
3. 使用 GameBoy A/B、Start、Pause 或 FIRE A/B/C 触发 Codex 操作。

### 选择 Agent

1. 长按 Pause 约 0.7 秒。
2. 顶部状态条变为橙色，表示已经进入 Agent 层。
3. 使用十字键选择前四个槽位。
4. 使用 GameBoy A/B 选择第五、第六个槽位。
5. 再次长按 Pause 返回普通层，顶部状态条恢复蓝色。

切换操作层时，固件会主动释放仍处于按下状态的按键，避免出现按键卡住。

## 7. 快速功能测试

建议首次配对后按以下顺序测试：

1. 确认蓝牙设备显示为“已连接”。
2. 打开 Codex Micro 界面，观察六个方块或 LED 是否改变颜色。
3. 按下并松开 GameBoy A，确认 Codex 收到操作。
4. 分别按十字键四个方向，确认径向输入正常。
5. 短按 Pause，确认普通按键事件正常。
6. 长按 Pause 约 0.7 秒，确认顶部由蓝色变为橙色。
7. 在橙色状态下按十字键和 GameBoy A/B，测试六个 Agent 槽位。
8. 再次长按 Pause，确认返回蓝色普通层。
9. 按住 Pause 后分别按上/下，确认 Codex 的推理强度选择发生变化。
10. 短按 FIRE B，确认当前高亮项目被执行；长按 FIRE B，确认打开 Codex Micro 设置。

## 8. 固件更新与恢复

发布包中的 `codex_faces_bridge_full.bin` 是合并固件，包含 Bootloader、分区表和应用程序，应从 Flash 地址 `0x0` 写入。

使用 ESP-IDF 5.5 环境或安装了 `esptool` 的 Python 环境执行：

```bash
python -m esptool --chip esp32 \
  -p /dev/cu.usbserial-XXXX \
  -b 460800 \
  write_flash \
  --flash_mode dio \
  --flash_size 16MB \
  --flash_freq 40m \
  0x0 codex_faces_bridge_full.bin
```

将 `/dev/cu.usbserial-XXXX` 替换为 FIRE 实际串口。可以使用以下命令查找：

```bash
ls /dev/cu.usbserial-*
```

刷写完成后设备会自动复位。如果 Mac 缓存了旧的蓝牙身份，请忽略旧设备并重新配对。

## 9. 故障排查

### 蓝牙列表中找不到设备

- 确认 FIRE 已启动并显示主界面。
- 关闭再打开 Mac 蓝牙。
- 重启 FIRE 后等待数秒。
- 确认设备名为 `kbd-1.0-codex-micro`。

### 蓝牙已连接，但 Codex 没有识别

- 在 macOS 蓝牙设置中忽略该设备，然后重新配对。
- 退出并重新启动 Codex。
- 重新打开 Codex Micro 引导界面。
- 确认 Codex 所需的 Input Monitoring 权限已经启用。
- 观察屏幕或 LED 是否收到六组颜色；如果没有，说明 RPC 尚未建立。

### 按键没有反应

- 确认 Codex 已连接，而不只是 macOS 蓝牙显示已配对。
- 短按和长按 Pause 的含义不同；切换层需要持续约 0.7 秒。
- 拔下电源，重新安装 FACES GameBoy Panel，再次启动。
- 如果十字键和 GameBoy A/B 全部失效，重点检查 FACES II 和 GameBoy Panel 的连接。

### 屏幕左侧空白、画面裁切或颜色反相

- 使用 `0.1.2` 或更高版本固件。
- 该版本已改用 FIRE 的 ILI9342C 原生 320×240 显示参数。
- 如果升级后仍异常，完全断电后再重新启动，不要只执行软件复位。

### 屏幕正常，但 LED 不亮

- 重新安装 FACES II Bottom。
- 确认使用的是 FACES II，而不是仍安装着 M5GO Bottom。
- 先检查 Codex 是否已经下发线程颜色；未连接时 LED 状态可能与正常工作时不同。

### 电池图标或 Codex 电量不更新

- 使用 `0.1.3` 或更高版本固件。
- IP5306 只能报告 100%、75%、50%、25% 和 0% 的粗略档位，这是硬件限制。
- 电量每 30 秒轮询一次，不会逐百分比变化。
- 如果显示红色短横线，请完全断电并重新安装 FACES II 后再启动。
- macOS 可能缓存旧的 BLE 服务；忽略蓝牙设备并重新配对后再检查。

## 10. 技术兼容范围

当前固件实现：

- BLE HID over GATT
- 标准 BLE Battery Service
- Vendor Usage Page `0xFF00`
- HID Report ID `6`
- 63 字节 HID 报告数据
- RPC Channel `2`
- Work Louder 兼容身份：VID `0x303A`、PID `0x8360`
- `sys.version`
- `device.status`
- `v.oai.thstatus`
- `v.oai.rgbcfg`
- `v.oai.hid` 通知
- `v.oai.rad` 通知
- 旋钮按压 `ENC` 与旋转 `ENC_CW` / `ENC_CC`
- `off`、`solid`、`snake`、`rainbow`、`breath`、`gradient`、`shallowBreath` 灯效
- 实时 `profile_index` / `layer_index` 状态
- 由芯片蓝牙 MAC 派生的唯一序列号

## 11. 发布产物

本地构建或项目发布页可以提供以下产物：

- `codex_faces_bridge_full.bin`：完整刷机固件
- `codex_faces_bridge_source.zip`：ESP-IDF 源码包
- `SHA256SUMS.txt`：发布文件 SHA-256 校验值

二进制构建产物不提交到源码仓库；发布时应从经过验证的源代码重新生成，并同时提供校验值。
