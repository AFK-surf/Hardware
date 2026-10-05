# T-Circle 家庭网关 + 通话音箱：规划

2026-10-05 · 公司产品（Comma），参照 Meta 开源的 Muse Home Link（`~/Desktop/AFK/muse-gadget-sdk`）

## 目标

T-Circle（ESP32-S3）只做两件事：

1. **Connector**：常驻家里，替云端 Comma agent 在局域网收发 HTTP/UDP，取代用户 Mac 上的 Connector（Mac 睡眠/重启就断）。
2. **通话音箱**：点一下和 Comma 实时通话（`comma.voice.v1`）。**已完成**，见提交 `81297a7`。

设备本身不懂任何家电协议，也不做语音识别；协议知识在云端 skill 里，加新家电不用改固件。

验收的终点：**关掉 Mac 上的 Connector，对 T-Circle（或手机）说「关灯」，灯照样关掉。**

## 结构

```
                  ┌────────── Comma 云端 (Salix) ──────────┐
                  │ Router agent + skills   GPT-Live 语音   │
                  └──▲───────────────────────▲─────────────┘
     /v1/connect     │ Connector 连接         │ /v1/agent-groups/:g/voice/sessions
     (connector token)│                       │ (voice key, salix_vk_)
                  ┌──┴───────────────────────┴──┐
                  │        T-Circle (ESP32)      │──局域网 HTTP/UDP──▶ 灯、空调…
                  └──────────────▲──────────────┘
                                 │ BLE 配对（首次设置）
                        安卓 Comma app（已登录）
```

通话里控制家电不需要新开发：GPT-Live 没有函数工具，靠 client delegation 交给 Router agent，Router 调 Connector，结果用 `voice.say` 念回来。这条链路用手机 + Mac Connector 已实测可用。

## 后端已有的东西（读代码结论）

| 能力 | 位置 | 说明 |
|---|---|---|
| Connector 协议 | `systems/apps/salix_web/lib/salix_web/connector_socket.ex`；Go 参考实现 `systems/connector/salix-connect/main.go` | 设备外连 `wss://…/v1/connect`，消息 `connected` / `heartbeat` / `metadata` / `request` → `response`/`error`（同 `id`） |
| `http_request` | `main.go:4283` | GET/POST/PUT/PATCH/DELETE，**不限内网地址**（云端自己的 `web.http_request` 禁内网） |
| 权限闸门 | `main.go:4040-4054`、`device_access.go:143`；`comma_core/lib/comma/devices.ex:72-93` | 默认只读，用户在「设置 › 设备」打开操作权限；**闸门由 Connector 自己执行** |
| connector token | `POST /v1/comma/workspaces/:ws/connector-token`（`comma_web/router.ex:2046, 4689-4712`）；`salix_env/connector_tokens.ex` | 组级 token 默认**不过期**；可设首次注册时限；返回 `device_id` |
| voice key | `POST /v1/comma/workspaces/:ws/voice-api-keys` | 安卓 `VoiceCredentials.kt` 已在用 |
| skill | `resources/salix-system-files/skills/<id>/SKILL.md`；miniskill（`activation: per-message`） | 按消息自动挑选，适合「提到灯就加载米家 skill」 |
| 给设备发 RPC 的工具范例 | `salix_agent/lib/salix_agent/tools/dependency_installations.ex` | `EnvDispatch.request(agent_id, %{device_id:, environment_id:}, method, params)` |

缺的：UDP、局域网扫描、直接暴露设备 `http_request` 的 agent 工具、BLE 配对、任何 IoT skill。

## 三条线

### A. 固件 Connector（主线，`Hardware/t-circle-s3`）

| 步骤 | 内容 | 验收 |
|---|---|---|
| **A1** | 电脑上写 Python 最小 Connector，连本地开发环境，写清协议必填字段；顺带确认「米家加密包在云端哪算」 | 出现在设备列表，能回一个 `http_request` |
| **A2** | 固件最小 Connector（token 暂放 `secrets.h`），与语音连接并行，断线重连 | T-Circle 出现在「设置 › 设备」 |
| **A3** | `http_request` + 目的地白名单 + `device_access` 闸门 | agent 经 T-Circle 读到局域网 HTTP |
| **A4** | `udp_request` + 米家灯端到端 | 关 Mac Connector，「关灯」生效 |
| **A5** | 局域网扫描（ARP/mDNS/SSDP，参考 Muse `net_discovery.c`） | agent 列出网络里的设备 |

