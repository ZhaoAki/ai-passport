<p align="right"><strong>简体中文</strong> · <a href="cyber-lanlan-service.md">English</a></p>

# 赛博懒懒服务与移动网页（M0 设计）

本文确定赛博懒懒宠物照顾记录器的第一代服务设计：持久化云服务、移动网页输入界面、记录模型、身份认证、设备同步协议、容量限制和提醒规则。设备应用本身在 [cyber-lanlan.md](cyber-lanlan.md) 中描述；验证步骤与验收映射见 [cyber-lanlan-testing.md](cyber-lanlan-testing.md)。

本文不启用任何被延后的范围：没有米家或摄像头集成，没有远程投喂控制，没有视频，没有手机锁屏推送，没有韩语学习内容，没有 AI 聊天，也没有饥饿、死亡或离家惩罚。虚拟宠物互动永不产生真实照顾记录。

## 1. 本次增量的范围

| 范围内 | 范围外 |
| --- | --- |
| 两位照顾者（赫赫、羊羊）通过手机网页记录喂食、饮水、护理、清洁、散步和其他项目 | 锁屏推送通知 |
| 以服务端持久化作为权威记录存储 | 来自宠物电器的自动事件采集 |
| 每条记录的修订、撤销与审计追踪 | 静默覆盖另一位照顾者的编辑 |
| 使用稳定 ID、版本和增量游标的设备同步 | 远程设备控制 |
| 设备上对近期记录和提醒的有界缓存 | 设备上的无界历史 |
| 初始为禁用、之后由所有者配置的照顾提醒 | 任何形式的预设照顾间隔 |
| CSV/JSON 导出、数据库备份与恢复 | 付费托管资源（延后到预算决策之后） |

## 2. 部署候选方案

这个家庭没有常开电脑，因此手机可见的服务最终必须运行在托管机器上。在预算决策之前，交付物是一个可复现的本地服务加上可部署的配置。

| 候选方案 | 形态 | 说明 |
| --- | --- | --- |
| A. 本地开发（当前默认） | 在开发机上运行 `python3 -m lanlan`，SQLite 文件存放在磁盘上 | 在同一局域网内形成网页加设备的闭环。不是常开部署；只有机器开机时手机才能访问。 |
| B. 单台小型 VPS | `docker compose`，包含服务容器和终止 HTTPS 的 Caddy 反向代理 | 推荐的首个托管目标。一个域名、一个卷、自动证书。需要预算决策。 |
| C. 托管容器平台 | 同一镜像推送到带持久卷的容器主机 | 若平台提供持久磁盘则可接受。仅提供对象存储的平台不适合 SQLite。 |

所有候选方案使用同一镜像、同一环境变量和同一 SQLite 文件布局，因此把 A 升级为 B 只是部署变更。生产环境强制使用 HTTPS；凭据来自环境变量或部署密钥，绝不提交到仓库。当 `LANLAN_ENV=production` 时，如果没有显式数据库路径和安全 cookie 设置，服务拒绝启动。

运行时依赖刻意保持为零个第三方包：仅使用 CPython 3.11 或更新版本的标准库（`http.server`、`sqlite3`、`hashlib`、`hmac`、`secrets`、`json`、`csv`、`zoneinfo`）。移动网页是作为静态文件提供的纯 HTML/CSS/JavaScript；没有构建步骤，也没有打包器。这使锁定文件按设计保持为空，并让服务在任何装有 Python 的机器上可复现。

## 3. 账户、角色与授权

- 每个部署一个**家庭**（户）。它保存显示名称和家庭时区（初始值为 `Asia/Shanghai`，可在设置中修改）。
- 家庭成员是**照顾者**。第一代内置赫赫和羊羊，由初始化命令创建。照顾者权限相同：两人都可以查看、创建、编辑和撤销共享记录。
- 一条记录分别保存**创建者**和**执行者**，因此赫赫可以记录由羊羊完成的散步。编辑绝不静默改写这两个字段；修订历史保留谁在何时改了什么。
- **设备凭据**是护照的独立身份。它可以读取记录、提醒和家庭设置，绝不能创建、编辑或撤销记录，也绝不管理账户。它随时可被撤销；撤销在下一个请求生效。
- 每个记录端点都需要已认证的照顾者会话或设备令牌。未认证请求收到 `401`，并且不返回任何记录内容。来自其他家庭的照顾者收到 `403` 或 `404`；家庭范围限定在每个数据库查询内部实施，而不是在页面层。

