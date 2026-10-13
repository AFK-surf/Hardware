# B3 Android 配对回归记录

日期：2026-10-13。所有时间均为 UTC+08:00。

## 结论

- 已定位并修复设备页空白。设备和登录会话已返回，但页面提前丢失了渲染回调。
- 已完成三轮已有配置的 StickS3 重新配网。三轮均保存新凭据，连接 Connector，并在 App 显示 `Pairing complete` 和 `Online`。
- 已修复手机关闭蓝牙后，App 停留在 Wi-Fi 选择页的问题。307 真机复测显示明确的断开提示，恢复蓝牙后可以重试。
- 配对成功文案已改为“配对成功 / Pairing complete”。成功仍以新设备的后端在线状态为准。
- 收尾发现 StickS3 被显示为 Computer，缺少移除入口。型号识别修复后，真机已成功清理两个旧测试条目。
- T-Circle 已补测一轮已有配置的重新配网，也显示配对成功并上线。
- 原始 13:58 的 `Could not add the device` 原因仍未知。注入网络失败能够产生同标题错误，但不能证明与原始故障同因。

## 环境与证据

| 项目 | 实际环境 |
| --- | --- |
| 手机 | Nothing A024，Android 16，ADB `23112153D000003` |
| App | `surf.comma.android.staging`，`stagingInternal`，未卸载或清除数据 |
| 源码基线 | Cue `6ef90f145`，本地修改 |
| 固件 | StickS3，已刷 `27c90ca`，本轮未刷写 |
| 板子 | `m5stack-sticks3`，MAC `14:C1:9F:D4:ED:20`，广播名 `Comma-ED20` |
| 账号 | `zhen@cue.surf` |
| Workspace | `wsp_X7wQDptcSaoxwp1TbjoYaakX` |
| 环境 | App `salix-staging.cue.surf`，板子 `salix-staging.comma.surf` |
| 初始设备 | 9 个条目。原 StickS3 为 `dev_WQ7_bdQRRc-lwD6mgAiQ2A` |

证据目录：`/Users/mafuzhen/Desktop/AFK/hw-logs/b3-android-pairing-20261013-fy29hge5/`。
目录包含 App logcat、板子串口、截图、UI XML、APK、mapping 和测试结果。
采集不绑定 App PID，因此覆盖了覆盖安装和 App 重启。
19:32 关机使 USB 串口断开，采集进程退出。19:33 重新启动采集。
19:37 更换板子后，19:38 启动独立的 `tcircle-serial.log`。
日志前缀为 Mac 收到日志的本地时间。手机日志自身时间约慢 0.5 秒。
长时间等待或电脑休眠可能导致串口日志延迟，不能用接收时间推算所有硬件耗时。

未记录手机解锁密码、Wi-Fi 密码或凭据明文到报告。
采集器屏蔽 token、voice key 和授权字段。
没有发送聊天指令、执行家电操作、重置板子、退出账号或修改后端。

## 设备页空白

303 冷启动后，Your devices 只有 Add device，Signed in 停留在 Loading。
304 诊断包收到 9 个设备和 65 个登录会话，页面仍为空。
重新进入页面后可以显示，说明请求成功不等于页面更新成功。

305 在 15:00:20.807 记录了决定性顺序：

1. DevicesPage 构造期间注册渲染回调。
2. 加载状态触发 `renderAll`，此时栈中只有 root。
3. 旧代码以“不在栈内”为条件删除 devices 回调。
4. 页面随后入栈，后端数据随后返回，但回调已经丢失。

修复让 PageHost 管理回调寿命。页面实际移除时才删除其回调。
`SettingsActivity` 委托 PageHost 渲染当前栈内页面。
回归测试在页面入栈前触发重绘，然后交付设备结果和离线更新。
旧行为只有 Add device 一行，修复后显示设备且能更新状态。

306 在 15:05 和 15:06 两次冷启动后均显示设备及当前登录会话。
证据：`fixed-306-devices.*`、`fixed-306-restart-devices.*`、`regression-before*`、`regression-after*`、`diagnostic.patch`。

## 三轮成功流程

三轮起始状态均为已有配置。没有把它们计为三个全新绑定测试。