### B. 配对（可与 A 并行）

| 步骤 | 内容 |
|---|---|
| **B1 固件** ✅ 2026-10-05 已合 main（B1b 板上验收，报告 `docs/reports/B1-report.md`） | 未配对时 BLE 广播 `Comma-XXXX`；GATT：Wi-Fi 列表、写 Wi-Fi、写凭证（server、group、connector token、voice key）、状态通知；**圆屏显示 6 位配对码**（LE Secure Connections passkey）；凭证存 NVS；长按 10 s 恢复出厂 |
| **B2 安卓** | 设置 › 设备 › 添加设备：扫描 → 配对码 → 选 Wi-Fi → app 申请 connector token + voice key 写入设备 → 等设备上线 |
| **B3** | 去掉 `secrets.h`；解绑时吊销两个凭证 |

### C. 后端（Cue PR）

| 内容 | 时机 |
|---|---|
| **C1** 放宽 4410（2 s → 8 s 可配）和 4400（允许卡顿后补发） | 现在（交给后端开发同事） |
| **C2** Connector 协议加 `udp_request`；新 agent 工具 `device.lan_request`（HTTP/UDP）；「家庭网关」skill | A3/A4 前（交给后端开发同事） |
| **C3** 米家等 skill（加密在云端算，T-Circle 只发包） | A4 |

## 顺序

```
A1 ─▶ A2 ─▶ A3 ─▶ A4 ─▶ A5
            ▲      ▲
C1 (现在)   C2 ────┘ C3
B1 ─▶ B2 ─▶ B3（并行，B3 等 A2）
```

## 默认决定（有异议再改）

1. **测试环境**：协议调试用本地开发环境（`make dev`），固件联调用 staging。
2. **加密协议的包在后端用 Elixir `:crypto` 算**（2026-10-05 定，A1 报告：云端没有常驻 Python）。每种加密协议（米家 miIO、格力、涂鸦本地）在后端写一个编解码器，具体命令和读回仍写在 skill 里；T-Circle 只发包。
3. **语音凭证**：先给每台设备发长期 voice key（解绑吊销），后端一次性 ticket（`codex/voice-session-ticket`，未合）上线后再换。
4. **B 线并行**，由另一个 agent 做。

## 共享约束

- **板子只有一块**（`/dev/cu.usbmodem1101`，MAC `98:a3:16:f5:5d:28`）：同一时间只能一个 agent 烧录；烧录前先问用户。
- 办公室 Wi-Fi 用 2.4 GHz 的 `ToEverything-IOT`（`ToEverything` 只有 5 GHz）；回家要改回 `Zhen`。
- **凭证走 NVS**（BLE 配网写入，`device_config_get()` 是唯一入口）。`main/secrets.h` 现在只是开发覆盖，必须定义 `COMMA_DEV_SECRETS 1` 才生效；没有它照样能编译。板子现在已用 staging 凭证配好网（T-Circle，`dev_M3Z6mxvKhQSYEwyYTber-g`）。
- **内部 RAM 很紧**：通话中最低 14.4 KB（B1 后，A2 时 23 KB）；配对模式里 Connector 在线时最低 2.8 KB。A3 加 HTTP client 前必须先腾内存。
- 固件 IDF 6.0.2：`. ~/.espressif/tools/activate_idf_v6.0.2.sh`。
- Git 身份 `aaaa-zhen <m502002313@gmail.com>`。
- 报告写到 `docs/reports/<步骤>-report.md`，不要追加进 prompt 文件。

## 通话现状（已提交 `81297a7`）

点击通话/挂断、发送独立锁、积压 ≤300 ms、TCP 发送缓冲 24 KB、SACK、200 ms 抖动缓冲、拨号音、logo 只在通话时动。
剩余断线（4410/4400）根因是办公室 Wi-Fi 下行卡 2–4 s，由 C1 解决。`pcmu_8k` 已实现但关闭：staging 用该格式时模型完全无回应（C1 prompt 的 C 项在查）。
