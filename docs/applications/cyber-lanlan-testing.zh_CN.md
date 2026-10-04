<p align="right"><strong>简体中文</strong> · <a href="cyber-lanlan-testing.md">English</a></p>

# 赛博懒懒验证计划（M0）

本文把第一代验收标准映射到具体、可复现的检查，并说明哪些部分可以在当前环境中执行。它所验证的设计见 [cyber-lanlan-service.md](cyber-lanlan-service.md) 和 [cyber-lanlan.md](cyber-lanlan.md)。

交付报告始终分开四个字段：

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

开发期间没有可用设备，因此每个上机项目都以 `Device tests: NOT RUN` 形式报告，并明确列出待检查项；构建成功绝不作为硬件验证呈现。

## 1. 验收映射

| ID | 检查项 | 本仓库中的证据 | 当前状态 |
| --- | --- | --- | --- |
| A01 | 手机页面能记录第一代的每个项目，两位照顾者都能使用 | `services/lanlan/tests/test_records_api.py` 与 `test_web_assets.py` 通过真实 HTTP 覆盖每个类别、单位和网页视图；移动端布局仍需在浏览器中以 390x844 检查 | API 自动化；手机视觉检查待做 |
| A02 | 创建者、执行者和修订可区分；未知数量绝不会变成零 | `services/lanlan/tests/test_records_api.py`（创建者与执行者、修订链、`amount_value` 保持 `NULL`），`tests/test_lanlan_record.c` 与 `tests/test_lanlan_caregiver.c` 覆盖设备渲染路径 | 自动化 |
| A03 | 冲突可见、重试不重复、撤销不重现 | `services/lanlan/tests/test_concurrency.py` 与 `test_change_sequence.py`（带当前版本的 409、`client_request_id` 重放、删除标记随游标下发）加上 `tests/test_lanlan_cache.c` 覆盖删除标记处理 | 自动化 |
| A04 | 未认证身份和其他家庭的身份无法读取记录；设备凭据权限受限 | `services/lanlan/tests/test_authz.py` 与 `test_config_auth_security.py`（匿名 401、其他家庭 403/404、设备令牌在所有写入与账户端点上被拒绝、CSRF 与来源校验） | 自动化 |
| A05 | 已保存数据在重启后仍然存在；写入失败时保留照顾者的输入 | `services/lanlan/tests/test_persistence.py` 针对同一数据库文件重启服务；`test_web_assets.py` 断言表单保留输入并在重试时复用同一请求 id | 自动化 |
| A06 | 同步达到约定延迟；离线时显示缓存和数据年龄 | `services/lanlan/tests/test_sync_protocol.py` 与 `test_change_sequence.py` 覆盖游标顺序、分页完整性和批次一致性；60 秒目标与离线行为需要硬件 | 协议自动化；上机检查待做 |
| A07 | 损坏的缓存可以重建，分页有界，服务器记录不受影响 | `tests/test_lanlan_cache.c`（损坏、被截断和版本不匹配的数据块、暂存提交失败、淘汰、失败时游标不前进） | 自动化 |
| A08 | 伙伴互动不创建真实记录，且不残留任何韩语入口 | `tests/test_lanlan_model.c`（互动永不发出记录操作）；本分支已移除韩语应用源码、测试、工具与资源，`main/CMakeLists.txt` 不再链接任何韩语资源 | 自动化 |
| A09 | 中文和动态文本遵循约定策略，关键字段不被截断 | `tests/test_lanlan_assets.py`（字形清单与固定字符串匹配）与 `tests/lanlan_ui/` 主机渲染环节，后者用真实的 `lanlan_ui.c` 渲染每个 240x320 屏幕并审计字形覆盖 | 主机渲染自动化；真实屏幕渲染待做 |
| A10 | 提醒初始为禁用、可由所有者配置，并且在刷新、重启或时钟变化时绝不响两次 | `tests/test_lanlan_reminder.c` 使用注入时钟和已持久化的已触发实例；`services/lanlan/tests/test_reminders_api.py` 覆盖默认为禁用以及启用/禁用规则 | 自动化 |
| A11 | 三按键的短按和长按、静音和熄屏唤醒行为一致，且不会误提交 | `tests/test_lanlan_model.c` 的按键序列（包括先唤醒后释放），加上已有的 BSP 按键测试 | 逻辑自动化；真实按键手感待做 |
| A12 | 网络、音频和动画同时运行且无泄漏 | 仅主机侧生命周期测试；上机时用堆和最大块日志进行页面切换和事件循环，应用在启动时和每次同步后都会打印这些日志 | 设备测量待做 |
| A13 | 导出与生效记录一致、备份可恢复、历史不被静默覆盖 | `services/lanlan/tests/test_export_backup.py`（CSV 和 JSON 与数据库比对、备份加恢复往返、修订历史保留） | 自动化 |
| A14 | 完整构建通过，且固件、ELF、MAP 和分区文件对应同一个提交 | `./tools/validate.sh` 加上对内容寻址归档执行 `python3 tools/archive_firmware.py verify <bundle>` | 固件门禁已通过；M4 复跑 |
| A15 | 源码中没有凭据、家庭记录或未经授权的照片；资源来源有记录 | `tools/check_repo.py` 密钥扫描、`.gitignore` 对数据库和部署密钥的规则，以及对 `assets/README.md` 和提交内容的人工审查 | 自动化扫描加人工审查 |

## 2. 如何运行检查

服务、网页和协议测试（不需要第三方包）：

```bash
python3 -m unittest discover -s services/lanlan/tests -t . -v
PYTHONPATH=services python3 -m lanlan check-config
```

固件主机逻辑测试和仓库检查：

```bash
./tools/validate.sh --static
```

在已激活的 ESP-IDF 5.5.3 环境中运行完整门禁：

```bash
./tools/validate.sh
```

`tools/validate.sh` 仍是唯一入口：新增的 C 和 Python 测试注册在其中，而不是放在第二条流水线里，因此本地和 CI 行为不会漂移。

## 3. 自动化测试覆盖的故障注入

| 场景 | 注入 |
| --- | --- |
| 重复提交 | 同一个 `client_request_id` 被提交两次，包括并发提交 |
| 并发编辑 | 两位照顾者用相同的 `expected_version` 修补同一条记录；恰好一方胜出，另一方收到当前记录 |
| 同步中断 | 批次已应用，但模拟游标提交失败；缓存必须保留先前的游标和内容 |
| 缓存损坏 | 解码一个被截断的数据块和一个版本不匹配的数据块；缓存必须重建，且不触碰其他 NVS 键 |
| 时钟变化 | 注入时钟跨提醒实例向前和向后跳变；没有实例响两次 |
| 数据库重启 | 服务进程被停止，并针对同一文件重启；每条已保存记录仍可读取 |
| 已撤销记录 | 一个已撤销的 id 在后续批次中再次下发；它不得重新出现在列表或设备上 |

## 4. 自动化未覆盖的内容

- 真实屏幕渲染、色彩、240x320 下的可读性和像素美术缩放。
- Wi-Fi 关联、TLS 握手时间、射频稳定性，以及 Wi-Fi、TLS、界面和音频同时活跃时的组合 RAM 峰值。
- 硬件上的真实按键手感、背光行为，以及先唤醒后释放的边界情况。
- 提醒音频和“保存到护照 60 秒内可见”的延迟目标。
- 实际使用中的电池行为以及任何调暗或熄屏时序。
- 所有者对照片和美术的批准。
