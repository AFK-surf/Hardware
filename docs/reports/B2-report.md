# B2 报告：安卓「添加设备」

2026-10-05 · comma-android 分支 `codex/add-device-ble`（worktree `~/Desktop/AFK/comma-android-add-device`，提交 `96a5a6e`），未推送、未开 PR。基于 main `3e435f7`。

## 结论

设置 › 设备 › 添加设备：扫描 → 配对码 → 选 Wi-Fi → 铸造两个凭证并写入 → 等后端报在线 → 完成。设备详情页可移除（吊销两个凭证 + 删除设备 + 提示长按 10 s 重置）。按 `docs/ble-provisioning.md` 最新版（`connect_url` 必填、原样写入）实现。

**没有在真机 + T-Circle 上跑过**。验证到的程度：JVM 单测全绿、lint 绿、模拟器上走了界面（见下）。BLE 配对、Wi-Fi、写凭证、上线这一段需要真机和板子。

## 用户定的两个决定

| 项 | 决定 | 实现 |
|---|---|---|
| voice key 有效期 | 长期，解绑时吊销 | 不传 `expires_at`（后端视为永不过期） |
| connector token 首次注册时限 | 10 分钟 | 请求体带 `registration_expires_in_seconds: 600`（**后端目前不生效**，见「后端缺口」1） |

## 流程（顺序遵守契约）

1. 扫描：只按 service UUID 过滤；同名设备只显示一次（每次配对会话换随机地址）。15 s 没找到时提示屏幕应显示 "Hello"/"Pairing mode"。
2. 配对：连接 → `requestMtu(517)` → 发现服务 → 读 `info`。读失败（0x05/0x0F/0x89）就等系统配对（监听 `ACTION_BOND_STATE_CHANGED`），用户在系统弹窗输入设备屏幕上的 6 位码。之后订阅 `status`、`wifi_scan`。
3. Wi-Fi：分页收集扫描结果（乱序、去重、新一轮扫描会重置），可重新扫描、改设备名；标注「只显示 2.4 GHz 网络」。有密码网络弹密码框（8–64 位才可提交），开放网络直接加入。
4. 设置：写 `wifi_credentials` → 等同一 SSID 的 `wifi_up` → **这时才铸造** connector token（先）和 voice key（后，名字为 `<设备名> (<device_id>)`）→ 写 `credentials`（`connect_url` 原样、`server`/`group_id` 从 voice key 的 `readiness_url` 解析）→ 等 `saved`。`saved` 后保持 BLE（设备 30 s 内自己关），能收到 `connector_failed`。
5. 完成：后端 `GET devices/:id` 每 2 s 轮询到 `connected` 即完成，并把后端设备名改成用户起的名字（固件 metadata 的 hostname 固定是 "T-Circle"）。提示「轻点屏幕给 Comma 打电话试试」。

## 错误处理 / 不留孤儿凭证

| 情况 | 处理 |
|---|---|
| 密码错 / 找不到网络 / 超时 | 回到 Wi-Fi 页显示原因，**没有铸造任何凭证** |
| 铸造失败 | voice key 失败时回收已铸的 token；`connect_url` 不是 ws(s) 也先回收；403/409(key 满 20 个)/网络 分别提示 |
| 铸造中 BLE 断开或用户离开 | 铸造请求跑完（NonCancellable），结果到达时直接吊销 |
| 写入被拒 / `save_failed` / `connector_failed` | 吊销两个凭证 |
| 写入后断开或 20 s 没等到 `saved` | 不确定是否已存：等上线 90 s；上线则确认，否则吊销 |
| 已 `saved` 但 90 s 没上线 | 不吊销，显示「快好了，稍后在设备里查看」 |
| 吊销时没网 | 本地加密记录保留（未确认），下次打开设备列表时重试 |
| App 中途被杀 | 铸造后立即写本地记录（未确认）；打开设备列表时对账：设备连过 → 确认；超过 15 分钟从没连上 → 吊销 |

本地记录：`noBackupFilesDir/comma-devices.bin`，用会话的 Keystore AES-GCM 密钥加密（独立 purpose）。记 connector token 原文是因为后端只存 hash，吊销必须给原文。代码里 token/key 都不进日志和 `toString`（有测试）。

## 解绑

