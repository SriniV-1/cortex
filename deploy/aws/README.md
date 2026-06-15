# Deploying Cortex on AWS (single Graviton EC2)

A from-scratch runbook to put Cortex online at an HTTPS URL on one arm64 EC2
instance running the full stack (server + PostgreSQL + Redis) behind Caddy,
which handles TLS automatically.

**Cost:** a new AWS account's free plan ($100–200 in credits, 6 months) fully
covers this (~$14/month of usage). After the credits expire it's ~$14/month, or
tear it down. Everything here is portable — the same files run on any arm64
Docker host (e.g. Oracle Cloud's always-free VM) if you later want $0 forever.

What's in this directory:

| File | Purpose |
|------|---------|
| `docker-compose.prod.yml` | The stack: Caddy → cortex server → Postgres + Redis |
| `Caddyfile` | Reverse proxy + automatic Let's Encrypt TLS (and WebSocket passthrough) |
| `.env.example` | Template for your domain, email, and DB password |
| `bootstrap.sh` | One-time instance setup (Docker, swap, clone) |
| `load-data.sh` | One-time historical data load (~4.7M events) |

---

## 1. Create a free DuckDNS subdomain

1. Go to <https://www.duckdns.org> and sign in (GitHub/Google).
2. Pick a subdomain, e.g. `cortex-nba` → you get `cortex-nba.duckdns.org`. Click **add domain**.
3. Leave the tab open — you'll paste your server's IP into the subdomain's
   **current ip** box in step 5.

## 2. Create an AWS account (free plan)

1. Go to <https://aws.amazon.com> → **Create an AWS account**.
2. During signup choose the **Free** plan (the one with $100 in credits). A
   credit card and phone verification are required even though you won't be
   charged within the free credits.
3. In the console, set a budget guardrail: **Billing and Cost Management →
   Budgets → Create budget → Zero spend / $5 monthly**, so you're emailed if
   anything starts to cost money.

## 3. Launch the EC2 instance

In the console, go to **EC2 → Launch instance**:

- **Name:** `cortex`
- **AMI:** Ubuntu Server 24.04 LTS — **make sure the architecture is `64-bit (Arm)`**
- **Instance type:** `t4g.small` (2 vCPU, 2 GB — arm64, runs the NEON SIMD path)
- **Key pair:** create one, download the `.pem`, keep it safe
- **Network / security group:** create one allowing inbound:
  - SSH (22) — **source: My IP**
  - HTTP (80) — source: Anywhere (0.0.0.0/0)
  - HTTPS (443) — source: Anywhere (0.0.0.0/0)
- **Storage:** 20 GB gp3
- **Launch.**

Then give it a stable IP: **EC2 → Elastic IPs → Allocate**, then
**Actions → Associate** to this instance. (An Elastic IP is free while it's
associated with a running instance.)

## 4. Point your subdomain at the instance

Back on duckdns.org, set your subdomain's **current ip** to the Elastic IP and
click **update ip**.

## 5. Set up the instance

SSH in (replace with your key and Elastic IP):

```bash
ssh -i cortex-key.pem ubuntu@YOUR_ELASTIC_IP
```

Run the bootstrap (installs Docker, adds swap, clones the repo):

```bash
curl -fsSL https://raw.githubusercontent.com/SriniV-1/cortex/main/deploy/aws/bootstrap.sh | bash
```

Log out and back in (so your user picks up Docker group access), then:

```bash
cd /opt/cortex/deploy/aws
cp .env.example .env
nano .env          # set DOMAIN (your-subdomain.duckdns.org), ACME_EMAIL, POSTGRES_PASSWORD
```

Generate a strong DB password with `openssl rand -hex 24` and paste it in.

## 6. Build and start

```bash
docker compose -f docker-compose.prod.yml up -d --build
```

The first build compiles the C++ server and takes ~15–30 min on a `t4g.small`
(the 4 GB swap added by bootstrap keeps it from running out of memory). Watch
progress with `docker compose -f docker-compose.prod.yml logs -f`.

Once it's up, Caddy fetches the TLS certificate automatically (a few seconds).
Visit `https://your-subdomain.duckdns.org` — the dashboard loads, but tables are
empty until you load data.

## 7. Load the historical data (one time)

```bash
./load-data.sh
```

This fetches ~4.7M play-by-play events (2019–2026) from the NBA feed — ~15–30
min. It's safe to re-run if interrupted (the ETL is idempotent). After this the
server keeps the current season fresh on its own.

Refresh the dashboard — leaderboards, games, Elo, and similarity search are now
live. 🎉

---

## Operations

```bash
# logs
docker compose -f docker-compose.prod.yml logs -f cortex

# restart after pulling new code
cd /opt/cortex && git pull && cd deploy/aws \
  && docker compose -f docker-compose.prod.yml up -d --build

# stop everything (data in the pgdata volume is preserved)
docker compose -f docker-compose.prod.yml down

# stop AND wipe the database volume
docker compose -f docker-compose.prod.yml down -v
```

## Notes

- **Why auth is off:** every data endpoint is a read-only `GET` over public NBA
  data; the only non-GET route issues an optional JWT. There is nothing to
  protect, so the public dashboard runs unauthenticated by design.
- **Only Caddy is exposed.** The server, Postgres, and Redis listen only on the
  internal Docker network. Keep the security group limited to 22/80/443.
- **Tearing down to stop all charges:** terminate the EC2 instance and release
  the Elastic IP in the console.
- **Free forever instead:** these same files run on an Oracle Cloud Always-Free
  arm64 VM. Only steps 2–3 change (create the Oracle VM); steps 5–7 are identical.