### 3.1 密码与令牌处理

| 项目 | 决策 |
| --- | --- |
| 密码存储 | PBKDF2-HMAC-SHA256，210000 次迭代，每个用户 16 字节随机盐，存储为 `pbkdf2_sha256$<iterations>$<salt>$<hash>` |
| 会话令牌 | 32 个随机字节，URL 安全编码；数据库只保存其 SHA-256 摘要；30 天滑动过期 |
| 会话 cookie | `HttpOnly`、`SameSite=Lax`、`Path=/`，部署为 HTTPS 时使用 `Secure` |
| 设备令牌 | 32 个随机字节；以 SHA-256 摘要形式存储，并带设备 id、标签、创建时间、最后出现时间和撤销时间 |
| CSRF | 登录时返回的会话绑定随机令牌；每个改变状态的请求都必须在 `X-Lanlan-CSRF` 中携带它，并且必须带上同源的 `Origin` 请求头 |
| 登录限流 | 按源地址加用户名的失败计数器，采用指数退避和短暂锁定窗口 |
| 密钥材料 | 仅来自环境变量或部署密钥；绝不写入仓库、数据库转储、导出文件或日志行 |

仓库中没有任何示例配置包含真实密码、令牌、设备密钥、照顾记录或家庭照片。

## 4. 记录模型

### 4.1 数据表

```sql
families(id TEXT PRIMARY KEY, name TEXT NOT NULL, timezone TEXT NOT NULL, created_at TEXT NOT NULL)
users(id TEXT PRIMARY KEY, family_id TEXT NOT NULL REFERENCES families(id),
      username TEXT NOT NULL UNIQUE, display_name TEXT NOT NULL,
      password_hash TEXT NOT NULL, created_at TEXT NOT NULL, disabled_at TEXT)
sessions(token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id),
         csrf TEXT NOT NULL, created_at TEXT NOT NULL, expires_at TEXT NOT NULL, last_seen_at TEXT NOT NULL)
devices(id TEXT PRIMARY KEY, family_id TEXT NOT NULL REFERENCES families(id), label TEXT NOT NULL,
        token_hash TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL, last_seen_at TEXT, revoked_at TEXT)
records(seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL, version INTEGER NOT NULL,
        family_id TEXT NOT NULL REFERENCES families(id),
        category TEXT NOT NULL, subitem TEXT, custom_name TEXT,
        occurred_at TEXT NOT NULL, occurred_tz TEXT NOT NULL, time_confidence TEXT NOT NULL,
        created_at TEXT NOT NULL, created_by TEXT NOT NULL, performed_by TEXT NOT NULL,
        amount_value REAL, amount_unit TEXT, duration_minutes INTEGER, note TEXT,
        status TEXT NOT NULL, source TEXT NOT NULL, client_request_id TEXT,
        supersedes_seq INTEGER, revoke_reason TEXT,
        UNIQUE(id, version))
reminders(seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL, version INTEGER NOT NULL,
        family_id TEXT NOT NULL REFERENCES families(id),
        category TEXT NOT NULL, subitem TEXT, custom_name TEXT,
        enabled INTEGER NOT NULL DEFAULT 0, schedule_type TEXT NOT NULL DEFAULT 'daily',
        time_local TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, updated_by TEXT NOT NULL,
        UNIQUE(id, version))
device_acks(device_id TEXT PRIMARY KEY REFERENCES devices(id), cursor INTEGER NOT NULL,
        synced_at TEXT NOT NULL)
idempotency(family_id TEXT NOT NULL, client_request_id TEXT NOT NULL, record_id TEXT NOT NULL,
        created_at TEXT NOT NULL, PRIMARY KEY(family_id, client_request_id))
```

