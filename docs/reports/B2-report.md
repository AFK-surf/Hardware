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
