<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 澜澜服务

支撑澜澜手机页面与护照设备的常驻记录服务。它为一个家庭保存吃饭、喝水、护理、清洁、
散步等记录，保留全部修订，托管手机网页，并实现
[cyber-lanlan-service.md](../../docs/applications/cyber-lanlan-service.md)
中约定的设备同步协议。

## 零第三方依赖

服务只使用 Python 标准库（`http.server`、`sqlite3`、`hashlib`、`hmac`、`secrets`、
`json`、`csv`、`zoneinfo`、`argparse`），没有依赖清单、没有虚拟环境、也没有需要维护的
锁文件。它可以在 CPython 3.9 到 3.13 的干净解释器上运行，测试套件同样不需要任何安装。

手机页面是纯 HTML、CSS 和 JavaScript 静态文件，没有构建步骤、没有打包器、没有框架，
也不引用任何 CDN。

因此从部署角度看，运行依赖只有 CPython 和一个可写的 SQLite 数据库目录。

## 目录结构

| 路径 | 作用 |
| --- | --- |
| `services/lanlan/__main__.py` | `python3 -m lanlan <子命令>` 入口 |
| `services/lanlan/config.py` | 环境配置与生产模式检查 |
| `services/lanlan/db.py` | SQLite 表结构、PRAGMA 与事务辅助 |
| `services/lanlan/model.py` | 校验、时间处理与行数据整形 |
| `services/lanlan/auth.py` | 口令散列、会话、设备令牌、CSRF、限流 |
| `services/lanlan/api.py` | JSON 接口路由与查询 |
| `services/lanlan/server.py` | 多线程 HTTP 服务与静态文件托管 |
| `services/lanlan/export.py` | 导出全部修订的 CSV 与 JSON |
| `services/lanlan/admin.py` | 初始化、设备、备份、恢复与导出的底层实现 |
| `services/lanlan/tests/` | 通过真实 HTTP 请求驱动的 unittest 测试 |
| `services/lanlan/run_tests.sh` | 仓库门禁使用的测试运行脚本 |
| `web/lanlan/` | 手机页面（`index.html`、`styles.css`、`app.js`） |
| `deploy/` | `docker compose` 部署加 Caddy HTTPS 代理 |

## 本地启动

在 `services` 目录下运行，这样无需安装即可导入包：

```bash
cd services
LANLAN_DB=./lanlan.sqlite3 PYTHONDONTWRITEBYTECODE=1 python3 -m lanlan serve
```

也可以只用环境变量：

```bash
cd services
export LANLAN_DB=./lanlan.sqlite3
python3 -m lanlan serve            # http://127.0.0.1:8787/
```

`python3 -m lanlan --help` 会列出全部子命令：

| 子命令 | 作用 |
| --- | --- |
| `serve` | 启动 HTTP 服务 |
| `init` | 创建家庭、两位看护人和七条默认关闭的提醒 |
| `device-create` | 创建设备凭据并只打印一次令牌 |
| `device-list` | 列出设备及其创建时间、最近使用时间与吊销状态 |
| `device-revoke` | 吊销一个设备凭据 |
| `backup` | 在线备份，附带完整性校验与 SHA-256 校验文件 |
| `restore` | 把备份恢复回配置的数据库路径 |
| `export` | 导出含全部修订的 CSV 或 JSON |
| `check-config` | 打印已解析的配置并校验 |

## 环境变量

| 变量 | 默认值 | 含义 |
| --- | --- | --- |
| `LANLAN_DB` | `./lanlan.sqlite3` | SQLite 数据库路径；生产模式下必须显式设置 |
| `LANLAN_HOST` | `127.0.0.1` | 监听地址（容器反代后使用 `0.0.0.0`） |
| `LANLAN_PORT` | `8787` | 监听端口 |
| `LANLAN_ENV` | `development` | `development` 或 `production` |
| `LANLAN_SECURE_COOKIES` | 生产模式为 `1`，否则 `0` | 给会话 Cookie 加 `Secure` |
| `LANLAN_WEB_DIR` | 检出目录下的 `web/lanlan` | 静态页面目录 |
| `LANLAN_PBKDF2_ITERATIONS` | `210000` | 口令散列强度，仅在测试中调低 |
| `LANLAN_SESSION_DAYS` | `30` | 会话滑动有效期（天） |
| `LANLAN_TIMEZONE` | `Asia/Shanghai` | 初始化时使用的家庭时区 |
| `LANLAN_FAMILY_NAME` | `Lanlan family` | 初始化时使用的家庭名称 |
| `LANLAN_ALLOW_INSECURE` | 未设置 | 设为 `1` 时允许生产模式缺少显式数据库路径或安全 Cookie |

