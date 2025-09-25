echo "Installing servecm (Chart Manager) and common templates to helm3"

# Install helm servecm plugin
helm plugin install https://github.com/jdolitsky/helm-servecm
eval $(helm env | grep HELM_REPOSITORY_CACHE)
echo "Helm cache dir: ${HELM_REPOSITORY_CACHE}"

# Install chartmuseum (64-bit for Ubuntu 22.04)
curl -LO https://get.helm.sh/chartmuseum-v0.15.0-linux-amd64.tar.gz
tar xzvf chartmuseum-v0.15.0-linux-amd64.tar.gz
chmod +x ./linux-amd64/chartmuseum
sudo cp ./linux-amd64/chartmuseum /usr/local/bin/

# Start helm servecm in background
nohup helm servecm --port=8879 --context-path=/charts \
  --storage local --storage-local-rootdir $HELM_REPOSITORY_CACHE/local/ >/dev/null 2>&1 &

# Wait until it's ready
CURL_CMD="curl --silent --output /dev/null http://127.0.0.1:8879/charts"
$CURL_CMD
READY=$?
while [ ${READY} -ne 0 ]; do
    echo "servecm not yet running. sleeping for 2 seconds"
    sleep 2
    $CURL_CMD
    READY=$?
done
echo "servecm up and running"

# Prepare local helm repo
eval $(helm env | grep HELM_REPOSITORY_CACHE)
mkdir -p "${HELM_REPOSITORY_CACHE}/local/"

export COMMON_CHART_VERSION=$(grep version ../ns-o-ran_ric_bash/ric-common/Common-Template/helm/ric-common/Chart.yaml | awk '{print $2}')
helm package -d /tmp ../ns-o-ran_ric_bash/ric-common/Common-Template/helm/ric-common
cp /tmp/ric-common-${COMMON_CHART_VERSION}.tgz "${HELM_REPOSITORY_CACHE}/local/"

helm repo remove local || true
helm repo add local http://127.0.0.1:8879/charts

echo "checking that ric-common templates were added"
helm search repo local/ric-common
