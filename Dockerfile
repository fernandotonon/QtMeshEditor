FROM ubuntu:24.04

ARG VERSION=latest

# Runtime system deps only (X11, Mesa GL, virtual framebuffer)
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb x11-utils libxcb-cursor0 libxcb-xinerama0 libx11-6 libxrandr2 \
    libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 \
    freeglut3 libcurl4t64 libopengl0 libgomp1 \
    libxkbcommon0 libegl1 libglib2.0-0t64 libdbus-1-3 \
    && rm -rf /var/lib/apt/lists/*

# Copy and install the .deb (skip declared Qt package deps — libs are bundled)
COPY qtmesheditor.deb /tmp/qtmesheditor.deb
RUN dpkg --install --force-depends /tmp/qtmesheditor.deb \
    && rm /tmp/qtmesheditor.deb

# The .deb installs /usr/bin/qtmesheditor (launcher script that sets
# LD_LIBRARY_PATH, QT_QPA_PLATFORM, etc.) and /usr/bin/qtmesh (symlink).
# If the .deb predates the qtmesh symlink, create it here as a fallback.
RUN [ -e /usr/bin/qtmesh ] || ln -sf qtmesheditor /usr/bin/qtmesh

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Create non-root runtime user
RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin qtmesh \
    && mkdir -p /workspace \
    && chown -R qtmesh:qtmesh /workspace

LABEL org.opencontainers.image.source="https://github.com/fernandotonon/QtMeshEditor"
LABEL org.opencontainers.image.description="qtmesh CLI - 3D mesh conversion, optimization, and animation tools"
LABEL org.opencontainers.image.version="${VERSION}"

WORKDIR /workspace
USER qtmesh
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["--help"]
