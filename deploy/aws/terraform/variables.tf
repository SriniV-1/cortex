variable "region" {
  description = "AWS region to deploy into."
  type        = string
  default     = "us-east-1"
}

variable "instance_type" {
  description = "EC2 instance type. Must be arm64 (Graviton) so the engine's NEON SIMD path runs."
  type        = string
  default     = "t4g.small" # 2 vCPU, 2 GB — the smallest that builds + runs the stack
}

variable "root_volume_gb" {
  description = "Root EBS volume size in GB."
  type        = number
  default     = 20
}

variable "ssh_cidr" {
  description = "CIDR allowed to SSH (port 22). Use YOUR_IP/32. Find it with: curl -s https://checkip.amazonaws.com"
  type        = string
}

variable "ssh_public_key" {
  description = <<-EOT
    Public key material for SSH access. Derive it from your AWS .pem private key:
      ssh-keygen -y -f ~/.ssh/cortex-aws.pem
    Paste the resulting one-line "ssh-rsa AAAA..." string.
  EOT
  type        = string
}

variable "name" {
  description = "Name tag / resource prefix."
  type        = string
  default     = "cortex"
}
