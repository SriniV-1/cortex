output "public_ip" {
  description = "Stable Elastic IP of the instance."
  value       = aws_eip.cortex.public_ip
}

output "hostname" {
  description = "Free HTTPS-capable hostname (sslip.io resolves it to the IP). Use as DOMAIN in .env."
  value       = "${replace(aws_eip.cortex.public_ip, ".", "-")}.sslip.io"
}

output "url" {
  description = "Public dashboard URL once the stack is up."
  value       = "https://${replace(aws_eip.cortex.public_ip, ".", "-")}.sslip.io"
}

output "ssh_command" {
  description = "SSH into the instance."
  value       = "ssh -i ~/.ssh/${var.name}-aws.pem ubuntu@${aws_eip.cortex.public_ip}"
}