设备详情页「移除设备」→ 确认框说明要长按 10 s 重置 → 吊销 token（本机设置的才有原文）→ 删 voice key（有 id 用 id，否则按名字后缀 `(<device_id>)` 查找）→ `DELETE devices/:id` → 本地记录删除 → 提示重置。吊销失败则什么都不删。电脑类 Connector 只显示状态，不给移除。

## 后端缺口（需要后端同事）

1. **首次注册时限没透传**：`comma_web/router.ex` 的 `connector_token_attrs` 只取 `name/alias/expires_in_seconds/scope/stable_device_id`，丢掉了 `registration_expires_in_seconds`。所以现在铸的 token 实际没有注册时限。只要把这个键加进 attrs 就行（`SalixEnv.ConnectorTokens` 已支持）。
2. **删设备不吊销 token**：`Comma.Devices.remove` 调 `remove_environment(..., nil)`，token hash 是 nil 就跳过吊销。别的手机设置的 T-Circle 被移除后，token 仍有效，设备重置前可能重新连上（确认框里如实说了）。建议后端按 device_id 吊销。
3. 每个 group 最多 20 个 voice key，长期 key 每台设备占一个；满了提示「先移除一台设备」。

## 测试

- JVM：`TCircleProtocolTest`（UUID、分页扫描、status/info 解析、ATT 错误、Wi-Fi 规则、credentials JSON：`connect_url` 原样、7 个字段、长度/前缀/512 B 限制、voice URL 解析、key 命名、不打印密钥）、`AddDeviceFlowTest`（22 个场景：全流程、错密码、旧网络的 wifi_up、铸造失败、铸造后断开/取消→吊销、写入被拒、save_failed、不确定保存→上线确认/超时吊销、已保存慢上线、connector_failed、过期计时器、扫描去重）、`HomeDevicesTest`（铸造顺序与回滚、本机/他机解绑、离线吊销重试、对账、跨账号隔离）、`SettingsPagesTest` 加两条（设备列表 + 添加入口；移除确认后请求顺序）。
- 全量 `testDevDebugUnitTest` 1135 个，除一次 `ServerTaskRepositoryTest` 计时偶发（单独重跑通过，和本改动无关）全绿；`lintDevDebug` 绿。
- 模拟器（Pixel_10_Pro AVD，staging 已登录账号）：设备页正确列出 staging 上的设备（A2 那块 T-Circle 显示在线）；T-Circle 详情页和移除确认框（点了取消，**没有真删**）；添加设备：权限说明 → 系统权限弹窗 → 扫描页 → 15 s 提示；关蓝牙 → 「打开蓝牙」→ 系统弹窗 → 自动进入扫描；横竖屏重建后流程原样接上。无崩溃。模拟器的虚拟蓝牙看不到板子，后面的步骤没法在模拟器上走。
- 注意：模拟器上原来装的是别的分支的 staging 调试包（`0.1.190-stage4-call-ui`），我用 `install -r -d` 覆盖成了本分支的包（数据保留）。

## 真机验证要做的

需要你把手机（`23112153D000003`）和 T-Circle 给我时再做，我不会自己装手机：

1. 板子进配对（出厂态或长按 BOOT 3 s 松开）→ 扫到 → 系统弹窗输码 → Wi-Fi 列表。
2. 故意输错密码一次 → 回 Wi-Fi 页且后端无新 token/key。
3. 正确密码 → 上线 → 名字改成功 → 点屏通话。
4. 中途关蓝牙/拔电，确认凭证被吊销（看 `GET voice-api-keys`）。
5. 移除 → 板子长按 10 s 重置。
6. 注意板子现在是 A2 用的 token（staging 上那台 T-Circle）；重新配网会换成新凭证。

## 已知小问题

- 若在蓝牙刚打开、适配器还没就绪时开始扫描，扫描会静默失败停在「正在查找」，需返回重进（模拟器上没遇到）。
- 隐藏 SSID 无法手动输入（固件也不列出隐藏网络）。
- `removeBond` 没调（契约说可选）。

## B2b：设备详情页「Allow operations」开关

2026-10-05 · 同分支 `codex/add-device-ble`，提交 `8bdd09b`（在 `96a5a6e` 之上），未推送。

### 做了什么

