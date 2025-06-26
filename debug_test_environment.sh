#!/bin/bash

echo "=== Debug Test Environment ==="
echo "Working Directory: $(pwd)"

# Change to project root if we're not already there
if [ ! -d "media/models" ]; then
    cd /home/fernando/QtMeshEditor
    echo "Changed working directory to: $(pwd)"
fi

echo "PATH: $PATH"
echo "LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
echo "QT_QPA_PLATFORM: $QT_QPA_PLATFORM"
echo "DISPLAY: $DISPLAY"
echo "XDG_SESSION_TYPE: $XDG_SESSION_TYPE"
echo "QT_PLUGIN_PATH: $QT_PLUGIN_PATH"
echo "QT_LOGGING_RULES: $QT_LOGGING_RULES"
echo ""

echo "=== Running Test with Environment Info ==="
echo "Running: $1"

# Set a safe QT_QPA_PLATFORM if not already set
if [ -z "$QT_QPA_PLATFORM" ]; then
    export QT_QPA_PLATFORM=offscreen
    echo "Set QT_QPA_PLATFORM=offscreen"
fi

# Try running the test with additional debugging
exec "$@" 2>&1 