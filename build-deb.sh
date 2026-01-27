#!/bin/bash
set -e

echo "Building Workspace application..."

# Build the application
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)
cd ..

# Prepare debian package structure
echo "Preparing .deb package..."
rm -rf debian/usr/bin
mkdir -p debian/usr/bin

# Copy the binary
cp build/workspace debian/usr/bin/

# Set proper permissions
chmod 755 debian/usr/bin/workspace
chmod 755 debian/DEBIAN
chmod 644 debian/DEBIAN/control
chmod 644 debian/usr/share/applications/workspace.desktop
chmod 644 debian/usr/share/icons/hicolor/scalable/apps/workspace.svg

# Build the .deb package
echo "Building .deb package..."
dpkg-deb --build debian workspace_1.0.0_amd64.deb

echo ""
echo "Package created: workspace_1.0.0_amd64.deb"
echo ""
echo "To install: sudo dpkg -i workspace_1.0.0_amd64.deb"
echo "To uninstall: sudo dpkg -r workspace"
