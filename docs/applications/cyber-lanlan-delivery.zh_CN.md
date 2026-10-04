<p align="right"><strong>简体中文</strong> · <a href="cyber-lanlan-delivery.md">English</a></p>

# 赛博懒懒第一代交付

本文是第一代赛博懒懒宠物照顾记录器的交付记录：交付了什么、如何运行、验证了什么，以及还有什么未决。冻结的设计见 [cyber-lanlan.md](cyber-lanlan.md) 和 [cyber-lanlan-service.md](cyber-lanlan-service.md)；验收映射见 [cyber-lanlan-testing.md](cyber-lanlan-testing.md)。

## 1. 交付状态

| 项目 | 取值 |
| --- | --- |
| 仓库 | https://github.com/ZhaoAki/ai-passport |
| 分支 | `feature/cyber-lanlan` |
| 基线 | `99058004449a76c313375e238436b4642e36886c`（`feature/korean-learning`） |
| 保留分支 | `feature/korean-learning` 未被改动；本分支仅在自己的目录树中移除韩语应用 |
| 固件 | 合并镜像 `build/FoloToy-AI-Passport-full.bin`，应用镜像以及匹配的 ELF/MAP 保留在 `build/firmware/<sha256>/` 下的内容寻址归档中 |
| 服务 | `services/lanlan/`，仅使用 CPython 标准库，SQLite 存储 |
| 移动网页 | `web/lanlan/`，无构建步骤，无第三方资源 |
| 部署 | `deploy/`（`docker-compose.yml`、`Caddyfile`、环境变量示例） |

阶段覆盖：M0 设计文档，M1 服务与移动网页，M2 使用有界缓存的护照同步，M3 伙伴互动、声音和由所有者配置的提醒（默认全部禁用），M4 构建、产物和本交付记录。

## 2. 第一代的功能

- 两位照顾者通过移动网页记录喂食、饮水、护理（洗澡、梳毛、刷牙、梳理）、清洁、散步和其他项目，带明确的发生时间、可以是另一位照顾者的执行者、可选的带单位数量、可选的散步时长和可选备注。未设置的数量保持未知，绝不存储或显示为 0。
- 服务端是权威：只追加修订，带创建者、执行者、版本、撤销删除标记、幂等提交、可见的编辑冲突、CSV 和 JSON 导出、在线备份与恢复。
- 护照保留一个有界的近期缓存（40 条记录、32 个删除标记、16 条提醒，位于一个受 CRC32 保护的 blob 中），使用单个全局游标增量同步，显示同步状态、电量、最后一次成功同步时间和数据年龄，离线时从缓存工作，并提供第二套重新设计的界面，包含像素伙伴、短音效、全局静音和提醒列表。
- 提醒初始为禁用且不预设任何间隔；所有者在网页中启用一个每日时间。一个实例最多响一次，关闭提示不等于完成。
- 护照不能创建、编辑或撤销记录，虚拟互动永不产生照顾记录。

范围之外且未构建：米家或电器集成、摄像头通知或视频、远程投喂、手机锁屏推送、韩语学习内容、AI 聊天，以及任何饥饿、死亡或离家惩罚。

## 3. 如何运行

### 3.1 服务与移动网页（开发）

```bash
cd services
python3 -m lanlan init --password hehe=<password> --password yangyang=<password>   # creates the family and 7 disabled reminders
python3 -m lanlan serve --host 127.0.0.1 --port 8787
```

在手机上打开 `http://127.0.0.1:8787/`（同一网络），并以 `hehe` 或 `yangyang` 登录。配置基于环境变量：`LANLAN_DB`、`LANLAN_HOST`、`LANLAN_PORT`、`LANLAN_ENV`、`LANLAN_SECURE_COOKIES`、`LANLAN_TIMEZONE`、`LANLAN_PBKDF2_ITERATIONS`、`LANLAN_SESSION_DAYS`。详情见 [services/lanlan/README.md](../../services/lanlan/README.md)。

> 开发服务器不是常开部署：只有运行它的机器开机时，手机才能访问它。托管部署需要那个仍然待定的预算决策。

### 3.2 部署

```bash
cp deploy/lanlan.env.example deploy/lanlan.env     # set the public domain
docker compose -f deploy/docker-compose.yml up -d  # service + Caddy with automatic HTTPS
```

见 [deploy/README.md](../../deploy/README.md)。生产环境强制使用 HTTPS；如果生产配置没有显式数据库路径和安全 cookie，服务拒绝启动。