`records` 和 `reminders` 是只追加的修订日志：每次被接受的变更都会插入一行新数据，`version + 1` 且 `id` 相同。`seq` 是**全局唯一且单调递增的变更序号**，由两张表共用的同一个序列分配，并与修订行的插入在同一个事务中完成（服务为此保留一张专用的分配表）。因此对两张表使用同一个游标绝不会跳过任何变更，而两张表各自独立的行 id 序列无法保证这一点。记录的当前状态是其版本号最高的行。这两张表上都没有 `UPDATE`，也没有 `DELETE`，这正是增量游标、审计追踪和删除标记规则彼此一致的原因。统计和列表只读取 `status` 为 `active` 且版本为当前版本的行。

### 4.2 字段语义

| 字段 | 含义 |
| --- | --- |
| `id` | 稳定的 UUIDv4 字符串，跨修订永不改变；设备缓存的键 |
| `version` | 从 1 开始的整数，每次被接受的编辑或撤销都会递增 |
| `category` | `meal`、`water`、`care`、`cleaning`、`walk`、`other` 之一 |
| `subitem` | `care` 的固定子项（`bath`、`grooming`、`teeth`、`comb`、`other`），或 `cleaning` 的预设键；其他情况为 `NULL` |
| `custom_name` | 所有者定义的简短名称，用于 `cleaning` 以及 `care/other` 和 `other`，1 到 12 个字符 |
| `occurred_at` | 事件时间，以秒级精度的 RFC 3339 UTC 存储 |
| `occurred_tz` | 照顾者录入事件时使用的 IANA 时区；用于还原本地墙上时间 |
| `time_confidence` | 照顾者选择或确认时间时为 `trusted`，对明确近似补录时为 `estimated` |
| `created_at` / `created_by` | 服务器时间和提交记录的照顾者 |
| `performed_by` | 实际完成该事项的照顾者；默认为创建者，也可以是另一位照顾者 |
| `amount_value` / `amount_unit` | 可选数量；**`NULL` 表示未知，绝不能渲染或存储为 0** |
| `duration_minutes` | 可选的整数分钟数，目前由 `walk` 使用 |
| `note` | 可选自由文本，最多 200 个字符 |
| `status` | `active` 或 `revoked` |
| `source` | 第一代中始终为 `manual`；该字段的存在是为了让未来的集成无需迁移即可区分 |
| `client_request_id` | 网页提供的重试标识符；在每个家庭内唯一 |
| `supersedes_seq` | 本行所替换修订的 `seq`，首版为 `NULL` |
| `revoke_reason` | 照顾者撤销记录时记录的可选简短原因 |

被撤销的记录是删除标记：它留在日志中，从列表和摘要中消失，并会被下发到设备，使缓存无法将其复活。撤销在历史仍可检查这一意义上可逆，但第一代没有“取消撤销”操作；更正的产物是一条新记录。

### 4.3 类别与单位校验

校验位于模型层（`services/lanlan/model.py`），并作用于每条写入路径，包括设备认证的路径，因此网页中的校验器缺陷不可能产生非法行。

| 类别 | 允许的子项 | 数量 | 时长 | 自定义名称 |
| --- | --- | --- | --- | --- |
| `meal` | 无 | 可选，单位属于 `g`、`ml`、`scoop`、`cup`、`piece`、`bag` | 否 | 否 |
| `water` | 无 | 可选，单位属于 `ml`、`bowl` | 否 | 否 |
| `care` | `bath`、`grooming`、`teeth`、`comb`、`other` | 否 | 否 | 仅 `other` 必填 |
| `cleaning` | 预设键（`ear`、`paw`、`pad`、`litter`、`other`） | 否 | 否 | `other` 必填，预设项可选标签 |
| `walk` | 无 | 否 | 可选，1 到 1440 分钟 | 否 |
| `other` | 无 | 否 | 否 | 必填，1 到 12 个字符 |

额外的服务端规则：`amount_value` 存在时必须大于 0 且至多 9999；`occurred_at` 必须介于服务器时间之前 400 天到之后 1 天之间；`note` 限制为 200 个字符；`performed_by` 必须是同一家庭的成员。违反规则时返回 `422` 和机器可读的字段错误，并且不存储该记录。