| 轮次 | 起始状态与操作 | 串口结果 | App 结果 |
| --- | --- | --- | --- |
| R1，306 | 原设备重新进入配对模式 | 15:09:14 wifi_connecting，15:09:15 wifi_up，15:09:16 saved，15:09:27 新 Connector 在线 | `round1-success.*` 显示 Pairing complete、Online |
| R2，306 | R1 配置。先取消配对，再错误 Wi-Fi 密码，最后改正 | 15:14:55 wifi_connecting，15:14:58 saved，15:15:06 新 Connector 在线 | `round2-corrected.*` 显示 Pairing complete、Online |
| R3，307 | R2 配置。覆盖返回、断网和蓝牙断开后的重试 | 19:26:26 wifi_connecting，19:26:28 wifi_up，19:26:33 saved，19:26:38 新 Connector 在线 | `round3-307-submitted.*` 显示 Pairing complete、Online |

本轮新建的设备：

| 轮次 | device_id | 收尾处理 |
| --- | --- | --- |
| R1 | `dev_HvW0asdH2UwTz6cXIicZnQ` | 19:34 已撤销凭据并移除 |
| R2 | `dev_tURhj6ymVKHcehveNzceqg` | 19:35 已撤销凭据并移除 |
| R3 | `dev_yILlMIG_0yJkesqAUjaVqQ` | 保留为最后一个有效测试设备 |

R3 的旧 Connector 曾先恢复连接，随后新凭据使它重新连接。
成功页等待的是 R3 新 device_id 的后端状态，不能把旧设备恢复在线计为新配对成功。

## 失败路径

| 场景 | 证据与结果 | 判定 |
| --- | --- | --- |
| 取消系统配对 | 15:11:08，板子 status 1288、断开 0x205。App 提示 Pairing did not finish，并提供 Try again | 通过 |
| 六位码超时 | 多次等待超过约 30 秒，板子 status 7。没有进入 Wi-Fi 或凭据发放。App 提示重试 | 已观察。未输入的超时不计为错误码测试 |
| 错误 Wi-Fi 密码 | 15:13:52 开始，15:14:09 wrong_password。App 显示对应网络的错误密码提示 | 通过 |
| 改正密码重试 | 同一 BLE 会话重试，R2 成功。错误密码期间恢复原网络和 R1 凭据 | 通过 |
| Wi-Fi 连接期间返回 | 15:38:42 提交，15:38:44 返回。板子记录 app left without saving，15:38:51 原 R2 设备恢复在线 | 通过，无新凭据 |
| wifi_up 后凭据申请失败 | 关闭手机 Wi-Fi 和数据。15:40:42 板子 wifi_up，App 提示 Could not reach Comma. Check your connection. | 通过明确提示，无 credentials received |
| 失败后 BLE 重连 | 15:42 和 19:25 重连后仍要求选择 Wi-Fi。19:26:26 再次写入 Wi-Fi 并试连，随后 R3 成功 | 通过，没有跨 BLE 连接复用试连状态 |
| 306 关闭手机蓝牙 | 15:42:26 板子断开。App 一直停在 Choose Wi-Fi。系统未送达 GATT 断开回调 | 失败，已修复 |
| 307 关闭手机蓝牙 | 19:24:50 板子断开，19:24:51 App 显示 The connection to the device dropped.，可重试 | 通过 |

网络注入失败没有 HTTP 响应，因此没有 HTTP 状态码或服务器 request id。
现有日志未提供原始 13:58 故障的请求错误正文或 request id。
没有证据将该原始故障归因于网络、后端、蓝牙或固件。

蓝牙修复监听 `BluetoothAdapter.ACTION_STATE_CHANGED`。
关闭蓝牙时，它使待完成操作失败并发出 LinkLost。
链接关闭时移除广播接收器。
两项 Robolectric 测试覆盖已连接与连接等待状态，均在旧代码上失败、修复后通过。
证据：`bluetooth-regression-before.*`、`bluetooth-regression-after.xml`、`round3-307-bluetooth-off.*`。

## 证据排除与限制

- `round1-submitted.xml` 是 uiautomator 失败后残留的旧 XML，不作为页面证据。采集助手已改为先删除旧文件并拒绝失败结果。
- 15:39 的一次自动点击误选了重新排序后的 `403` 网络。已立即返回，未保存配置。它不计入指定的密码回归。
- 锁屏黑图不作为 App 故障证据。
- 未测试恢复出厂后的首次绑定或错误六位码。取消配对已覆盖该失败路径的二选一要求。
- 本轮没有验证 45 秒 Connector 通话故障，也没有执行 voice-first 时序。