- 设置 › 设备 › 某台设备：新增一行 "Allow operations"，副标题 "Lets Comma act through this device. Off keeps it read-only."。**所有 Connector 设备都显示**（电脑也有）。
- 状态来自 `GET devices/:id` 的 `allows_operations`；详情页打开时重新拉一次这台设备（404 则从列表移除）。
- 切换调 `PUT devices/:id/access`，体 `{"allow_operations": bool}`，以返回的 `allows_operations`（设备自己的回答）为准。等回答期间开关先显示目标值，再点无效。
- **打开**前弹一次确认："Allow operations?" / "Comma will be able to send requests on your local network through this device." / Allow。每次打开都确认，关闭不确认。
- 离线：开关置灰，下方显示 "Device is offline."。
- 失败：开关回到原值，下方红字说明原因：

| 后端返回 | 显示 |
|---|---|
| `device_access_unavailable` | The device didn’t confirm the change. Try again. |
| `disconnected` | Device is offline.（同时把这台设备标成离线，开关置灰） |
| `timeout` | The device didn’t answer in time. Try again. |
| 403 / 404 | 账号无权限 / 设备已不在工作区 |
| 网络 / 其他 | 复用网络提示 / "Comma couldn’t change the device’s access." |

（后端 `comma_error` 对这几个原子都是 400 + `{"error": "<原子>"}`，所以按 `error` 字段区分。）
- 开关是新写的 `SettingsSwitch`（app 里原来没有开关控件，二选一设置都是 On/Off 菜单）：轨道 `bg.brandSolid` / `border.primary`，白色圆点；圆点位移用 `springSpatial`，轨道颜色 150 ms softInOut。无障碍上整行报告为 Switch，带勾选状态。
- 中文、英文文案都加了。

### 测试

- 新 `HomeDeviceAccessApiTest`（5）：PUT 路径/方法/请求体、解析回答、缺字段报错、`device_access_unavailable` 识别、GET 读 `allows_operations`（缺省为只读）、错误码映射。
- `HomeDevicesTest` +5：等回答时显示目标值且重复点击忽略；未确认→回退+原因、下次重试清掉；`disconnected`→标离线；断网→回退；单设备刷新更新状态、消失则移除。
- `SettingsPagesTest` +2（Robolectric）：打开要确认、关闭不确认，PUT 体正确；离线设备开关置灰、显示提示、点击不发请求。
- 全量 `testDevDebugUnitTest` 1147 个全绿；`lintDevDebug` 没有新问题。

### 真机（手机 `23112153D000003`，T-Circle 在线，staging）

经用户同意 `adb install -r` 装上本分支 staging debug 包。每一步都**强制停止 app 再重新进入**设备详情（内存里的状态全部清空，值来自后端）：

1. 初始：关（截图 `B2b-screens/b2b-0-off.png`）。
2. 打开 → 弹出确认 → Allow → 开关打开，没有报错；重启后读回仍是**开**（`b2b-1-on.png`）。
3. 关闭（不弹确认）→ 重启后读回是**关**。
4. 离线的 Mac（Computer）：开关置灰 + "Device is offline."（`b2b-3-offline.png`）。

T-Circle 最后的状态是**关**（只读），和开始时一样。板子本身没动。

### 说明

- "confirmation once" 我理解成每次打开只问一次，不是每台设备一辈子只问一次；如果要的是后者再改。
- 失败原因 `timeout`/`disconnected` 这两个分支只有单测覆盖，真机没造出来过（没有拔板子）。

## B2d：B2c 真机测试里的安卓问题修复

2026-10-06 · `codex/add-device-ble` 提交 `5dd4559`（基于 `8bdd09b`，未推送）· 手机 `23112153D000003` 已装 staging debug 包。T-Circle 当时被 B1c 占用（在通话中），**需要板子的项都没上真机验**。

