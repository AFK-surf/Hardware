# B2c 报告：用安卓 app 真机添加 T-Circle

2026-10-05 19:07 – 2026-10-06 11:33 · 手机 `23112153D000003`（`codex/add-device-ble` `8bdd09b`，staging）· 板子固件 A3（`App version 5d93755-dirty`，17:43 编译；开机内部堆 168 KB，与 A3 报告一致；A3 已提交为 `8513178`）· 办公室 `ToEverything-IOT`

## 结论

出厂 → 手机添加 → 上线 → 开操作权限 → agent 经 T-Circle 读局域网 → 点屏通话，**整条链路在真机上跑通**。错误密码不铸凭证，移除设备会吊销 connector token 和 voice key。发现 2 个固件问题、5 个安卓问题（都不阻塞），见文末。

新设备：
- 第一次配网（测试主体）：**`dev_PPsW5YUvPh80-q0Z8I8YlA`**（之后被用户移除，用于验证吊销）
- 最后一次重新配网（板子现状）：**`dev_yAA_sfKRFE_vb1d_f_70WA`**，在线、只读、名字 `Comma-5D28`

## 逐项结果

| 检查 | 结果 |
|---|---|
| 恢复出厂（BOOT 10 s） | ✅ `main: factory reset` → `NVS erased: ESP_OK` → `no stored credentials` → 广播 `Comma-5D28`。第一次用户 3–10 s 松手进了配对模式；第二次一直按着，**连擦 3 次**（见固件问题 F1） |
| 手机添加设备 | ✅ 全流程 1 分 46 秒，状态序列与契约一致 |
| 新设备出现、在线 | ✅ 新 `device_id` `dev_PPsW…`，设置 › 设备显示 Online（`01-devices-after-add.png`） |
| 旧设备 `dev_M3Z6…` | ✅ 用户从手机移除，有确认框、无报错。⚠️ 它的凭证是 B1b 用 Mac 脚本配的，这台手机没有 token 原文，按 B2 设计**吊销不了**；后端删设备也不吊销（B2 后端缺口 2）。板子已恢复出厂，旧 token 不在任何设备上，但在后端仍有效 |
| 初始只读 | ✅ 串口 `access: read-only`，拨号 `scope=local_file_read` |
| Allow operations | ✅ 19:20:48 `device_access` → `operations allowed` → metadata `scope=""` |
| agent 经新设备调 `lan-test` | ✅ agent 自己发现旧设备没了，用 `mcp_manager.update` 把 `lan-test` 重绑到 `dev_PPsW…`，`lan_status` 返回 "LAN test device ok, local time 19:23"。LAN 服务器 4 个请求全部来自 192.168.5.17，板子上 33–127 ms（`02-lan-status-chat.png`） |
| 新 voice key 通话 | ✅ `vc_01M45WWNC5RB1G8YYZHXX0QQJX`，45.9 s（比要求的 ~1 分钟短），`caller_hangup`，`close 1000`，无 4410/4400，`mic_dropped` 全程 0，`send_max_ms` ≤ 170；通话中内部堆最低 105.8 KB |
| 错误 Wi-Fi 密码 | ✅ app 6 s 后回到 Wi-Fi 页，红字 "Wrong password for "ToEverything-IOT". Try again."（`06-wrong-password.png`）；串口 `wifi_failed: wrong_password`；设备列表无新设备 |
| 中途退出（无孤儿） | ✅ 两次：① 错误密码后从 Wi-Fi 页返回；② 用户输对密码后在 "Setting up" 页点 Cancel。之后设备列表没有多出设备（`07-devices-after-remove.png`）。⚠️ ② 发生时板子被拔下换了 USB 口，**串口没录到**，不知道取消时走到了哪一步 |
| 移除本机配的设备 → 吊销 | ✅ 用户移除 `dev_PPsW…` 后：板子重启拨 Connector 得 `handshake=401`；点屏通话得 `voice/sessions` `handshake HTTP 401`。两个凭证都已失效 |

另外补测了 A3 没测的一项：**配对模式（BLE 开）+ Connector 在线时内部堆 106.8 KB**（最低 106.3 KB），BLE 开着时 Connector 握手最低 107.7 KB，远高于 20 KB 目标。

## 时间线（第一次添加，2026-10-05）

