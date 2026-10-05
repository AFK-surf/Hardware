# B1 报告：固件 BLE 配网

2026-10-05 · 分支 `codex/ble-provisioning`（worktree `~/Desktop/AFK/Hardware-b1`），未推送、未合 main。

## 结论

新烧的 T-Circle 不再需要编译进任何密钥：开机广播 `Comma-5D28`，手机用圆屏上的 6 位码配对，下发 Wi-Fi 和 Comma 凭证，设备试连成功后写入 NVS、关掉 BLE、可以打电话。验收三项（出厂 → 配网 → 上线 → 通话；重启保留；恢复出厂）都在板子上跑通。

给 B2 的契约：`docs/ble-provisioning.md`（UUID、JSON、状态、错误码、时序、屏幕文案、NVS 风险都在里面）。

## 和 prompt 设计的差异（都有原因）

| 项 | 改动 | 原因 |
| --- | --- | --- |
| `wifi_scan` 通知 | 不是裸 JSON 数组，而是分页 `{"page","pages","aps":[...]}` | 20 个网络约 900 B，超过一条通知（MTU−3）；分页对任何 MTU 都成立。实测 MTU 517 下 20 个网络分 3 条 |
| `status.state` | `idle / wifi_connecting / wifi_failed(+error) / wifi_up / saved`，`connector_*` 预留给 A2 | A2 还没合，B1 能报告的终点是"凭证已存 + Wi-Fi 通"；app 以后端设备列表判定上线 |
| `credentials` | 多一个可选 `connector_server` | 后端 connector-token 响应里的 `server` 可能和语音 server 不同 |
| `info` | 多 `name`、`provisioned` | app 区分全新设备 / 配对模式 |
| 配对模式 | 长按 BOOT 3–10 s 松手进入；点屏或 5 分钟无连接退出 | prompt 提到"pairing mode"但没定义入口；换 Wi-Fi（公司 ↔ 家）和轮换凭证都要用 |
| 绑定 | 开 bonding，但只存 RAM；每次配对会话用新的随机静态地址 | 同一会话掉线可免码重连；恢复出厂后手机上的旧 bond 永远对不上新地址，不会出现"加密失败要去系统里忽略设备" |
| NVS 加密 | **没开** | 见下 |

## NVS 加密：没开，风险说明

ESP32-S3 上加密 NVS 要么开 flash encryption，要么把 HMAC 密钥烧进 eFuse，两者都**不可逆地改芯片**；而且没有 secure boot 时，攻击者可以刷自己的固件让芯片替他解密。对唯一一块共用开发板不值得。现状：拿到板子的人可以经 USB 读出 Wi-Fi 密码、connector token、voice key。缓解：两个凭证按设备发、可吊销（B3 解绑时吊销），恢复出厂会擦除。量产方案：secure boot v2 + flash encryption（release）+ NVS HMAC 加密，工厂预置。

## 实现

| 文件 | 内容 |
| --- | --- |
| `main/provisioning.c/.h` | NimBLE GATT 服务、passkey（仅显示，SC only，MITM，16 字节密钥）、Wi-Fi 试连、提交、BLE 关闭 |
| `main/device_config.c/.h` | 唯一凭证入口 `device_config_get()`（语音已改用；A2 的 Connector 也用它）；NVS 命名空间 `comma`，`provisioned` 标志最后写，断电不会半配 |
| `main/wifi.c/.h` | 无凭证也启动 STA（可扫描）；试连模式按断开原因判定 `wrong_password` / `not_found`；`wifi_set_power_save()` 处理 BLE 共存 |
| `main/ui_card.c/.h`、`ui_font.c/.h` | 圆屏文字卡片（Inter，OFL；`tools/gen_ui_fonts.py` 生成）；按圆的弦宽自动换行 |
| `main/display.c` | 显示任务在 logo 和卡片间切换 |
| `main/main.c` | BOOT 长按倒计时（3 s 显示，松手 = 配对模式，10 s = 恢复出厂）；BLE 开着时不能拨号，通话中长按不触发 |
| `main/cue_voice.c` | 从 `device_config_get()` 取 server/group/key；重新配网后清掉 4401 锁 |
| `secrets.h.example` | 只在 `COMMA_DEV_SECRETS 1` 时生效的开发覆盖；没有 `secrets.h` 也能编译 |
| `tools/ble_provision.py` | bleak 测试客户端：`scan / info / wifi-scan / provision / wrong-password / bad-writes` |
| `tools/card_preview.c` | 在 Mac 上渲染所有屏幕卡片（`build-preview/cards.png`） |

