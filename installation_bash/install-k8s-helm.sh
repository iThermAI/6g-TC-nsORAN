#!/bin/bash

usage() {
    echo "Usage: $0 [ -k <k8s version> -e <helm version> -c <cni-version>" 1>&2
    exit 1
}

wait_for_pods_running () {
  NS="$2"
  CMD="kubectl get pods --all-namespaces"
  if [ "$NS" != "all-namespaces" ]; then
    CMD="kubectl get pods -n $2"
  fi
  KEYWORD="Running"
  if [ "$#" == "3" ]; then
    KEYWORD="${3}.*Running"
  fi

  CMD2="$CMD | grep \"$KEYWORD\" | wc -l"
  NUMPODS=$(eval "$CMD2")
  while [  $NUMPODS -lt $1 ]; do
    sleep 5
    NUMPODS=$(eval "$CMD2")
  done 
}

# Default versions
KUBEV="1.30.2"
HELMV="3.14.4"
KUBECNIV="1.4.0"

while getopts ":k:e:c:" o; do
    case "${o}" in
    e) HELMV=${OPTARG} ;;
    k) KUBEV=${OPTARG} ;;
    c) KUBECNIV=${OPTARG} ;;
    *) usage ;;
    esac
done

if [[ ${HELMV} == 2.* ]]; then
  echo "Helm 2 not supported"
  exit 1
fi

set -x
export DEBIAN_FRONTEND=noninteractive
echo "$(hostname -I) $(hostname)" >> /etc/hosts

# Store config info
rm -rf /opt/config
mkdir -p /opt/config
echo "${KUBEV}" > /opt/config/k8s_version.txt
echo "${KUBECNIV}" > /opt/config/k8s_cni_version.txt
echo "${HELMV}" > /opt/config/helm_version.txt
echo "$(hostname -I)" > /opt/config/host_private_ip_addr.txt
echo "$(curl -s ifconfig.co)" > /opt/config/k8s_mst_floating_ip_addr.txt
echo "$(hostname -I)" > /opt/config/k8s_mst_private_ip_addr.txt
echo "$(hostname)" > /opt/config/stack_name.txt

# Load required kernel modules
modprobe ip_vs ip_vs_rr ip_vs_wrr ip_vs_sh nf_conntrack nf_conntrack_proto_sctp

# Disable swap
SWAPFILES=$(grep swap /etc/fstab | sed '/^[ \t]*#/ d' | awk '{print $1}')
if [ ! -z "$SWAPFILES" ]; then
  for SWAPFILE in $SWAPFILES; do
    swapoff $SWAPFILE || true
    sed -i "\%$SWAPFILE%d" /etc/fstab
  done
fi

# Add Kubernetes apt repo for Ubuntu 22.04
mkdir -p /etc/apt/keyrings
curl -fsSL https://pkgs.k8s.io/core:/stable:/v1.30/deb/Release.key | gpg --dearmor -o /etc/apt/keyrings/kubernetes-apt-keyring.gpg
echo "deb [signed-by=/etc/apt/keyrings/kubernetes-apt-keyring.gpg] https://pkgs.k8s.io/core:/stable:/v1.30/deb/ /" | tee /etc/apt/sources.list.d/kubernetes.list

apt-get update
apt-get install -y curl jq netcat make ipset moreutils apt-transport-https ca-certificates gnupg

APTOPTS="--allow-downgrades --allow-change-held-packages --allow-unauthenticated --ignore-hold"

# Clean old kube packages if present
apt-get -y $APTOPTS remove kubeadm kubelet kubectl kubernetes-cni || true
apt-get -y autoremove

# Install Kubernetes
apt-get install -y $APTOPTS kubeadm=${KUBEV}-1.1 kubelet=${KUBEV}-1.1 kubectl=${KUBEV}-1.1 kubernetes-cni
apt-mark hold kubelet kubeadm kubectl kubernetes-cni

kubeadm config images pull --kubernetes-version=${KUBEV}

# Initialize control plane
NODETYPE="master"
if [ "$NODETYPE" == "master" ]; then
  kubeadm init --pod-network-cidr=10.244.0.0/16

  mkdir -p /root/.kube
  cp -i /etc/kubernetes/admin.conf /root/.kube/config
  chown root:root /root/.kube/config
  export KUBECONFIG=/root/.kube/config
  echo "KUBECONFIG=${KUBECONFIG}" >> /etc/environment

  # Deploy Flannel CNI
  kubectl apply -f https://raw.githubusercontent.com/flannel-io/flannel/v0.25.1/Documentation/kube-flannel.yml

  wait_for_pods_running 7 kube-system
  wait_for_pods_running 1 kube-flannel
  kubectl taint nodes --all node-role.kubernetes.io/control-plane:NoSchedule-

  # Install Helm
  cd /root
  if [ ! -e helm-v${HELMV}-linux-amd64.tar.gz ]; then
    wget https://get.helm.sh/helm-v${HELMV}-linux-amd64.tar.gz
  fi
  tar -xvf helm-v${HELMV}-linux-amd64.tar.gz
  mv linux-amd64/helm /usr/local/bin/helm

  while ! helm version; do
    sleep 10
  done

  # Enable local storage on master
  PV_NODE_NAME=$(kubectl get nodes -l node-role.kubernetes.io/control-plane --no-headers | awk '{print $1}' | head -1)
  kubectl label --overwrite nodes $PV_NODE_NAME local-storage=enable
  if [ "$PV_NODE_NAME" == "$(hostname)" ]; then
    mkdir -p /opt/data/dashboard-data
  fi
fi
