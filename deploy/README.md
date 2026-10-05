<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Deploying the Cyber Lanlan service

This directory holds a two-container deployment: the service image built from
`services/lanlan/Dockerfile` and a Caddy reverse proxy that terminates HTTPS.
Both images are public; no paid or third-party service is involved. The service
uses only the Python standard library, so the image has no dependency install
step and no lock file.

## What the deployment provides

| Item | Detail |
| --- | --- |
| `lanlan` container | Runs `python3 -m lanlan serve` as an unprivileged user (uid 10001) with the SQLite database on the `lanlan-data` volume |
| `caddy` container | Terminates TLS, redirects HTTP to HTTPS, forwards to `lanlan:8787` on an internal bridge network, and stores certificates in the `caddy-data` volume |
| Published ports | Only 80 and 443, from Caddy. The service port is never published |
| Persistence | `lanlan-data` keeps the database; deleting the volume deletes the family data |

## Steps

1. Copy the environment placeholder and edit it:

   ```bash
   cd deploy
   cp lanlan.env.example lanlan.env
   ```

   Set `LANLAN_DOMAIN` to the real host name. Keep `LANLAN_ENV=production` and
   `LANLAN_SECURE_COOKIES=1`; the service refuses to start in production
   without them unless `LANLAN_ALLOW_INSECURE=1` is set deliberately.

2. Point the domain's DNS record at the host before the first start, because
   Caddy requests the certificate during startup.

3. Build and start:

   ```bash
   docker compose --env-file lanlan.env up -d --build
   docker compose --env-file lanlan.env logs -f caddy
   ```

4. Create the family, the two caregivers and the seven disabled reminders. The
   initialization command prints the generated passwords exactly once:

   ```bash
   docker compose --env-file lanlan.env exec lanlan python3 -m lanlan init
   ```

   Add `--password hehe=...` style options to choose the passwords yourself.
   Record them in a password manager; nothing stores the plaintext.

5. Generate a device credential for the passport and keep the printed token:

   ```bash
   docker compose --env-file lanlan.env exec lanlan python3 -m lanlan device-create --label passport-a
   ```

6. Open `https://<LANLAN_DOMAIN>/` in a phone browser and sign in.

## Backups

The database is a single SQLite file on the `lanlan-data` volume. The backup
command uses the SQLite online backup API, verifies the copy and writes a
SHA-256 sidecar:

```bash
docker compose --env-file lanlan.env exec lanlan python3 -m lanlan backup --out /data/backups
```

Copy `/data/backups` off the host with `docker compose --env-file lanlan.env cp` (or a volume backup)
so the copy does not share the disk with the original. Restore refuses to
overwrite a non-empty database unless `--force` is passed:

```bash
docker compose --env-file lanlan.env exec lanlan python3 -m lanlan restore --from /data/backups/<file> --force
```

## HTTPS requirements

- Plain HTTP must never carry a login: Caddy redirects `http://` to `https://`
  and the session cookie is marked `Secure` in production.
- The session cookie is `HttpOnly` and `SameSite=Lax`; the `X-Lanlan-CSRF`
  header plus a same-origin `Origin` header are required for every
  state-changing request, which is also why the page must be served from the
  same host as the API.
- The service trusts `Host` and `Origin` as seen by Caddy; do not add another
  proxy that rewrites them incompatibly.
- Keep the service port on the internal network. If it must be reachable
  directly for debugging, do not expose it to the internet without TLS.

## Security notes

- No secret is committed: `lanlan.env` holds deployment settings and stays out
  of version control; device tokens exist in plaintext only at creation time.
- The service never logs record content, request bodies, passwords, tokens or
  query strings. Each request logs method, path without query, status and
  duration.
- Logins are throttled per source address plus username with exponential
  backoff, and device tokens are rejected on every write and account endpoint.
- The database file is owned by the unprivileged service user. Protect the
  volume and any backup copy with host-level permissions and encryption.

## Local development without Docker

The same code runs directly on the host:

```bash
cd services
LANLAN_DB=./lanlan.sqlite3 PYTHONDONTWRITEBYTECODE=1 python3 -m lanlan serve
```

Development uses `http://127.0.0.1:8787`; production requires HTTPS as above.
