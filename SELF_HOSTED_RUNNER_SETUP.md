# Setting Up Self-Hosted GitHub Runner on Intel Mac

This guide will help you set up your Intel Mac as a self-hosted GitHub Actions runner to build x86_64 binaries for the universal binary workflow using `lipo -create`.

## Prerequisites

- Intel Mac running macOS 11.0 or later
- Administrator access to the Mac
- Stable internet connection
- GitHub repository admin access

## Step 1: Prepare Your Intel Mac

### Install Required Tools

```bash
# Install Xcode Command Line Tools (if not already installed)
xcode-select --install

# Install Homebrew (if not already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install CMake
brew install cmake

# Install Git (if not already installed)
brew install git

# Verify installations
cmake --version
git --version
xcode-select --version
```

### Verify Architecture

```bash
# Confirm this is an Intel Mac
uname -m
# Should output: x86_64

# Check macOS version
sw_vers -productVersion
# Should be 11.0 or later
```

## Step 2: Add Self-Hosted Runner to GitHub

### In GitHub Repository:

1. Go to your repository on GitHub
2. Click **Settings** → **Actions** → **Runners**
3. Click **New self-hosted runner**
4. Select **macOS** as the operating system
5. Select **x64** as the architecture

### Follow GitHub's Instructions:

GitHub will provide specific commands like these (use the actual commands from your GitHub page):

```bash
# Download
mkdir actions-runner && cd actions-runner
curl -o actions-runner-osx-x64-2.311.0.tar.gz -L https://github.com/actions/runner/releases/download/v2.311.0/actions-runner-osx-x64-2.311.0.tar.gz
tar xzf ./actions-runner-osx-x64-2.311.0.tar.gz

# Configure (use your actual token and URL from GitHub)
./config.sh --url https://github.com/YOUR_USERNAME/QtMeshEditor --token YOUR_TOKEN
```

### Configure the Runner:

When prompted during configuration:

- **Runner name**: Choose something like `intel-mac-x64` or `your-name-intel`
- **Runner group**: Use `Default` (press Enter)
- **Labels**: Add `macOS, x64` (this matches the workflow)
- **Work folder**: Use default (press Enter)

## Step 3: Set Up Runner as a Service

### Create Launch Agent (Recommended)

```bash
# Install the runner as a service
sudo ./svc.sh install

# Start the service
sudo ./svc.sh start

# Check status
sudo ./svc.sh status
```

### Alternative: Manual Start (for testing)

```bash
# Run the runner manually (for testing)
./run.sh
```

## Step 4: Verify Runner Setup

### Check in GitHub:

1. Go to **Settings** → **Actions** → **Runners**
2. You should see your runner listed as "Online"
3. The runner should have labels: `self-hosted`, `macOS`, `x64`

### Test the Runner:

Create a simple test workflow to verify it works:

```yaml
# .github/workflows/test-intel-runner.yml
name: Test Intel Runner

on: workflow_dispatch

jobs:
  test-intel:
    runs-on: [self-hosted, macos-intel]
    steps:
    - name: Test Intel Runner
      run: |
        echo "Testing Intel Mac runner"
        echo "Architecture: $(uname -m)"
        echo "macOS version: $(sw_vers -productVersion)"
        echo "Available CPU cores: $(sysctl -n hw.ncpu)"
        echo "Runner labels: self-hosted, macOS, X64, macos-intel"
```

## Step 5: Configure Runner for QtMeshEditor

### Install Project Dependencies:

```bash
# Install Qt (if not already available system-wide)
# The workflow will handle Qt installation, but you can pre-install for faster builds
brew install qt

# Install any other common dependencies
brew install pkg-config
```

### Set Up Environment:

Create a `.bashrc` or `.zshrc` file with common environment variables:

```bash
# Add to ~/.zshrc or ~/.bashrc
export CMAKE_PREFIX_PATH="/usr/local/opt/qt/lib/cmake"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig"
export PATH="/usr/local/bin:$PATH"
```

## Step 6: Update Workflow Configuration

The workflow file `.github/workflows/dual-runner-universal.yml` uses these runner labels:

```yaml
runs-on: [self-hosted, macos-intel]
```

Your runner should have labels like: `self-hosted, macOS, X64, macos-intel`

```bash
# Stop the service
sudo ./svc.sh stop

# Reconfigure with additional labels
./config.sh remove
./config.sh --url https://github.com/YOUR_USERNAME/QtMeshEditor --token YOUR_NEW_TOKEN --labels macOS,x64

# Start the service again
sudo ./svc.sh start
```

## Step 7: Security Considerations

### Runner Security:

```bash
# Create a dedicated user for the runner (recommended)
sudo dscl . -create /Users/github-runner
sudo dscl . -create /Users/github-runner UserShell /bin/bash
sudo dscl . -create /Users/github-runner RealName "GitHub Runner"

# Set up proper permissions
sudo chown -R github-runner:staff /path/to/actions-runner
```

### Network Security:

- Ensure your Mac has proper firewall settings
- Consider using a VPN if accessing from outside your network
- Regularly update the runner software

## Step 8: Monitoring and Maintenance

### Check Runner Status:

```bash
# Check if service is running
sudo ./svc.sh status

# View runner logs
tail -f _diag/Runner_*.log
```

### Update Runner:

```bash
# Stop the service
sudo ./svc.sh stop

# Download and install updates
./config.sh remove
# Download new runner version and repeat setup
```

### Automatic Updates:

Consider setting up automatic runner updates:

```bash
# Create a script for automatic updates
cat > ~/update-runner.sh << 'EOF'
#!/bin/bash
cd /path/to/actions-runner
sudo ./svc.sh stop
./config.sh remove
# Add commands to download and configure new version
sudo ./svc.sh install
sudo ./svc.sh start
EOF

chmod +x ~/update-runner.sh

# Add to crontab for weekly updates
crontab -e
# Add: 0 2 * * 1 /Users/yourusername/update-runner.sh
```

## Step 9: Troubleshooting

### Common Issues:

**Runner offline:**
```bash
# Check service status
sudo ./svc.sh status

# Restart service
sudo ./svc.sh stop
sudo ./svc.sh start
```

**Permission errors:**
```bash
# Fix permissions
sudo chown -R $(whoami):staff /path/to/actions-runner
chmod +x ./run.sh ./config.sh
```

**Network issues:**
```bash
# Test GitHub connectivity
curl -I https://github.com
curl -I https://api.github.com
```

**Build failures:**
```bash
# Check Xcode tools
xcode-select --version
xcode-select --print-path

# Reinstall if needed
sudo xcode-select --reset
```

### Debug Workflow:

Add debug steps to your workflow:

```yaml
- name: Debug Environment
  run: |
    echo "Runner info:"
    uname -a
    echo "Environment variables:"
    env | grep -E "(CMAKE|QT|PATH)" | sort
    echo "Available tools:"
    which cmake git lipo codesign
```

## Step 10: Testing the Complete Workflow

### Trigger a Test Build:

1. Push a commit to trigger the workflow
2. Check the Actions tab in GitHub
3. Verify that:
   - ARM64 job runs on GitHub runner
   - x86_64 job runs on your Intel Mac
   - Universal binary is created successfully

### Expected Output:

You should see something like:

```
✅ ARM64 build: Completed on GitHub runner
✅ x86_64 build: Completed on self-hosted Intel Mac  
✅ Universal binary: Created with lipo -create
✅ Code signing: Applied
✅ DMG package: Created
```

## Benefits of This Setup

- **True Architecture Separation**: Each binary is built on its native architecture
- **Better Debugging**: Easier to isolate architecture-specific issues
- **Resource Control**: Your Intel Mac handles only x86_64 builds
- **Cost Efficiency**: Reduce GitHub Actions minutes for x86_64 builds
- **Reliability**: More control over the build environment

## Maintenance Schedule

- **Weekly**: Check runner status and logs
- **Monthly**: Update runner software if new versions available
- **Quarterly**: Review security settings and update dependencies

This setup gives you the perfect foundation for creating true universal binaries using the `lipo -create` approach! 