## 5. HTTP API

所有端点都位于 `/api/v1` 下，除另有说明外都交换 JSON。错误使用 `{"error": {"code": "...", "message": "...", "field": "..."}}`。照顾者端点需要会话 cookie 以及写入时的 CSRF 请求头；设备端点需要 `Authorization: Bearer <device token>`。

| 方法 | 路径 | 认证 | 用途 |
| --- | --- | --- | --- |
| `POST` | `/auth/login` | 无 | 开始照顾者会话；返回用户、家庭和 CSRF 令牌 |
| `POST` | `/auth/logout` | 会话 | 结束会话 |
| `GET` | `/me` | 会话 | 当前照顾者、家庭时区和家庭成员 |
| `POST` | `/records` | 会话 | 创建记录；`client_request_id` 使重试幂等 |
| `GET` | `/records` | 会话 | 使用 `from`、`to`、`category`、`limit` 和键集游标列出记录 |
| `GET` | `/records/{id}` | 会话 | 当前修订加上完整修订历史 |
| `PATCH` | `/records/{id}` | 会话 | 编辑记录；请求体携带 `expected_version` |
| `POST` | `/records/{id}/revoke` | 会话 | 撤销记录；请求体携带 `expected_version` 和可选原因 |
| `GET` | `/summary/today` | 会话 | 家庭时区下按类别统计的今日数量，以及最新的喂食/饮水/散步 |
| `GET` | `/reminders` | 会话 | 带启用标志、日程类型和时间的提醒列表 |
| `PATCH` | `/reminders/{id}` | 会话 | 启用、禁用或设置某条提醒的时间 |
| `GET` | `/export/records.csv`、`/export/records.json` | 会话 | 完整导出，包含所有修订，包括被撤销的删除标记 |
| `POST` | `/devices` | 会话 | 创建设备凭据；明文令牌只返回一次 |
| `GET` | `/devices` | 会话 | 列出设备及其最后出现状态和撤销状态 |
| `POST` | `/devices/{id}/revoke` | 会话 | 撤销设备凭据 |
| `GET` | `/sync/changes` | 设备 | 游标之后的增量变更 |
| `GET` | `/sync/snapshot` | 设备 | 分页的全量重新同步 |
| `POST` | `/sync/ack` | 设备 | 上报已应用的游标和设备时钟用于诊断 |
| `GET`、`PATCH` | `/profile` | 会话 | 档案页展示的懒懒资料字段 |
| `POST` | `/auth/change-password` | 会话 | 修改当前登录照顾者的密码 |
| `GET` | `/status` | 会话 | 账户页展示的服务状态 |

两个同步响应还会带上 `members`，即该家庭的照顾者列表
`[{"id", "display_name", "username"}]`，使护照能够标注创建者和执行者，而无需持有家庭管理权限。

实现另外保留了一张用于登录限流的 `login_attempts` 表、一条用于档案页的 `pet_profile` 记录，以及提供第 4.1 节所述全局变更序号的 `change_seq` 分配表。这些都属于服务内部实现；上文的记录表和提醒表完全按定义创建。

`GET /` 和 `/assets/*` 从 `web/lanlan/` 提供移动网页。这些文件不包含家庭数据；记录数据只能通过已认证的 JSON API 获取。

### 5.1 写入语义

1. 网页在打开表单时生成 `client_request_id`（UUIDv4），并在同一次提交的重试中保持不变。
2. 服务在一个事务中插入记录和幂等行。重复的 `client_request_id` 会返回先前创建的记录，HTTP 状态为 `200`，并带 `"idempotent_replay": true`，而不是存储第二行。
3. 网页只有在该响应之后才告诉照顾者“已保存到服务”。失败时保留表单内容，并提供使用同一标识符重试。失败绝不会被呈现为成功。
4. `PATCH` 和 `revoke` 需要 `expected_version`。如果已存储的版本更新，服务返回 `409` 以及当前记录，让页面显示冲突并由照顾者选择；绝不静默覆盖。