**BLE 不和通话同时跑**：存完凭证后 BLE 完全 deinit（`nimble_port_stop/deinit`），Wi-Fi 切回 `WIFI_PS_NONE`；BLE 运行期间 Wi-Fi 必须开 modem sleep（共存要求），所以配网期间也禁止拨号。

## 实测（2026-10-05，办公室 `ToEverything-IOT`，macOS + bleak 1.x）

| 测试 | 结果 |
| --- | --- |
| 擦 NVS 后首次启动 | `provisioning started (unprovisioned)`，广播 `Comma-5D28`（名字在扫描响应里） |
| 配对 | 连接即发 Security Request，圆屏出码，macOS 弹窗输码 → `paired (LE Secure Connections, passkey)`；同会话重连免码 |
| `wifi_scan` | 20 个网络、3 条通知；中文 SSID 原样传回 |
| 坏写入 5 种 | 全部按文档错误码拒绝（0x80/0x81/0x82） |
| 错误密码 | 6 s 报 `wifi_failed: wrong_password`（reason 202 两次） |
| 配网全流程 | 写 Wi-Fi → 3.5 s 拿到 IP → 写凭证 → `saved`，共 **5.7 s**；随后 BLE off，扫描不到广播 |
| 通话 | 用 NVS 里的 voice key 拨通 staging，22 s，`session.ended reason=caller_hangup`，`close 1000`，`mic_dropped=0`，无 4410/4400；用户确认正常 |
| 断电重插 | `config: provisioned as "T-Circle B1"` → 自动连 Wi-Fi，无 BLE |
| 配对模式 | 长按 3–10 s 松手 → `provisioning started (pairing mode)`，保留原 Wi-Fi 继续广播 |
| 恢复出厂（在配对模式中长按 10 s） | `NVS erased: ESP_OK` → 重启 → `no stored credentials` → 重新广播 |

串口日志：`/private/tmp/claude-501/-Users-mafuzhen/a8de1816-a9bd-4054-ada3-cd7ef8a6adfd/scratchpad/tcircle.log`（临时目录）。

## 资源

| | main（757aff5） | B1 |
| --- | --- | --- |
| 镜像 | 2.49 MB | 2.72 MB（4 MB 分区） |
| 静态 DIRAM | 240.6 KB（70.4%） | 265.8 KB（77.8%） |

NimBLE 走 PSRAM（`MEM_ALLOC_MODE_EXTERNAL`）；BLE 关掉后控制器内存释放，但没调 `esp_bt_mem_release`（那样就不能再进配对模式）。运行时堆没有单独测。

## 没做 / 留给后面

- **没测 Android**：只用 macOS + bleak 验过。Android 的 `requestMtu`、系统配对弹窗、`removeBond` 由 B2 实测。
- **测试凭证**：voice key / group 用的是 main 里 `secrets.h` 的 staging 值，connector token 是占位串 `salix_conn_b1_placeholder`（A2 合并前没人用），workspace 填的 `b1-test`。B2 会走真正的铸造流程。
- **板子现状：恢复出厂、未配网**（在广播）。另一个会话 A2 若要打电话，要先用 `tools/ble_provision.py provision` 配一次（需要在 macOS 弹窗输码），或者在 `secrets.h` 加 `#define COMMA_DEV_SECRETS 1`。
- 把 main 上的 secrets.h 留作开发覆盖：现有 `secrets.h` 没有 `COMMA_DEV_SECRETS`，合并后默认走 BLE 配网。
- 串口插曲：16:37 我用 RTS 复位后 USB-JTAG 控制台整个哑了（应用照常跑、能 ping，esptool 也连不上），用户拔插后恢复。原因没查清，像是我的日志脚本复位/重开端口的时序问题，不是固件逻辑。
- B3：去掉 `secrets.h` 本身、解绑吊销。A2 的 Connector 接 `device_config_get()` 读 `connector_server`/`connector_token`，状态可调 `provisioning_set_connector_state()`。
