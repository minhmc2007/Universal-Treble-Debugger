#!/bin/bash
# UTD (Universal Engine) Build & Injector Script
# Usage: bash main.sh system.img system_patched.img

set -e

SYS_IMG="$1"
SYS_NEW="$2"

if [ -z "$SYS_NEW" ]; then
    echo "Usage: $0 <source_system.img> <output_system.img>"
    exit 1
fi

echo "[*] Detecting Compiler..."
CC=""
if command -v aarch64-linux-musl-gcc >/dev/null 2>&1; then
    CC="aarch64-linux-musl-gcc"
    echo "    -> Found musl-gcc. Using strict static compilation."
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    CC="aarch64-linux-gnu-gcc"
    echo "    -> Warning: musl-gcc not found. Falling back to glibc (binary will be larger)."
else
    echo "    -> Error: Require aarch64 cross-compiler (aarch64-linux-gnu-gcc or musl-gcc)."
    exit 1
fi

echo "[*] Compiling bootlog (UTD)..."
$CC -static -Os -o bootlog bootlog.c
strip bootlog
echo "    -> Size: $(du -sh bootlog | cut -f1)"

echo "[*] Mounting $SYS_IMG..."
mkdir -p sys_mnt sys_new
sudo mount -o loop,ro "$SYS_IMG" sys_mnt

echo "[*] Copying contents (preserving symlinks, SELinux contexts, and ACLs)..."
# rsync -aHAX is essential for not breaking Android's strict file system security
sudo rsync -aHAX sys_mnt/ sys_new/

echo "[*] Unmounting original image..."
sudo umount sys_mnt

echo "[*] Injecting UTD Engine..."
# Handle both legacy A-only and modern A/B Treble partition structures
BIN_DIR="sys_new/system/bin"
INIT_DIR="sys_new/system/etc/init/hw"
if [ ! -d "$BIN_DIR" ]; then
    BIN_DIR="sys_new/bin"
    INIT_DIR="sys_new/etc/init/hw"
fi

sudo cp bootlog "$BIN_DIR/bootlog"
sudo chmod 755 "$BIN_DIR/bootlog"
sudo chown 0:2000 "$BIN_DIR/bootlog" # root:shell

echo "[*] Patching init.rc (Automated Injection)..."
# Automatically swap the binary in Android's initialization instructions
if [ -f "$INIT_DIR/init.rc" ]; then
    sudo sed -i 's/\/system\/bin\/bootanimation/\/system\/bin\/bootlog/g' "$INIT_DIR/init.rc"
    echo "    -> Bootanim service overwritten successfully."
else
    echo "    -> Warning: Could not find init.rc at $INIT_DIR!"
fi

echo "[*] Repacking $SYS_NEW (ext4)..."
# Compute the raw filesystem size, add 200MB padding for safety.
SIZE_KB=$(sudo du -sk sys_new | cut -f1)
SIZE_KB=$((SIZE_KB + 204800))

# Standard Linux Ext4 creation tool.
sudo mke2fs -O ^has_journal -t ext4 -d sys_new "$SYS_NEW" "${SIZE_KB}k"

echo "[*] Cleanup..."
sudo rm -rf sys_mnt sys_new bootlog

echo "[SUCCESS] GSI/System Image patched: $SYS_NEW"
echo "You can now flash this image via fastboot."
