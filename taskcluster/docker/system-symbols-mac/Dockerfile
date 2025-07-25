FROM          $DOCKER_IMAGE_PARENT
MAINTAINER    Gabriele Svelto <gsvelto@mozilla.com>

RUN mkdir -p /builds
RUN id worker || useradd -d /builds/worker -s /bin/bash -m worker
WORKDIR /builds/worker

# We need to declare all potentially cache volumes as caches. Also,
# making high I/O paths volumes increase I/O throughput because of
# AUFS slowness.
VOLUME /builds/worker/checkouts

RUN apt-get update && \
    apt-get install --no-install-recommends -y python3-dev python3-pip python3-requests-futures python3-setuptools curl pax build-essential libxml2-dev libssl-dev zlib1g-dev libusb-dev cmake libbz2-dev libpng-dev wget zip liblzma-dev && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir /opt/data-reposado/
RUN chown -R worker.worker /opt/data-reposado/

COPY setup.sh /setup/
COPY requirements.txt /setup/
# %include tools/crashreporter/system-symbols/mac

RUN /bin/sh /setup/setup.sh

COPY topsrcdir/tools/crashreporter/system-symbols/mac /builds/worker/mac-update-symbols
# %include python/mozbuild/mozpack/macpkg.py
COPY topsrcdir/python/mozbuild/mozpack/macpkg.py /builds/worker/mac-update-symbols/mozpack/
WORKDIR /builds/worker/mac-update-symbols

RUN chown -R worker:worker /builds/worker

# Set a default command useful for debugging
CMD ["/bin/bash", "--login"]