在 `LANLAN_ENV=production` 下，除非显式设置 `LANLAN_ALLOW_INSECURE=1`，服务会要求显式
的 `LANLAN_DB` 和 `LANLAN_SECURE_COOKIES=1`，否则拒绝启动。查看解析结果：

```bash
cd services
python3 -m lanlan check-config
```

`check-config` 会打印配置并以 `check-config: OK` 结尾，或在 `FAIL:` 行中说明问题；它
永远不会打印密码或令牌。

## 首次初始化

```bash
cd services
export LANLAN_DB=./lanlan.sqlite3
python3 -m lanlan init
```

`init` 会创建一个家庭、两位看护人（使用家中成员的显示名）以及七条提醒——全部默认关闭
且没有时间。它只会打印一次生成的密码：

```text
initialized database: /absolute/path/lanlan.sqlite3
family: Lanlan family (<uuid>, Asia/Shanghai)
reminders created: 7 (all disabled)
caregiver passwords are shown once; store them in a password manager now:
  hehe  <generated>
  yangyang  <generated>
```

也可以自行指定密码：

```bash
python3 -m lanlan init --password hehe='example-password-one' \
                       --password yangyang='example-password-two' \
                       --timezone Asia/Shanghai --family-name 'Example family'
```

如果数据库里已经有家庭，`init` 会拒绝继续，除非加上 `--force`。仓库和数据库里都不会
出现真实密码，只保存 PBKDF2 散列值。

## 设备凭据

设备凭据可以读取记录、提醒和家庭设置，但永远不能创建、修改或作废记录，也不能管理账号。

```bash
cd services
python3 -m lanlan device-create --label passport-a
# device id: <uuid>
# label: passport-a
# token (shown once, store it on the passport now): <token>

python3 -m lanlan device-list
python3 -m lanlan device-revoke <device-id>
```

明文令牌只在那一次输出中出现，数据库保存的是它的 SHA-256 摘要。吊销会在设备的下一次
请求时生效。同样的操作也可以在手机页面的“账号与导出”中完成。

## 备份、恢复与导出

这两条命令都可以在服务运行时操作在线数据库。`backup` 使用 SQLite 在线备份 API，对副本
执行 `PRAGMA integrity_check`，并在旁边写入 `.sha256` 文件：

```bash
cd services
python3 -m lanlan backup --out ./backups
# backup written: ./backups/lanlan-<stamp>.sqlite3
# sha256: <digest>
# checksum file: ./backups/lanlan-<stamp>.sqlite3.sha256

python3 -m lanlan restore --from ./backups/lanlan-<stamp>.sqlite3 --force
```

没有 `--force` 时，`restore` 拒绝覆盖非空数据库；恢复完成后会再次校验副本。替换文件期间
请先停止服务，否则正在处理的请求会失败。

导出始终包含全部修订，包括已作废的墓碑记录；JSON 是无损格式：

```bash
python3 -m lanlan export --format json --out ./lanlan-export.json
python3 -m lanlan export --format csv  --out ./lanlan-export.csv
python3 -m lanlan export --format csv  --out -            # 输出到标准输出
```

已登录的看护人也可以在页面上通过 `/api/v1/export/records.json` 和
`/api/v1/export/records.csv` 下载同样的文件。

## 测试

在仓库根目录执行：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s services/lanlan/tests -t .
```

也可以使用封装脚本，它会固定工作目录并自动降低 PBKDF2 强度：

```bash
./services/lanlan/run_tests.sh
```

测试会在 `127.0.0.1` 上用临时端口和临时数据库启动真实服务，通过 HTTP 驱动它，并在结束后
清理全部文件。测试会设置 `LANLAN_PBKDF2_ITERATIONS=1000` 以保证速度，生产环境保持默认
的 210000。仓库门禁运行同一套测试；需要在 `tools/validate.sh` 的 `run_static_checks`
中加入的那一行是：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s services/lanlan/tests -t .
```

## 部署

`deploy/docker-compose.yml` 会构建本服务，并把它放在负责终止 HTTPS 的 Caddy 反向代理
之后。包含 DNS、证书、备份与恢复的完整步骤见
[deploy/README.zh_CN.md](../../deploy/README.zh_CN.md)。简要流程：

```bash
cd deploy
cp lanlan.env.example lanlan.env      # 修改：设置 LANLAN_DOMAIN
docker compose up -d --build
docker compose exec lanlan python3 -m lanlan init
docker compose exec lanlan python3 -m lanlan device-create --label passport-a
```

## HTTPS 要求

- 用明文 HTTP 提供页面只适用于开发环境。生产环境由反向代理提供 HTTPS，会话 Cookie 带
  `Secure` 标记，因此服务在 `LANLAN_ENV=production` 且没有 `LANLAN_SECURE_COOKIES=1`
  时会拒绝启动。
