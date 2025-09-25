FROM wineslab/o-ran-sc-bldr-ubuntu18-c-go:9-u18.04 AS buildenv
ARG log_level_e2sim=2

# Install deps and build
RUN apt-get update --fix-missing || true && apt-get install -y \
    build-essential git cmake libsctp-dev autoconf automake libtool bison flex libboost-all-dev \
    gcc-8 g++-8 curl python3.8 python3.8-distutils python3-pip && \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 80 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-8 80 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Build e2sim
RUN git clone -b develop https://github.com/wineslab/ns-o-ran-e2-sim e2sim && \
    mkdir -p e2sim/e2sim/build && cd e2sim/e2sim/build && \
    cmake .. -DDEV_PKG=1 -DLOG_LEVEL=${log_level_e2sim} && \
    make package && dpkg -i ./e2sim-dev_1.0.0_amd64.deb && ldconfig

# Build ns3
RUN git clone https://github.com/aliasadi2001/ns-o-ran-ns3-mmwave-3.38.git ns3-mmwave-oran && \
    git clone -b master https://github.com/o-ran-sc/sim-ns3-o-ran-e2 ns3-mmwave-oran/contrib/oran-interface

# Copy your scenarios into scratch before build
COPY ns3_scenario/slicing_AD_v5.cc /workspace/ns3-mmwave-oran/scratch/

WORKDIR /workspace/ns3-mmwave-oran
RUN ./ns3 configure && ./ns3 build

# Install Python deps system-wide
COPY requirements.txt /workspace/
RUN python3.8 -m pip install --no-cache-dir --upgrade pip && \
    python3.8 -m pip install --no-cache-dir -r /workspace/requirements.txt && \
    python3.8 -m pip install --no-cache-dir statsd-telegraf

#Install grpcurl
RUN curl -sSL "https://github.com/fullstorydev/grpcurl/releases/download/v1.9.2/grpcurl_1.9.2_linux_x86_64.tar.gz" | tar -xz -C /usr/local/bin

# ------------------------------
# Runtime image (much smaller!)
# ------------------------------
FROM ubuntu:18.04

# System deps for runtime only
RUN apt-get update && apt-get install -y \
    libsctp1 python3.8 python3-pip && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Copy only runtime binaries and scripts from buildenv
COPY --from=buildenv /usr/local/bin/grpcurl /usr/local/bin/
COPY --from=buildenv /workspace/ns3-mmwave-oran /workspace/ns3-mmwave-oran
COPY --from=buildenv /usr/local/bin /usr/local/bin
COPY --from=buildenv /usr/local/lib/python3.8/dist-packages /usr/local/lib/python3.8/dist-packages

# Copy app scripts
COPY sim_watcher.py /workspace/ns3-mmwave-oran/
COPY influx_db/ /workspace/ns3-mmwave-oran/influx_db/
COPY metric_src/ /workspace/ns3-mmwave-oran/metric_src/
COPY abd_ts_src/ /workspace/ns3-mmwave-oran/abd_ts_src/

CMD ["/bin/sh"]