| 项 | 改动 | 真机 |
|---|---|---|
| A1 配对请求是通知 | ① 文案改为：手机会弹出配对请求；如果它出现在通知里，先点 **Pair & connect** 再输入码（中英文都改）。② `pair()` 在第一次加密读之前，若未绑定就由 app 自己调 `createBond()` 并等绑定完成（原来是加密读失败后才调）。依据：AOSP 设置在"发起绑定的 app 在前台"时倾向弹对话框而不是通知。 | ❌ 没验（要板子进配对模式）。是否真变成对话框未知；两种情况文案都对 |
| A2 "Working…" 图标不转 | 当前步骤那一行的图标换成任务列表的 `TaskStatusIconView`（进行中图标，2 s 一圈匀速，只在可见且系统动画开启时转；DESIGN 里加载指示器匀速旋转是弹簧规则的例外）。 | ❌ 没验（要走到 Setting up） |
| A3 密码明文 | 根因：`TextFieldDialog` 先设 `inputType` 再设 `isSingleLine = true`，后者装上 `SingleLineTransformationMethod`，把密码遮罩顶掉了。改为先单行再设类型；密码框右侧加显示/隐藏按钮（Central `IconEyeOpen`/`IconEyeSlash`，切换时光标位置不变，有无障碍描述）。 | ❌ 没验（要走到 Wi-Fi 页） |
| A4 不自动聚焦 | 原来在 `post{}` 里 `showSoftInput`，此时对话框窗口往往还没拿到焦点，调用被忽略。改为对话框窗口带 `SOFT_INPUT_STATE_ALWAYS_VISIBLE`（新参数 `SettingsDialog(keyboard = true)`），构建时输入框先 `requestFocus()`。改名对话框也受益。 | ❌ 没验 |
| A5 列表在线滞后 | 见下 | 部分 |
| 图标 | Comma 硬件改用 Central `IconSpeaker`（`ic_settings_comma_device`），列表、扫描结果、完成页都换了；已登记 `IconSources`。 | ✅ 设备列表里 Comma-5D28 和 Mac/登录设备区分开了（`B2d-screens/devices-icon.png`） |

### A5 结论：是 app 的问题

- **app**：设备列表只在 `DevicesPage` 创建时拉一次。从设备详情页/添加设备页返回时，列表页还在栈里，不会重新拉，所以 B2c 里那次（进添加设备页试配对 → 板子掉线 → 返回）显示的是进页面时的旧值。
- **后端**（读代码 `salix_web/connector_socket.ex`，未实测）：每 15 s 检查一次，45 s 没收到设备 heartbeat 就关连接，`terminate` 把记录改成 `disconnected`。所以后端最迟约 60 s 就会报离线，不会到 5 分钟。
- **修复**：`SettingsPage` 加 `onTopChanged(top)`（`PageHost` 压栈/出栈时调用）。列表页重新到顶时立刻刷新；在顶且 app 在前台时每 15 s 刷新；被盖住时不轮询。
- **没做到**：提示要求的"同一时刻对比 app 和 `GET …/devices/:id`"没做。板子在 B1c 手里（通话中，串口确认在线，app 显示 Online，一致），没法让它掉线；app 的 token 在 Keystore 里加密，也拿不出来直接调 API。下次板子空闲时：拔掉板子，每 10 s 记一次 API `status`/`disconnected_at` 和列表显示，预期两者都在 ≤60 s 内变成离线。如果 API 本身超过 60 s 还是 `connected`，就是后端问题，交给后端。

### 测试

- `SettingsPagesTest` +3（Robolectric）：列表被盖住时不轮询、回到顶部显示离线、在屏幕上 15 s 内跟上变化；密码默认遮罩、按钮切换且光标不动、输入框有焦点、窗口请求键盘；普通输入框没有显示按钮。
- 全量 `testStagingDebugUnitTest` 1150 个：1 个失败是 `ServerTaskRepositoryTest`（任务流，没动过），单独重跑通过，属负载下超时；`IconSourcesTest` 通过；`verify-scroll-usage.py` 通过。
- A2 旋转、A1 `createBond` 没有 JVM 测试（纯 UI / 依赖系统蓝牙）。

### 下次板子空闲时验证

1. 板子进配对模式（按 BOOT 3–10 s）→ 添加设备：看系统是**对话框**还是通知；文案是否对得上。
2. Wi-Fi 页点网络：键盘自动弹出、密码遮罩、眼睛按钮切换。
3. Setting up 页：当前步骤图标在转。
4. A5：拔板子，按上面的办法对比 app 和 API。

### B2d 补充：对照 B1c 新配网行为（中途断开会丢掉试连的 Wi-Fi）

