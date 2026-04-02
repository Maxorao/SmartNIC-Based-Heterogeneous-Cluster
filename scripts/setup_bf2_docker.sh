#!/bin/bash
# setup_bf2_docker.sh — Install Docker on BF2 ARM and prepare Nginx image.
#
# Run from any host that can SSH to the BF2s.
# Usage: bash scripts/setup_bf2_docker.sh

set -e
source "$(dirname "$0")/config.sh"

BF2_HOSTS=("${FUJIAN_IP}" "${HELONG_IP}")

for host_ip in "${BF2_HOSTS[@]}"; do
    echo "=== Setting up Docker on BF2 via ${host_ip} ==="
    ssh "$(whoami)@${host_ip}" "
        ssh root@192.168.100.2 '
            # Check if Docker is already installed
            if command -v docker &>/dev/null; then
                echo \"Docker already installed: \$(docker --version)\"
            else
                echo \"Installing Docker on BF2 ARM...\"
                apt-get update -qq
                apt-get install -y -qq docker.io
                systemctl enable docker
                systemctl start docker
                echo \"Docker installed: \$(docker --version)\"
            fi

            # Pull Nginx arm64 image
            if docker image inspect nginx:alpine &>/dev/null; then
                echo \"nginx:alpine image already available\"
            else
                echo \"Pulling nginx:alpine (arm64)...\"
                docker pull nginx:alpine
            fi

            # Verify
            echo \"Docker images:\"
            docker images --format \"{{.Repository}}:{{.Tag}} {{.Size}}\"
            echo \"BF2 Docker setup complete\"
        '
    " &
done
wait
echo "=== All BF2s ready ==="