## 6. 同步协议

服务数据库是权威；护照保留一个有界的近期缓存。

### 6.1 增量变更

```text
GET /api/v1/sync/changes?cursor=<integer>&limit=<1..100>
Authorization: Bearer <device token>

200 OK
{
  "server_time": "2026-10-04T12:00:00Z",
  "timezone": "Asia/Shanghai",
  "utc_offset_minutes": 480, // family timezone offset in effect at server_time
  "cursor": 412,            // cursor to persist with this batch
  "has_more": false,
  "records": [ ... ],       // changed records, ordered by seq, at most `limit`
  "reminders": [ ... ],     // changed reminders in the same seq order
  "revoked": [ ... ]        // stable ids of records that must be deleted from the cache
}
```

- `cursor` 是本批次中包含的最大 `seq`。`cursor=0` 表示“发送全部内容”。
- 记录以缓存使用的紧凑形式下发：
  `{"id","v","cat","sub","name","at","tz","tc","by","perf","amt","unit","dur","note","st","seq"}`，
  其中 `amt` 在未知时被省略，而不是作为 `0` 发送。
- 批次是一致快照：服务在一个事务内读取日志，因此游标不会跳过并发写入。
- `utc_offset_minutes` 是家庭时区在 `server_time` 时刻的偏移。护照没有时区数据库，因此使用该偏移来计算本地时间和提醒实例，并在每次同步时刷新；服务因此会重新计算它，而不是缓存一个固定值。
- 如果 `cursor` 大于最新的 `seq`（恢复了备份、替换了数据库），服务返回 `409 cursor_invalid`；设备随后通过 `/sync/snapshot` 分页获取。
- 第一代中保留是永久的，因此游标不会因清理而过期。过期游标路径用于恢复，而不是日常运行。

### 6.2 全量重新同步

`GET /api/v1/sync/snapshot?offset=<n>&limit=<n>` 按稳定的 `(occurred_at, id)` 顺序返回当前状态的记录和提醒，并带 `has_more`，在结束时返回一个游标。设备只有在最后一页存储完成后才替换缓存。

### 6.3 设备侧规则

- 设备在一次 NVS 提交中持久化新的缓存批次和新的游标；只有该提交成功后游标才前进。
- 失败或被中断的同步保留先前的缓存和游标，页面显示最后一次成功同步时间和数据年龄。
- 离线时设备显示缓存记录、缓存提醒及其时间戳，并保持互动和声音功能可用。
- 冲突以服务为准解决：设备绝不编辑记录。
- “手机上保存的内容在 60 秒内出现在护照上”是一个待上机验证的测量目标，而不是第一代的保证。

## 7. 容量、保留、备份与导出

| 项目 | 决策 |
| --- | --- |
| 设备缓存大小 | 40 条最新记录，加上最多 32 个撤销删除标记，加上 16 条提醒，以唯一的、带 CRC32 校验的二进制块形式存储在 NVS 命名空间 `lanlan` 下：单个条目不超过 160 字节，blob 不超过 8 KB，运行期缓存加暂存缓冲区不超过 16 KB 静态 RAM。设备只保留备注的 48 字节 UTF-8 前缀，完整备注保留在服务端 |
| NVS 分区 | 不变：已跟踪的 `partitions.csv` 保留 24 KB NVS。除非出现经过测量的需要，否则不重新分区，任何此类变更都必须说明其对应用容量和升级数据的影响 |
| 缓存淘汰 | 最旧的记录只从设备缓存中丢弃；服务历史永远不会因淘汰而被删除 |
| 缓存损坏 | 设备丢弃无法读取的缓存并从服务重建，同时保留可见的未保存/未同步警告；服务器数据绝不被触碰 |
| 服务保留 | 所有修订无限期保留；没有静默覆盖或删除 |
| 数据库模式 | SQLite 启用 WAL、`synchronous=FULL`、`busy_timeout=5000`，并启用外键 |
| 备份 | `lanlan-admin backup --out <dir>` 使用 SQLite 在线备份 API，然后校验完整性并在文件旁记录 SHA-256 |
| 恢复 | `lanlan-admin restore --from <file>` 在没有显式标志时拒绝覆盖非空数据库 |
| 导出 | CSV 和 JSON 导出包含每条修订及其创建者、执行者、状态和时间戳；JSON 导出是无损形式 |

