<p align="right"><strong>简体中文</strong> · <a href="cyber-lanlan-delivery.md">English</a></p>

# 赛博懒懒初代实机测试版

> 本页保留 v0.1 的交付记录。新版外观见[角色 v0.2 更新](cyber-lanlan-character.zh_CN.md)。

本次修复日期为 2026-10-05，替代此前 DeepSeek 的交付说明。代码基于 `989f746e6ff03fd660839b94e7e81a8d99db18a4`，修复保留在本地，尚未提交或推送。固件、服务和网页应配套使用，目前尚未刷机或完成硬件验收。米家联动继续留待后续，提醒默认关闭。

## 已修复的问题

- 手机输入按浏览器本地时区转换为 UTC；编辑数量未知的记录时，会清空上一笔的数量。
- 网络断开后，重复提交相同内容会返回原记录。如果修改了内容，网页会保留填写，提示再次点击“保存修改”，更新原记录，避免静默丢失或重复新增。
- 恢复同步时，一次读取近期记录、提醒和游标。护照最多缓存 40 条记录，备注使用预览，响应限制在 15 KB 内。极端内容会减少本次缓存条数，完整历史仍留在手机和服务端。详见[同步协议](cyber-lanlan-service.zh_CN.md)。
- 网络正常时，空闲同步间隔为 30 秒；失败后仍会退避重试。
- 提醒保存最近响铃日期，时钟回拨不会让旧日期重新响铃。保存成功后才播放声音。如果时间误调到很远的未来，提醒会暂停到超过已保存日期。
- 固件和界面压力测试统一使用 32 KB LVGL 内存。固件比此前增加 8 KB，测试不再使用此前的 48 KB 配置。

初代包含手机记录、赫赫与羊羊独立账号、护照缓存阅读、伙伴互动、音效和静音。角色仍为占位像素形象，最终外观待确认。服务尚未部署到云端。

## 验证结果与固件身份

| 项目 | 结果 |
| --- | --- |
| Build | PASS：使用 ESP-IDF 5.5.3 和受版本管理的默认配置构建，合并布局和配套调试归档通过校验 |
| Host tests | 部分通过：固件逻辑、资源、网页时间与表单回归、无端口的 SQLite/API 回归通过；完整测试受本地 HTTP 监听权限限制 |
| Device tests | NOT RUN：尚未刷机 |
| Unverified | 实际 Wi-Fi/TLS 内存峰值、屏幕、按键、声音、NVS 持久化、电池、手机到护照的同步时间，以及完整 HTTP 测试和生产部署 |

已尝试完整检查。HTTP 测试在启动服务器时遇到 `PermissionError: [Errno 1] Operation not permitted`，不能视为通过。在具备 ESP-IDF 5.5.3 和 Node.js 的普通终端中补跑：

```bash
./tools/validate.sh
python3 tools/preview_lanlan.py --mode stress
```

主机界面压力测试通过了 1,103 次切页、2,160 次按键和 2,880 次渲染。结束时 LVGL 空闲内存回到 13,432 字节，最低空闲为 10,328 字节。该结果不代表 ESP32 在 Wi-Fi/TLS 工作时的实际内存余量。

- 合并镜像大小为 1,831,568 字节，从 `0x0` 刷写。
- 完整镜像 SHA-256：`2fc9a71d82f6ca43961d0627d70d66a4c4d09ec3f643c0a5632100ad3b4df90e`。
- 配套 ELF SHA-256：`14a07985330bf04deb99fdbd36e4264fb44b93619d2ecc1bf388055c3b874fdf`。
- 嵌入版本为 `989f746-dirty`，请用哈希区分具体构建。
- 调试归档位于 `build/firmware/2fc9a71d82f6ca43961d0627d70d66a4c4d09ec3f643c0a5632100ad3b4df90e/`。

合并刷写会替换现有应用，可能重置 NVS 配置和缓存。不要把仅应用镜像刷到 `0x0`，也不需要例行全片擦除。刷写前另行取得所有者授权。

## 手机与护照联调

实机测试期间临时在电脑上运行服务，手机、电脑、护照应在可以互相访问的网络中，护照连接 2.4 GHz Wi-Fi。在仓库根目录使用 Python 3.9 或更新版本执行：

```bash
export PYTHONPATH="$PWD/services"
export LANLAN_DB="$PWD/.local-data/lanlan.sqlite3"
mkdir -p .local-data
python3 -m lanlan init
python3 -m lanlan serve --host 0.0.0.0 --port 8787
```

只对新数据库执行初始化，并保存终端首次显示的两位照顾者密码。手机打开 `http://<电脑局域网IP>:8787/`；手机上的 `127.0.0.1` 指向手机自身。系统防火墙询问时，允许局域网访问。HTTP 仅用于可信局域网测试。测试期间保持终端运行，电脑停止服务后护照可看缓存，但不能更新。日常独立使用需要后续部署常在线服务。

在网页账号页面创建设备令牌，通过护照 USB 串口控制台以 115200 波特率输入以下命令。替换尖括号内容，输入时不保留尖括号。不要将密码、令牌写入截图或 Git：

```text
lanlan cfg ssid <Wi-Fi名称>
lanlan cfg pass <Wi-Fi密码>
lanlan cfg url http://<电脑局域网IP>:8787
lanlan cfg token <设备令牌>
lanlan cfg tz +08:00
lanlan cfg show
lanlan sync now
```

上下键选择，OK 进入，长按 OK 返回；熄屏后的第一次操作只唤醒屏幕。先从手机新增记录，在护照核对，再修改备注和时间、切换照顾者、检查数量未知的记录。随后测试断网恢复、重启、伙伴音效和静音。护理提醒保持关闭，除非主动测试日程。

## 后续部署

准备真实域名与常在线主机后执行：

```bash
cp deploy/lanlan.env.example deploy/lanlan.env
# 修改域名后再运行：
docker compose --env-file deploy/lanlan.env -f deploy/docker-compose.yml up -d --build
```

数据库初始化、备份和 HTTPS 配置见[部署说明](../../deploy/README.zh_CN.md)。本次修复没有创建付费资源或发布到外部平台。
