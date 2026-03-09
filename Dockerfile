FROM ubuntu:22.04

ARG VERSION=latest

# Runtime system deps only (X11, Mesa GL, virtual framebuffer)
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb x11-utils libxcb-cursor0 libxcb-xinerama0 libx11-6 libxrandr2 \
    libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 \
    freeglut3 libcurl4 \
    && rm -rf /var/lib/apt/lists/*

# Copy and install the .deb (skip declared Qt package deps — libs are bundled)
COPY *.deb /tmp/qtmesheditor.deb
RUN dpkg --install --force-depends /tmp/qtmesheditor.deb \
    && rm /tmp/qtmesheditor.deb

# Create qtmesh symlink for CLI mode detection
# (binary name must start with "qtmesh" but NOT contain "editor")
RUN ln -sf /usr/share/qtmesheditor/qtmesheditor /usr/local/bin/qtmesh

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

LABEL org.opencontainers.image.source="https://github.com/fernandotonon/QtMeshEditor"
LABEL org.opencontainers.image.description="qtmesh CLI - 3D mesh conversion, optimization, and animation tools"
LABEL org.opencontainers.image.version="${VERSION}"

WORKDIR /workspace
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["--help"]
