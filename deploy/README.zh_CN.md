<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 澜澜服务部署说明

本目录提供一个两容器部署：由 `services/lanlan/Dockerfile` 构建的服务镜像，以及负责终止
HTTPS 的 Caddy 反向代理。两个镜像都是公开镜像，不涉及付费或第三方服务。服务只使用
Python 标准库，因此镜像没有依赖安装步骤，也不需要锁文件。

## 部署内容

| 项目 | 说明 |
| --- | --- |
| `lanlan` 容器 | 以非特权用户（uid 10001）运行 `python3 -m lanlan serve`，SQLite 数据库放在 `lanlan-data` 卷上 |
| `caddy` 容器 | 终止 TLS、把 HTTP 跳转到 HTTPS，并在内部 bridge 网络上转发到 `lanlan:8787`，证书存放在 `caddy-data` 卷上 |
| 对外端口 | 只有 Caddy 的 80 和 443，服务端口不对外发布 |
| 持久化 | `lanlan-data` 保存数据库；删除该卷即删除家庭数据 |

## 操作步骤

1. 复制环境变量模板并修改：

   ```bash
   cd deploy
   cp lanlan.env.example lanlan.env
   ```

   把 `LANLAN_DOMAIN` 改成真实域名，并保持 `LANLAN_ENV=production` 与
   `LANLAN_SECURE_COOKIES=1`；除显式设置 `LANLAN_ALLOW_INSECURE=1` 外，服务在生产模式下
   缺少这两项会拒绝启动。

2. 首次启动前把域名的 DNS 记录指向该主机，因为 Caddy 会在启动时申请证书。

3. 构建并启动：

   ```bash
   docker compose up -d --build
   docker compose logs -f caddy
   ```

4. 创建家庭、两位看护人和七条默认关闭的提醒。初始化命令只会打印一次生成的密码：

   ```bash
   docker compose exec lanlan python3 -m lanlan init
   ```

   也可以用 `--password hehe=...` 这样的参数自行指定密码。请立刻记入密码管理器，服务端
   不会保存明文。

5. 为设备生成凭据，并保存打印出的令牌：

   ```bash
   docker compose exec lanlan python3 -m lanlan device-create --label passport-a
   ```

6. 用手机浏览器打开 `https://<LANLAN_DOMAIN>/` 登录。

## 备份

数据库是 `lanlan-data` 卷上的单个 SQLite 文件。备份命令使用 SQLite 在线备份 API，校验
副本并写入 SHA-256 校验文件：

```bash
docker compose exec lanlan python3 -m lanlan backup --out /data/backups
```

请用 `docker compose cp`（或卷备份）把 `/data/backups` 复制到主机之外，避免副本与原文件
共用同一块磁盘。恢复默认拒绝覆盖非空数据库，必须显式加 `--force`：

```bash
docker compose exec lanlan python3 -m lanlan restore --from /data/backups/<文件> --force
```

## HTTPS 要求

- 明文 HTTP 不得承载登录：Caddy 会把 `http://` 跳转到 `https://`，生产模式下会话 Cookie
  带 `Secure` 标记。
- 会话 Cookie 为 `HttpOnly` 且 `SameSite=Lax`；每个写操作都必须同时带上 `X-Lanlan-CSRF`
  请求头和同源的 `Origin` 请求头，这也是页面必须与接口同域的原因。
- 服务直接采用 Caddy 传入的 `Host` 与 `Origin`，不要再加一层会改写这两个头的代理。
- 服务端口只保留在内部网络。若为调试需要直连，也不要无 TLS 暴露到公网。

## 安全说明

- 仓库中不提交任何密钥：`lanlan.env` 保存部署配置且不进入版本控制；设备令牌只在生成时
  以明文出现一次。
- 服务不会记录记录内容、请求体、密码、令牌或查询字符串。每个请求只记录方法、去掉查询串
  的路径、状态码和耗时。
- 登录按来源地址加用户名做指数退避限流；设备令牌在所有写接口和账号接口上都会被拒绝。
- 数据库文件属于非特权服务用户。请用主机级权限与加密保护该卷以及任何备份副本。

## 不使用 Docker 的本地开发

同一份代码可以直接在主机上运行：

```bash
cd services
LANLAN_DB=./lanlan.sqlite3 PYTHONDONTWRITEBYTECODE=1 python3 -m lanlan serve
```

开发环境使用 `http://127.0.0.1:8787`；生产环境必须按上面的要求使用 HTTPS。
