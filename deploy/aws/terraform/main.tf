# ─────────────────────────────────────────────────────────────────────────────
# Cortex — single-instance AWS deployment as code.
#
# Codifies exactly what `deploy/aws/README.md` does by hand: an arm64 Graviton
# EC2 instance with a stable Elastic IP and a security group exposing only SSH
# (to you), HTTP, and HTTPS. cloud-init installs Docker + swap and clones the
# repo on first boot, so after `terraform apply` you only fill in .env and run
# docker compose + the data load.
#
#   terraform init
#   terraform apply           # builds the infra, prints the public IP + hostname
#   terraform destroy         # tears it all down cleanly
# ─────────────────────────────────────────────────────────────────────────────

# Latest Ubuntu 24.04 (arm64) AMI, resolved at plan time from Canonical's
# published SSM parameter — never a stale hardcoded AMI id.
data "aws_ssm_parameter" "ubuntu_2404_arm64" {
  name = "/aws/service/canonical/ubuntu/server/24.04/stable/current/arm64/hvm/ebs-gp3/ami-id"
}

resource "aws_key_pair" "cortex" {
  key_name   = var.name
  public_key = var.ssh_public_key
}

resource "aws_security_group" "cortex" {
  name        = "${var.name}-sg"
  description = "Cortex demo: SSH (you) + HTTP/HTTPS (public)"

  ingress {
    description = "SSH (your IP only)"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.ssh_cidr]
  }

  ingress {
    description = "HTTP (ACME challenge + redirect to HTTPS)"
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  ingress {
    description = "HTTPS (the dashboard)"
    from_port   = 443
    to_port     = 443
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  egress {
    description = "All outbound"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.name}-sg" }
}

resource "aws_instance" "cortex" {
  ami                    = data.aws_ssm_parameter.ubuntu_2404_arm64.value
  instance_type          = var.instance_type
  key_name               = aws_key_pair.cortex.key_name
  vpc_security_group_ids = [aws_security_group.cortex.id]

  root_block_device {
    volume_size           = var.root_volume_gb
    volume_type           = "gp3"
    delete_on_termination = true
  }

  # First-boot bootstrap: 4 GB swap (so the C++ image builds on 2 GB RAM),
  # Docker Engine + Compose, and a shallow clone of the repo. Mirrors
  # deploy/aws/bootstrap.sh so the imperative and IaC paths stay in sync.
  user_data = <<-CLOUDINIT
    #!/usr/bin/env bash
    set -e
    if ! swapon --show | grep -q /swapfile; then
      fallocate -l 4G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
      echo '/swapfile none swap sw 0 0' >> /etc/fstab
    fi
    apt-get update -y && apt-get install -y git
    curl -fsSL https://get.docker.com | sh
    usermod -aG docker ubuntu
    [ -d /opt/cortex/.git ] || git clone --depth 1 https://github.com/SriniV-1/cortex.git /opt/cortex
    chown -R ubuntu:ubuntu /opt/cortex
  CLOUDINIT

  tags = { Name = var.name }
}

resource "aws_eip" "cortex" {
  instance = aws_instance.cortex.id
  domain   = "vpc"
  tags     = { Name = "${var.name}-eip" }
}
