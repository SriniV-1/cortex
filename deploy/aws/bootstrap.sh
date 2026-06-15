#!/usr/bin/env bash
# One-time setup for a fresh Ubuntu 24.04 (arm64) EC2 instance.
# Installs Docker, adds swap (so the C++ image can build on a 2 GB box),
# clones Cortex, and prepares the deploy directory.
#
# SSH into the instance, then:
#   curl -fsSL https://raw.githubusercontent.com/SriniV-1/cortex/main/deploy/aws/bootstrap.sh | bash
# or copy this file over and run `bash bootstrap.sh`.
#
# After it finishes:
#   cd /opt/cortex/deploy/aws
#   cp .env.example .env && nano .env        # set DOMAIN, ACME_EMAIL, POSTGRES_PASSWORD
#   docker compose -f docker-compose.prod.yml up -d --build
#   ./load-data.sh
set -euo pipefail

REPO="https://github.com/SriniV-1/cortex.git"
DEST="/opt/cortex"

echo "==> Adding 4 GB swap (needed to build the C++ image on a 2 GB instance)"
if ! sudo swapon --show | grep -q /swapfile; then
  sudo fallocate -l 4G /swapfile
  sudo chmod 600 /swapfile
  sudo mkswap /swapfile
  sudo swapon /swapfile
  echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab >/dev/null
fi

echo "==> Installing Docker Engine + Compose plugin"
if ! command -v docker >/dev/null 2>&1; then
  curl -fsSL https://get.docker.com | sudo sh
  sudo usermod -aG docker "$USER"
fi

echo "==> Cloning Cortex into $DEST"
if [ ! -d "$DEST/.git" ]; then
  sudo git clone "$REPO" "$DEST"
  sudo chown -R "$USER":"$USER" "$DEST"
else
  git -C "$DEST" pull --ff-only
fi

cp -n "$DEST/deploy/aws/.env.example" "$DEST/deploy/aws/.env" || true

cat <<EOF

==> Bootstrap complete.

Next steps (log out and back in first so the 'docker' group applies, or run 'newgrp docker'):

  cd $DEST/deploy/aws
  nano .env          # set DOMAIN, ACME_EMAIL, POSTGRES_PASSWORD
  docker compose -f docker-compose.prod.yml up -d --build   # ~15-30 min first build
  ./load-data.sh                                            # ~15-30 min data load

Then open https://<your-domain> .
EOF
