# Cortex infrastructure as code (Terraform)

This provisions the Cortex demo's AWS infrastructure declaratively — the same
arm64 Graviton EC2 + Elastic IP + security group that `../README.md` builds by
hand, but defined in code so it's reproducible, reviewable, and tears down
cleanly.

| File | Purpose |
|------|---------|
| `versions.tf` | Terraform + AWS provider version constraints |
| `variables.tf` | Inputs (region, instance type, SSH CIDR, public key) |
| `main.tf` | AMI lookup, key pair, security group, instance (with cloud-init), Elastic IP |
| `outputs.tf` | Public IP, HTTPS hostname, URL, SSH command |
| `terraform.tfvars.example` | Template for your values |

## Prerequisites

- [Terraform](https://developer.hashicorp.com/terraform/install) ≥ 1.5
- AWS credentials configured (`aws configure`) — Terraform uses the same ones
- An SSH key pair. To reuse the one from the manual deploy, derive its public
  key: `ssh-keygen -y -f ~/.ssh/cortex-aws.pem`

## Use

```bash
cd deploy/aws/terraform
cp terraform.tfvars.example terraform.tfvars
# edit terraform.tfvars: set ssh_cidr (your IP/32) and ssh_public_key

terraform init      # download the AWS provider
terraform plan      # preview what will be created (dry run)
terraform apply     # build it — prints public_ip, hostname, url, ssh_command
```

cloud-init installs Docker + 4 GB swap and clones the repo to `/opt/cortex` on
first boot. Once `terraform apply` finishes, SSH in and bring up the stack:

```bash
ssh -i ~/.ssh/cortex-aws.pem ubuntu@<public_ip>
cd /opt/cortex/deploy/aws
cp .env.example .env && nano .env     # set DOMAIN=<hostname output>, ACME_EMAIL, POSTGRES_PASSWORD
docker compose -f docker-compose.prod.yml up -d --build
./load-data.sh
```

Then open the `url` output. Tear everything down with:

```bash
terraform destroy
```

## Notes

- **AMI is resolved at plan time** from Canonical's SSM parameter, so you always
  get the current Ubuntu 24.04 arm64 image — no stale hardcoded AMI id.
- **State**: `terraform.tfstate` records what was created. It can contain
  sensitive values and is git-ignored — keep it (locally, or in a remote
  backend like S3 for a team).
- **Adopting an already-running stack**: if you created the instance manually
  first, you can `terraform import` the existing resources instead of creating
  duplicates. For a fresh environment, just `apply`.
- **arm64 is required**, not just cheaper: the engine's hand-written vector
  search uses ARM NEON SIMD, so the instance type must stay in the `t4g`/`c7g`/
  `m7g` (Graviton) families.