按 `docs/ble-provisioning.md`（4647a42）核对 app，**不需要改流程**：
- 选 Wi-Fi、连 Wi-Fi、铸凭证这三段里只要 BLE 断开，流程都直接判 `DISCONNECTED` 失败（铸好的凭证在到达时吊销）。"Try again" 从扫描重新开始，连接 → 配对 → **Wi-Fi 页重新选网输密码** → 重新写 `wifi_credentials`。app 不会自动重连，也没有只写 `credentials` 的路径。
- 重新配对后、重发 Wi-Fi 之前，设备回到原网络发出的 `wifi_up` 会被忽略，不会拿它去铸凭证。
- 写完 `credentials`、但还没收到 `saved` 时断开：照旧等新 `device_id` 上线 90 s。如果其实已经保存，设备会在新网络上线，app 确认凭证；如果没保存，设备会回原网络，新 id 不会上线，超时后吊销凭证。新行为下这个判断仍然成立。
- 新增 JVM 测试 `reconnectingAfterADropSendsWifiAgainEvenIfTheDeviceWasOnIt`（提交 `d817975`）：到 `wifi_up` 后断开 → 重试 → 重新配对后停在 Wi-Fi 页；原网络的 `wifi_up` 不触发铸凭证；重新 Join 才会再写 Wi-Fi，新的 `wifi_up` 之后才铸凭证。

### B2d 真机验证（2026-10-06 12:01–12:53，手机 `23112153D000003`，板子固件 B1c `c18ceb8-dirty`）

| 项 | 结果 |
|---|---|
| A1 | ✅ 改了实现（提交 `bc50372`）。之前那版只提前调 `createBond()`，在这台 Nothing 手机上**没用**：板子一连上就主动请求配对（`connected, asking for pairing`），抢在 app 之前。系统收到的是 consent 类请求（`pairingVariant=3`），判定不在前台弹窗，于是只发通知（`pairing-notification.png`）。没人点的话 30 s 后 SMP 超时（`status 13` / `SMP_CONN_TOUT`），连续 3 次都是这样。新做法：连接前让 app 发起一次经典蓝牙扫描并立即停止。Android 只在"手机刚扫描过设备"时把配对请求弹成前台对话框。实测有了这一步就变成系统对话框 "Pair with Comma-5D28?"（`pairing-dialog.png`），点 Pair 后是输码框（`pairing-code.png`）。连接 → 板子显示码 2 s，输码 → `paired` 12 s。提前 `createBond()` 那段代码留着，无害。 |
| A2 | ✅ Setting up 页当前步骤图标在转（录屏取帧，每 1/8 s 一帧，相邻两帧方向差 22.5°，即 2 s 一圈；`setting-up-spin-frames.png`）。 |
| A3 | ✅ 默认遮罩，眼睛按钮可切换显示/隐藏（`password-masked.png`、`password-shown.png`）。 |
| A4 | ✅ 对话框一打开键盘就弹出（`mInputShown=true`），键盘还识别出是密码框（出现 Passwords 建议）。 |
| A5 | ✅ 拔板子：12:18:34 USB 口消失（串口最后一行 12:18:32），12:19:38 列表在屏幕上自己变成 Offline，没碰手机，用时 64 s。后端：第一次拔板 12:01:05–12:01:34，设备页 12:06:15 显示 "Last active 4 minutes ago"，可见 `disconnected_at` 在 12:01:15–12:02:15 之间，即后端约 1 分钟内就标离线，符合 45 s 心跳超时。**不是后端问题。** |
| 图标 | ✅（见上） |
| 错误密码 + B1c 新行为 | ✅ 错误密码 6 s 后 `wifi_failed: wrong_password` → 板子 `back to the stored network`；app 回到 Wi-Fi 页，显示红字 "Wrong password for "ToEverything-IOT"."（`wrong-password.png`）。 |
| 完整流程 | ✅ 正确密码：`wifi_up` 4 s → 写入凭证 1 s → `saved` → `connector_up` 7 s → app 显示 "All set · Online"（`done.png`）。板子 credentials 被轮换：新设备 **`dev_KokfkxljPn6yuBAjq3ucJA`**。 |

注意 / 遗留：
- **配对时限太紧**（B1 方向）：从板子请求配对到用户点 Pair，以及显示码后到输完码，都只有 30 s。对话框 / 通知没被注意到就失败。建议板子不主动发安全请求，让手机读加密特征时再触发配对；或者把时限放宽。app 侧已经尽量让请求弹成对话框。
- 板子上一个设备 `dev_DYYhXrF9yQ6RjBadR9Qslw`（B1c 配的）还留在工作区，离线。它的凭证不是这台手机铸的，app 吊销不了，需要的话手动删。
- 板子仍是只读（Allow operations 关）。
- 测试期间临时打开了"充电时保持亮屏"，测完已恢复（`stay_on_while_plugged_in=0`）。