### 3.3 护照固件

```bash
source <path-to-esp-idf-5.5.3>/export.sh
./tools/validate.sh --firmware          # builds and verifies the merged 0x0 image
idf.py -p <port> flash monitor          # optional incremental development flashing
```

没有硬件时，合并镜像就是交付物。刷写是一项单独的操作，需要明确批准；在空白设备上，把 `build/FoloToy-AI-Passport-full.bin` 刷写到偏移 `0x0`。

### 3.4 护照首次设置

设备出厂时不带 Wi-Fi 凭据，也不带设备凭据。在网页中创建该凭据（“账户与导出”页面的“设备凭据”），然后通过护照的 USB 串口控制台（与日志使用同一端口）粘贴以下内容：

```text
lanlan cfg ssid <wifi name>
lanlan cfg pass <wifi password>
lanlan cfg url <https://host>
lanlan cfg token <device token>
lanlan cfg tz +08:00
lanlan cfg show
lanlan sync now
```

`url` 必须是 `http(s)://` 且不带结尾斜杠；对于局域网开发服务，可以接受纯 `http://`，并在状态页面上标记为不安全。`cfg show` 绝不完整打印密码或令牌。面向所有者的操作对照表见 [cyber-lanlan.md](cyber-lanlan.md#1-pages-and-controls)。

## 4. 如何验证

```bash
./tools/validate.sh --static     # repository checks, firmware host tests, the service test suite
./tools/validate.sh --firmware   # ESP-IDF build, merged image, layout check, debug archive
./tools/validate.sh              # complete gate
python3 tools/preview_lanlan.py  # render every screen at 240x320 with host LVGL plus a glyph audit
python3 tools/archive_firmware.py verify build/firmware/<sha256>
```

服务测试套件和固件主机测试都注册在 `tools/validate.sh` 中，因此本地和 CI 运行相同的检查。

## 5. 本次交付的验证结果

```text
Build:        PASS
Host tests:   PASS
Device tests: NOT RUN
Unverified:   on-glass rendering, Wi-Fi association and TLS behaviour, real key feel and
              backlight timing, audio playback, reminder ringing across a real day boundary,
              the 60-second save-to-passport goal, the measured RAM peak with Wi-Fi and TLS
              active, cache rebuild on a real NVS partition, battery behaviour, and the
              owner's decision on the final character art
```

确切的命令、测试数量、镜像大小和哈希记录在提交信息以及上面的章节中；逐条验收的视图见 [cyber-lanlan-testing.md](cyber-lanlan-testing.md)。

## 6. 已知限制与偏差

| 项目 | 状态 |
| --- | --- |
| 角色美术 | 精灵集是占位美术，已在生成的源码和 [assets/README.md](../../assets/README.md) 中如此标记。像素颗粒和最终外观在冻结之前仍需所有者批准。 |
| 备注预览 | 护照保留备注的 48 字节 UTF-8 预览，并渲染明确的截断标记；完整备注保留在服务和手机上。 |
| 照顾者标签 | 护照从服务的同步负载中学习姓名；如果无法解析某个姓名，它会显示中性标签 未知，而不是原始 id 或槽位编号。 |
| 按键提示 | 底部提示行使用 ASCII 形式 `UP/DN OK HOLD=BACK`；冻结的字符串表中没有通用提示的条目。 |
| 显示超时 | 调暗（30 秒）和熄屏（90 秒）可通过配置 blob 和控制台配置，而不是通过设置行配置。 |
| 提醒日程 | 在一个本地时间每日重复是本代唯一支持的日程。 |
| 照片 | 原始家庭照片既不上传也不提交；档案页面只有文本字段。 |
| 托管 | 本地服务加上就绪的容器配置；没有创建付费资源，也没有做出托管决策。 |
| 延迟的实际情况 | “手机上保存，60 秒内在护照上可见”的目标是一个测量目标，而不是保证。 |

## 7. 待所有者决定的事项

1. 托管：是否在小型 VPS 或容器主机上运行服务，以及域名。
2. 角色美术：批准或替换占位精灵集，然后冻结资源；如果调色板变化，重新生成字体/声音。
3. 设备测试：通过 USB 连接护照，以便执行设备检查清单，并让 `Device tests` 从 `NOT RUN` 变为真实结果。
4. 提醒策略：启用哪些项目、在哪些本地时间，以及是否开启提醒声音。
5. 上游仓库是否应接收来自本分支的 PR，或者该 fork 继续作为交付目标。
