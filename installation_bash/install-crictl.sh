#!/bin/bash
set -e

# Update package lists
sudo apt update

# Install cri-tools (crictl) with non-interactive flags
sudo apt install -y --allow-change-held-packages cri-tools

# Configure crictl to use containerd socket
sudo tee /etc/crictl.yaml > /dev/null <<EOF
runtime-endpoint: unix:///run/containerd/containerd.sock
image-endpoint: unix:///run/containerd/containerd.sock
timeout: 2
debug: true
pull-image-on-create: false
EOF

# Make sure config is world-readable
sudo chmod 644 /etc/crictl.yaml
