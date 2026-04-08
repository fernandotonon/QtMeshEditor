#!/bin/bash
set -e

# Start Xvfb if no display available (Ogre GL needs X11)
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:99
    Xvfb :99 -screen 0 1x1x24 -nolisten tcp &>/dev/null &
    # Wait for Xvfb to be ready
    ready=0
    for _ in $(seq 1 10); do
        if xdpyinfo -display :99 &>/dev/null; then
            ready=1
            break
        fi
        sleep 0.2
    done
    if [ "$ready" -ne 1 ]; then
        echo "Failed to start Xvfb on :99" >&2
        exit 1
    fi
fi

# Route to the right binary.
# /usr/bin/qtmesh is a symlink to /usr/bin/qtmesheditor (launcher script)
# which always exec's /usr/share/qtmesheditor/qtmesheditor — the argv[0]
# contains "editor" so CLI mode detection by name fails. Pass --cli
# explicitly to force CLI mode.
case "${1:-}" in
    info|fix|convert|anim|validate|lod|pose|--help|-h|--version|-v)
        exec /usr/bin/qtmesheditor --cli "$@"
        ;;
    --mcp|--with-mcp)
        exec /usr/bin/qtmesheditor "$@"
        ;;
    qtmesh)
        shift
        exec /usr/bin/qtmesheditor --cli "$@"
        ;;
    QtMeshEditor|qtmesheditor)
        shift
        exec /usr/bin/qtmesheditor "$@"
        ;;
    *)
        exec /usr/bin/qtmesheditor --cli "$@"
        ;;
esac
