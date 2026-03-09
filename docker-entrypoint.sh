#!/bin/bash
set -e

# Start Xvfb if no display available (Ogre GL needs X11)
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:99
    Xvfb :99 -screen 0 1x1x24 -nolisten tcp &>/dev/null &
    # Wait for Xvfb to be ready
    for i in $(seq 1 10); do
        xdpyinfo -display :99 &>/dev/null && break
        sleep 0.2
    done
fi

# Route to the right binary
case "${1:-}" in
    info|fix|convert|anim|--help|-h|--version|-v)
        exec qtmesh "$@"
        ;;
    --mcp|--with-mcp)
        exec /usr/bin/qtmesheditor "$@"
        ;;
    qtmesh)
        shift
        exec qtmesh "$@"
        ;;
    QtMeshEditor|qtmesheditor)
        shift
        exec /usr/bin/qtmesheditor "$@"
        ;;
    *)
        exec qtmesh "$@"
        ;;
esac