## StickS3 型号与清理

307 的设备详情把 StickS3 显示为 Computer，没有 Remove device。
`HomeDevice.isCommaDevice` 只识别 `t-circle`。
板上固件实际报告 `m5stack-sticks3`。
修复在现有设备类型判断中加入 StickS3，没有改变身份或凭据所有权。

回归测试通过真实 HomeDevicePage 点击移除，再确认撤销凭据和移除设备的调用顺序。
旧代码找不到移除按钮，修复后通过。
308 临时诊断包仅记录所打开详情的 device_id 和 client_source，用于精确核对清理目标。
核对后，通过 App 正式撤销流程清理 R1 和 R2，相关 DELETE 均返回 200。
309 已移除该临时日志。
清理后有原来的 9 个条目和 R3，有效历史设备未删除。
证据：`sticks3-kind-before*`、`sticks3-kind-after.xml`、`cleanup-308-round*-detail.*`、`cleanup-round*-removed.*`。

## 重启验证

用户在 19:32 将 StickS3 关机再开机。
USB 重连采集又触发一次 USB reset，因此未取得首次电源启动的完整日志。
19:33 的完整启动日志确认从已保存配置连接 Wi-Fi，并以 R3 原 device_id 重新上线。
19:33:29 Connector 为 `dev_yILlMIG_0yJkesqAUjaVqQ`，generation 3。
没有重新输入配对码或 Wi-Fi 密码。

19:37 对最终 309 强制停止并重新启动。
首次进入 Devices 即显示完整列表、Signed in 和 R3 Online。
证据：`final-309-version.*`、`final-309-restart-devices.*`。

## T-Circle 对照

用户随后要求补测 T-Circle，并将圆屏板接到同一 USB 端口。
重新读取 USB 序列号确认 MAC 为 `98:A3:16:F5:5D:28`，未凭端口名推断板型。
启动日志确认固件 `27c90ca`，原有设备 `dev_rprE9KdUAd-25Q8nLHZIHQ` 能连接。

这是一轮已有配置的重新配网，使用最终 309。
前两次因未及时输入六位码而超时，板子 status 7，App 提示配对未完成。
第三次于 19:43:27 完成 Secure Connections 配对。
19:43:55 重新试连 Wi-Fi，19:43:56 wifi_up，19:43:57 保存新凭据。
19:44:06 Connector 以 `dev_eIKz8AYxDJPlHt4Qb6DxWQ` 上线。
19:44:19 App 显示 Pairing complete 和 Online。

保留新 T-Circle 设备和最后成功的 StickS3 设备，未删除原有 T-Circle 历史条目。
最终列表共有 11 个条目，即原来的 9 个加两块板子的有效测试设备。
证据：`tcircle-serial.log`、`tcircle-timeout.*`、`tcircle-success.*`、`final-all-device-list.*`。
T-Circle 只做了这一轮对照，没有重复 StickS3 的全部失败路径。

## 构建与收尾

306：103 项设置测试通过。307：105 项设置测试通过。
最终 309：106 项设置测试通过，0 失败、0 跳过。
309 的滚动检查、stagingInternal 构建、APK 签名和 release isolation 检查通过。
固定签名 SHA-256：`0e4898f6f9528d66dcedeab1a85fd21fc0eed6b768decabc8b854199114f78b0`。
所有安装均使用 `adb install -r`，没有修改签名或清除 App 数据。

最终产物为 `comma-staging-309.apk` 和 `mapping-309.txt`。
`artifact-sha256-309.txt` 记录产物来源校验值。
`test-results-309/`、`test-summary-309.json` 和 `validation-309.log` 保留完整构建测试证据。

Cue 修复提交：`2ac3e13c0`，`fix(android): restore device updates and pairing recovery`。
提交包含三项修复、成功文案、行为回归测试和 versionCode 309。
APK 在提交前生成，所以版本显示 `0.2.0-6ef90f145-dirty (309)`。
最终源码与该包对应。本轮没有 push、merge 或后端部署。

19:44:56 已确认手机设置恢复：息屏 30000 ms，Wi-Fi 开，移动数据开，蓝牙开，飞行模式关。
已删除手机上的临时 UI XML，停止采集并清理本轮遗留的 logcat 进程。
最终 crash buffer 为空。此结果只表示该 buffer 中未发现崩溃。
其他人的工作区文件、历史报告和日志保持原样。