| 时间 | 串口 | 间隔 |
|---|---|---|
| 19:15:11 | 恢复出厂后开始广播 | |
| 19:16:58 | 手机连上，`MTU 517` | |
| 19:17:03 | `showing passkey` | 5 s（配对请求是通知横幅，要用户点开，见 A1） |
| 19:17:15 | `paired (LE Secure Connections, passkey)` | 输码 12 s |
| 19:17:20 | `scan found 18 networks` | 5 s |
| 19:18:35 | `wifi_connecting` | 选网 + 输密码 75 s |
| 19:18:39 | `wifi_up` | 4 s |
| 19:18:40 | `credentials received` → `saved` | 铸造两个凭证 + 写入 1 s |
| 19:18:44 | `connector_up`，`dev_PPsW5YUvPh80-q0Z8I8YlA` | 4.5 s |
| 19:18:46 | app 断开 → `BLE off` | 2 s |

连接到上线 **1 分 46 秒**，其中用户操作约 1.5 分钟，系统耗时约 15 s。app 显示的步骤（Join → Create its access → Save on the device → Connect to Comma）与串口状态一一对应。

重新配网（2026-10-06 11:31:54 → 11:32:57，63 s）时间分布相同：配对请求 4 s 后出码、输码 9 s、扫描 5 s、选网输密码 36 s、`wifi_up` 4 s、写入 1.5 s、上线 4 s。中间有一次失败：我用 adb 点了通知上的 "Pair & connect" 但输码框没出来，30 s 后 `pairing failed, status 13`，app 显示 "Could not add the device / Pairing did not finish…" + Try again，重试后正常。

## 问题

### B1（固件）

- **F1 按住不放会连续恢复出厂。** 按 BOOT 超过 10 s 擦完 NVS 重启后，键仍按着，新开机又从 0 计时，10 s 后再擦一次（19:15:08 / 21 / 34 三次）。无害但混乱。建议：开机时若 BOOT 已按下，等松开后才开始计时。
- **F2 配对模式里试连失败后不回原网络。** 已配网设备进配对模式，app 写了错误密码（`wifi_failed`）然后退出，设备一直停在没 Wi-Fi 的状态，直到 5 分钟配对超时才 `connecting to ToEverything-IOT`（19:33:32 断 → 19:39:08 Connector 重连，离线 5.5 分钟）。期间 app 设备列表仍显示 Online（见 A5）。建议：app 断开（或 `wifi_failed`）后立刻回到存储的网络，配对模式本身可以继续。

### B2（安卓）

- **A1 配对请求是通知横幅不是弹窗。** 这台 Nothing 手机上系统配对请求以 heads-up 通知出现（"Pairing request · Tap to pair with Comma-5D28" + "Pair & connect"），app 页面只写 "Type it into the pairing request on your phone"，用户要自己发现并点通知（`09-pairing-notification.png`）。建议文案改为提示"点系统通知里的 Pair & connect，再输入屏幕上的码"，或在 app 内先触发 `createBond()` 让系统更倾向弹对话框。
- **A2 "Setting up" 页加载图标不转。** "Join "ToEverything-IOT" · Working…" 那一行的图标是静止的（用户反馈），应加旋转动画（`05-setting-up.png`）。
- **A3 Wi-Fi 密码明文显示。** 密码框没有遮罩，也没有显示/隐藏切换（`04-password-dialog.png`）。
- **A4 密码框不自动聚焦。** 弹出后要再点一下输入框才出键盘。
- **A5 设备列表在线状态滞后。** 设备 Connector 已断开（19:33:32 起），返回设备列表时仍显示 `Comma-5D28` Online。可能是后端心跳超时前的状态，也可能列表没重新拉；没深查。

（另：设备列表里 T-Circle 和"Signed in"里的手机/电脑用同一个图标，不好区分，小问题。）

## 没做 / 注意

- 中途取消（Cancel）那次没有串口记录；能确认的只有"没留下孤儿设备"。铸造后、`saved` 前那 ~1 s 的取消窗口人手点不到，仍只有单测覆盖。
- 旧设备 `dev_M3Z6mxvKhQSYEwyYTber-g` 的 connector token 和 `secrets.h` 里的 staging voice key **在后端仍有效**（不是这台手机铸的，app 吊销不了）。需要的话由后端手动吊销。
- voice key 的吊销只通过"拨号 401"间接确认，没查 `GET voice-api-keys`。
- 打开串口时 pyserial 让板子复位了一次（19:07:53），无害。
- staging 的 MCP `lan-test` 现在绑在已删除的 `dev_PPsW…` 上；下次要用时 agent 会再重绑（本次就是这么做的），或删掉。
- 板子现状：`dev_yAA_sfKRFE_vb1d_f_70WA`，在线、**只读**（Allow operations 关），串口在 `/dev/cu.usbmodem101`（USB 口换了，不再是 `1101`）。

串口日志：`/private/tmp/claude-501/-Users-mafuzhen/686f549b-3ef2-45e5-9541-4fa4dbdac861/scratchpad/tcircle.log`、`tcircle2.log`（临时目录）。截图：`docs/reports/B2c-screens/`。