- 页面与接口必须同源：CSRF 校验会比较 `Origin` 请求头与浏览器使用的 `Host`。
- 在没有 HTTPS 的不可信网络上不要登录，会话 Cookie 是看护人权限的唯一凭据。

## 修改规则

记录同时保存记录人（`created_by`，写入第一条修订时固定）和执行人
（`performed_by`，默认等于记录人）。修改时可以重复提交原来的执行人，但不能改成别人：
提交不同的值会返回 `422`，错误字段为 `performed_by`，否则合并逻辑就会改写历史。
`PATCH` 与 `revoke` 都必须带 `expected_version`；版本过期会返回 `409` 并附上当前记录，
页面因此可以展示冲突而不是静默覆盖。已作废的记录是墓碑：它保留在日志和导出中，从列表与
汇总里消失，也不能再修改；更正的做法是重新记录一笔。

## 表结构与同步游标

`records.seq` 与 `reminders.seq` 都来自同一张分配表（`change_seq`），并且与修订行的
INSERT 处在同一个事务里，两个只追加日志因此共享同一条严格递增的变更顺序。这正是
`GET /sync/changes` 不丢数据的前提：游标是本批次的最高 `seq`，之后的任何写入都不可能落到
设备已经保存的游标之下。规范里的表定义没有改动，分配表是新增的，每条修订仍然显式写入
`seq`。

`PRAGMA user_version` 记录布局版本，当前为 `2`。由布局 1 写入的数据库——两张表各自使用
独立的 AUTOINCREMENT 计数器、可能重复使用同一个 `seq` 值——会在第一条打开它的命令中于
一个事务内完成迁移：两张表的全部修订会被重新编号为唯一的 1..N 顺序（保持各自表内相对
顺序，冲突时记录优先），`supersedes_seq` 修订链接同步重映射，不会丢失任何行或链接。
升级之后，设备需要用 `/sync/snapshot` 重新同步一次，因为旧编号下的游标已经没有意义；
当游标比日志更新时，`/sync/changes` 会返回 `409 cursor_invalid`。

## 设备同步响应

`GET /api/v1/sync/changes` 与 `GET /api/v1/sync/snapshot` 返回相同的顶层结构：

| 字段 | 含义 |
| --- | --- |
| `server_time` | RFC 3339 UTC 服务器时间，供设备判断时钟偏差 |
| `timezone` / `utc_offset_minutes` | 家庭时区，以及 `server_time` 时刻的偏移 |
| `cursor` / `has_more` | 本批次最高 `seq`，以及是否还有后续批次 |
| `records` / `reminders` | 按 `seq` 顺序的紧凑变更行 |
| `revoked` | 设备必须从缓存中删除的稳定 id |
| `members` | 家庭中启用的照顾者：`id`、`display_name`、`username` |

`members` 在每一页、每一次响应中都完全一致，设备只需读取一次并缓存 `id` 到显示名的映射，
不必再按“首次出现顺序”自行推断，否则两位照顾者可能被对调。其顺序为创建时间、用户名、
id，因此即使在初始化时两位照顾者写在同一秒内，列表依然稳定。这也是设备凭据能看到的唯一
账号视图：不包含任何口令材料、会话数据、邮箱地址或其他账号字段。忽略该字段的设备无需任何
改动即可继续工作。

## 安全说明

- 口令：PBKDF2-HMAC-SHA256，每个用户 16 字节随机盐，210000 次迭代，保存格式为
  `pbkdf2_sha256$<iterations>$<salt>$<hash>`。
- 会话：32 字节随机值，仅保存 SHA-256 摘要，30 天滑动有效期，Cookie 带 `HttpOnly`、
  `SameSite=Lax`、`Path=/`，生产模式再加 `Secure`。
- 设备令牌：32 字节随机值，保存摘要，可随时吊销，只读。
- CSRF：必须在 `X-Lanlan-CSRF` 中携带与会话绑定的令牌，并且请求必须带同源 `Origin` 头。
- 登录限流：按来源地址加用户名，指数退避并带短暂锁定窗口。
- 家庭隔离：每个查询都按会话或设备凭据解析出的家庭过滤，其他家庭看不到任何内容，只会
  得到 `403`/`404`。
- 日志：只记录方法、去掉查询串的路径、状态码和耗时。记录内容、请求体、密码、令牌和查询
  字符串都不会写入日志。
- 数据库：SQLite 启用 WAL、`synchronous=FULL`、`busy_timeout=5000` 与外键；记录和提醒都是
  只追加的修订日志，不会有静默覆盖或删除。
- 永远不要提交真实数据库、家庭记录、设备令牌或照片。