## 8. 提醒

- 每条提醒都以禁用且无日程的状态开始。初始化会为喂食、饮水、洗澡、梳毛、刷牙、散步和梳理创建禁用条目。不预设任何照顾间隔，第一代也不会凭空发明一个。
- 第一代唯一支持的日程是 `daily`，其本地时间 `HH:MM` 按家庭时区解释。启用没有时间的提醒会被拒绝并返回 `422`。
- 提醒实例由 `(reminder_id, local_date)` 标识。只有当该实例此前未响过时才允许响铃；设备为每条提醒持久化最后响铃的实例，因此刷新、重启和时钟校正都不会使同一实例响两次。
- 关闭提示或阅读横幅不等于完成。当该本地日期上存在该类别的一条匹配记录时，提醒在显示上才算已满足。提醒页面链接到记录表单，记录仍须显式提交。
- 在同一天更改提醒时间绝不会使该实例第二次响铃。如果新时间仍在将来，该实例可以在新时间响一次。
- 提醒音频默认关闭，可按提醒启用，并且始终被全局静音设置抑制。静音绝不在视觉上隐藏到期状态。
- 设备只在其时钟可信时响铃。时钟未经验证时，它显示缓存提醒及最后数据更新时间，并且不响铃。
- 提醒完成规则和重新排程行为由使用注入时钟的主机测试覆盖。

## 9. 移动网页

| 页面 | 内容与流程 |
| --- | --- |
| 登录 | 用户名和密码、错误文本，认证前没有数据 |
| 概览 | 日期可信时显示今日摘要，否则显示最近记录；最后一次设备同步时间；记录表单的快捷入口 |
| 记录 | 类别和子项选择、发生时间、执行者（默认为已登录的照顾者，可以是另一位）、带单位的可选数量、可选时长、可选备注，带重试的保存反馈 |
| 记录列表 | 按时间倒序的列表，带日期和类别筛选，键集分页 |
| 详情 | 完整记录、带创建者和时间的修订历史、编辑和撤销操作、`409` 时可见的冲突结果 |
| 提醒 | 每项的启用开关、时间选择器、提醒声音开关，并明确说明关闭不等于完成 |
| 档案 | 宠物名字、生日、品种、体重和自由备注；第一代没有照片上传 |
| 账户与导出 | 修改密码、照顾者列表、带撤销的设备列表、CSV 和 JSON 导出、服务状态 |

布局面向宽度为 360 到 430 CSS 像素的手机浏览器，使用大触控目标，保持保存按钮可达，并在提交失败时显示未保存状态。

## 10. 时间处理

- 所有存储的时间戳都是 RFC 3339 UTC；显示使用家庭行中的家庭时区，因此设置中的时区更改不会重复或丢失提醒。
- 服务在每个同步响应中设置 `server_time`，使设备能够检测时钟偏差并显示未验证时钟警告，而不是凭空编造日期。
- 设备自身不记录任何内容；记录的 `occurred_at` 始终来自明确的照顾者操作或明确的补录。
- 当服务应答时，网页上的日期被视为可信；设备上只有在 SNTP 同步成功之后才如此。若没有，设备显示“最近记录”而不是“今天”摘要。

## 11. M0 之后跟踪的待办事项

| 项目 | 状态 |
| --- | --- |
| 托管提供商与预算 | 未确认；先本地运行，容器配置已就绪 |
| 设备配网通道 | 设计：在网页设置页面生成设备凭据，并通过 USB 串口控制台粘贴到护照。Wi-Fi 凭据以相同方式录入。如果串口路径被证明不实用，蓝牙配网仍是有文档记录的备选方案 |
| 角色外观 | 占位像素美术，明确标记为非最终美术，直到所有者批准冻结的精灵集 |
| 60 秒同步目标 | 用于硬件验证的测量目标 |
| 手机锁屏推送 | 按任务说明延后 |
