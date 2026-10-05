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
| `credentials` | 多一个必填 `connect_url`（rebase 到 A2 后由可选 `connector_server` 改来） | A1 协议要求原样用铸造响应里的 `connect_url`（Salix 主机，不是 Comma API 主机） |
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
- B3：去掉 `secrets.h` 本身、解绑吊销。

## 接上 A2（rebase 到 5c4a59d 之后）

- `connector_credentials()` 改读 `device_config_get()` 的 `connect_url` / `connector_token`，每次拨号前重读；`device_config_generation()` 变了（配对模式里存了新凭证）就断开、立刻用新 token 重拨；401/403 不再"停到重启"，而是等新凭证。
- Connector 在凭证一存好就启动（BLE 还在），状态经 `provisioning_set_connector_state()` 报给 app：`connector_connecting` / `connector_up`（带后端 `device_id`）/ `connector_failed`。
- 未配网时 Wi-Fi 没有目标网络，`wifi_wait` 立刻返回错误；Connector 的等待循环对此加了 1 s 间隔，不会空转。
- **这部分只编译过，没上板**（A2 合入后板子还没刷，也还没有真实 connector token）。

## B1b：板上验收与合并（2026-10-05）

擦整片 flash 后刷 B1 分支，用 staging 真实凭证（connector token → `dev_M3Z6mxvKhQSYEwyYTber-g`，scope `local_file_read`）经 macOS + `tools/ble_provision.py` 配网。办公室 `ToEverything-IOT`。

### 第一次刷：发现问题，已修

凭证保存后 Connector 立刻拨号，但 BLE 还开着，控制器占着内部 RAM：`heap_internal=12463`，`websocket_client: Error create websocket task` 连续 6 次，直到 30 s 后 BLE 关掉（堆回到 52 KB）才连上。契约要求 BLE 开着时就报 `connector_up`，所以这是 bug。

修复 `08dfe19`：
- logo 任务栈（8 KB）放 PSRAM（`xTaskCreateWithCaps`），它只往 PSRAM 帧缓冲画、不碰 flash 写。
- Connector 的 `esp_websocket_client_init` 前后用 `heap_caps_malloc_extmem_enable(0 / CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)`，把客户端两块 4 KB 收发缓冲（普通 malloc，≤16 KB 默认进内部）挪到 PSRAM。websocket 任务栈本身在 esp_websocket_client 1.8 里固定用内部 RAM，挪不了。
- 首拨和拨号失败的日志加了最大连续块。

### 验收结果（修复后）

| 项 | 结果 |
| --- | --- |
| 出厂配网 | `saved` → `connector_connecting` → `connector_up`（`device_id` = `dev_M3Z6mxvKhQSYEwyYTber-g`），保存后 4.3 s，BLE 仍开着；app 断开后 `BLE off` |
| 设置 › 设备 | 用户在手机 Comma staging 上看到 T-Circle「在线」 |
| Connector 在线时通话 | 88.6 s，`caller_hangup`，`close 1000`，无 4410/4400；Connector 全程在线无重连；`mic_dropped` 只有两秒（4、1），都在 agent 说话、麦克风静音时 |
| 断电重插 | NVS 读到配置 → 6 s 拿 IP → 11 s Connector 上线，全程无 BLE；再打 64.2 s 正常 |
| 重新配对（BOOT 3–10 s） | 配对模式下 Connector 保持在线；测试客户端会先写 `wifi_credentials`（同一网络），Wi-Fi 重连导致 Connector 断一次；写 `credentials` 后 `credentials changed, reconnecting`，保存后 8.7 s `connector_up`，无需重启；随后 `BLE off` |
| BLE 是否全关 | 关后 8 s 扫描看不到 `Comma-*`；内部堆回到配对前水平（51.5 → 50.8 KB） |

### 内部堆（`heap_caps_*(MALLOC_CAP_INTERNAL)`）

| 场景 | 空闲 | 最低 |
| --- | --- | --- |
| 开机后（无 BLE），首拨前 | 67.2 KB（最大块 31 KB） | |
| Connector 在线、BLE 关、空闲 | 51.5 KB（修复前 34.4 KB） | 49.9 KB |
| 通话中（本次开机没起过 BLE，数字干净） | 17.1 KB | **14.4 KB**（A2 是 23 KB） |
| 出厂配网，BLE 开 + Connector 握手 | 首拨前 20.6 KB（最大块 7.7 KB）→ 5.9 KB | 3.1 KB |
| 配对模式（BLE 开）+ Connector 已在线 | 5.1 KB | **2.8 KB** |

B1 让通话最低值降了约 9 KB：NimBLE/配网代码多 25 KB 静态 DIRAM，logo 栈挪走省回 8 KB。BLE 窗口期只剩 3 KB 左右，能跑通但没有余量。

### 合并

`codex/ble-provisioning` rebase 到 main（只多了 docs 提交，固件树与上板测试的完全相同），main 快进到分支头，未推送。`docs/connector-plan.md` 标了 B1 完成、`secrets.h` 只作开发覆盖（需 `COMMA_DEV_SECRETS 1`）。

### 板子现状

已配网（T-Circle，staging，`ToEverything-IOT`），跑 main 的固件，Connector 在线。没有 `secrets.h` 覆盖。

### A3 必须知道

- **先腾内部 RAM 再加 HTTP client**。通话中只剩 14 KB；配对模式里 BLE + 在线 Connector 已经到 2.8 KB，再多一个 TLS 连接会在 BLE 窗口期失败或崩。可选：配对模式期间暂停 Connector；把不碰 flash 写的任务栈挪 PSRAM（`ui` 任务会擦 NVS 恢复出厂、`prov` 存 NVS、`connector` 写 allow 标志，这三个不能挪）；调低 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（全局，影响通话，要测）。
- `heap_caps_malloc_extmem_enable()` 是全局开关，`connector.c` 只在 init 那一刻改并马上恢复；别的模块照抄时也要立刻恢复。
- websocket 任务栈在 esp_websocket_client 1.8 里只能在内部 RAM（`xTaskCreatePinnedToCore`）。
- 给 B2：轮换凭证时只写 `credentials`，不要写 `wifi_credentials`，否则 Wi-Fi 会重连一次、Connector 掉线一次（测试客户端的 `provision` 总是两个都写）。

串口日志：`/private/tmp/claude-501/-Users-mafuzhen/f54661df-ccbe-4e40-aabc-a9495e53a393/scratchpad/tcircle.log`（临时目录）。
