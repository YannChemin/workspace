# Installation Guide

## Build Dependencies

Install the required development packages:

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    qt6-base-dev \
    libqt6network6-dev \
    libgdal-dev \
    qtermwidget6-dev \
    libx11-dev \
    libxkbcommon-dev \
    libxkbfile-dev
```

## Building from Source

1. Clone or download the repository

2. Build the application:
   ```bash
   ./build.sh
   ```

3. Run the application:
   ```bash
   ./build/workspace
   ```

## Building the .deb Package

To create a Debian package:

```bash
./build-deb.sh
```

This will generate `workspace_1.0.0_amd64.deb` in the project directory.

## Installing the .deb Package

### Install

```bash
sudo dpkg -i workspace_1.0.0_amd64.deb
```

If you encounter dependency errors, run:

```bash
sudo apt --fix-broken install
```

### Uninstall

```bash
sudo dpkg -r workspace
```

## Post-Installation

After installing the .deb package:

1. The application will appear in your GNOME dashboard
2. Search for "Workspace" in the application menu
3. Alternatively, run `workspace` from the terminal

## Runtime Dependencies

The following packages are required to run the application:

- libqt6core6
- libqt6widgets6
- libqt6network6
- libgdal34
- libqtermwidget6-0
- libx11-6
- libxkbcommon0
- libxkbfile1

These are automatically installed when using the .deb package.

## Troubleshooting

### Application not appearing in GNOME dashboard

Update the desktop database:

```bash
sudo update-desktop-database
```

### Icon not displaying

Update the icon cache:

```bash
sudo gtk-update-icon-cache /usr/share/icons/hicolor
```
