#!/bin/bash
set -euo pipefail

# the Valve developers that wrote steam.sh forgot to add this. While normal sh fails on `set -o pipefall`, other shells exist
if [ -z "$BASH_VERSION" ]; then
    echo "Not running in bash, restarting with bash..."
    exec bash "$0" "$@"
fi

KVER="6.15.9"
BUILD=true                  # Set to false if you already built it
SCRIPT_DIR="$PWD"
WORK_DIR="$PWD/build-$KVER"
DISK_SIZE=100               # In MiB
KVM=true                    # Requires superuser to enable
DISK_IMG=true               # Requires superuser to enable
INSTALL=false               # Requires superuser to enable
ADD_BOX=true                # Optional. Adds busybox for `sh`. Does not need superuser

# === QEMU Boot Configuration ===
QEMU_NET=true          # attach network
QEMU_GRAPHICAL=false   # set true for GUI, false for -nographic
QEMU_MEM=512M          # memory
QEMU_DISK="$WORK_DIR/disk.img"
QEMU_KERNEL="$WORK_DIR/vmlinuz"
QEMU_INITRD="$WORK_DIR/initramfs.cpio.gz"
QEMU_APPEND="console=ttyS0"
QEMU_MACHINE="q35"

IMG="$QEMU_DISK"
MNT="$WORK_DIR/mnt"

if [ "$INSTALL" = true ]; then
    echo "Installing required packages..."
    sudo apt update
    sudo apt install -y \
        build-essential gcc make wget cpio \
        gzip grub-pc-bin grub-efi-amd64-bin \
        qemu-system-x86 qemu-kvm \
        libncurses-dev bison flex libssl-dev

    echo "Setup complete. Proceeding to main script"
fi

mkdir -p "$WORK_DIR"


if [ ! -d "$SCRIPT_DIR/linux-$KVER" ]; then
    wget "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" -O "$SCRIPT_DIR/linux-$KVER.tar.xz"
    echo "Extracting Tarball..."
    tar -xf "$SCRIPT_DIR/linux-$KVER.tar.xz" -C "$SCRIPT_DIR"
fi

if [ ! -d "$WORK_DIR/linux-$KVER" ]; then
    echo "Copying files..."
    cp -r "$SCRIPT_DIR/linux-$KVER" "$WORK_DIR"
fi

if [ "$BUILD" = true ]; then
    cd "$WORK_DIR/linux-$KVER"

    # Load minimal default config
    make defconfig

    # Enable initramfs + tmpfs support
    scripts/config --enable CONFIG_BLK_DEV_INITRD
    scripts/config --enable CONFIG_INITRAMFS_SOURCE
    scripts/config --enable CONFIG_DEVTMPFS
    scripts/config --enable CONFIG_DEVTMPFS_MOUNT
    scripts/config --enable CONFIG_TMPFS
    scripts/config --enable CONFIG_TMPFS_POSIX_ACL
    scripts/config --enable CONFIG_TMPFS_XATTR
    scripts/config --enable CONFIG_RD_GZIP
    scripts/config --enable CONFIG_INITRAMFS_COMPRESSION_GZIP
    # Add whatever configs you like. e.g. framebuffers, not that the software I made uses them, but you can use them in busybox.

    # Update config to resolve dependencies
    make olddefconfig

    # Build kernel image only
    make -j$(nproc) bzImage

    cp "$WORK_DIR/linux-$KVER/arch/x86/boot/bzImage" "$WORK_DIR/vmlinuz"
    cd "$SCRIPT_DIR"
fi

mkdir -p "$SCRIPT_DIR/initramfs/bin"
if [ "$ADD_BOX" = true ]; then
    mkdir -p "initramfs/bin"
    mkdir -p "initramfs/usr/bin"

    # They said not to consume paths outside the CWD so I have to do this
    apt download busybox

    mkdir -p busybox_local
    dpkg-deb -x busybox_*.deb busybox_local/

    wget -O busybox_amd64 https://github.com/EXALAB/Busybox-static/raw/main/busybox_amd64
    cp busybox_amd64 busybox_local/usr/bin/busybox

    cp -r busybox_local/usr/bin initramfs
    cp -r busybox_local/usr initramfs
    chmod +x "$SCRIPT_DIR/initramfs/usr/bin/busybox"
    chmod +x "$SCRIPT_DIR/initramfs/bin/busybox"
fi

gcc -static "$SCRIPT_DIR/init.c" -o "$SCRIPT_DIR/initramfs/init"
chmod +x "$SCRIPT_DIR/initramfs/init" # GCC should theoretically already do this, but better safe than sorry

cd "$SCRIPT_DIR/initramfs"
find . -print0 | cpio --null -ov --format=newc | gzip -9 > "$WORK_DIR/initramfs.cpio.gz"
cd "$SCRIPT_DIR"

# disk image
if [ "$DISK_IMG" = true ]; then
    dd if=/dev/zero of="$IMG" bs=1M count=$DISK_SIZE
    LOOP=$(sudo losetup --show -fP "$IMG") # I tried looking everywhere to try to keep everything
    # in CWD, but I now realize that all roads (eventually) lead to loopback devices. In fairness, /dev/* is 
    # a kernel interface interfaced like a normal file but isn't TECHNICALLY host filesystem 
    # (assignment rules were vague/unclear. All actual image data technically resides in $IMG)

    # GPT partitioning with BIOS Boot Partition + EFI + root
    sudo parted -s "$LOOP" mklabel gpt

    # 1. BIOS Boot Partition (2MB, no filesystem)
    sudo parted -s "$LOOP" mkpart bios_grub 1MiB 3MiB
    sudo parted -s "$LOOP" set 1 bios_grub on

    # 2. EFI System Partition (fat32, 12MB)
    sudo parted -s "$LOOP" mkpart ESP fat32 3MiB 15MiB
    sudo parted -s "$LOOP" set 2 boot on

    # 3. Root partition (ext4, rest of disk)
    sudo parted -s "$LOOP" mkpart primary ext4 15MiB 100%

    sudo partprobe "$LOOP"

    # format partitions
    sudo mkfs.fat -F32 "${LOOP}p2"
    sudo mkfs.ext4 -L sysrootfs "${LOOP}p3"

    # mount root
    sudo mkdir -p "$MNT/"
    sudo mount "${LOOP}p3" "$MNT"
    sudo mkdir -p "$MNT/boot/efi"

    # mount EFI partition
    sudo mount "${LOOP}p2" "$MNT/boot/efi"

    sudo cp "$WORK_DIR/vmlinuz" "$MNT/boot/"
    sudo cp "$WORK_DIR/initramfs.cpio.gz" "$MNT/boot/"

    sudo mkdir -p "$MNT/boot/grub"
    sudo cp "$SCRIPT_DIR/grub.cfg" "$MNT/boot/grub"

    # Install GRUB (BIOS + UEFI)
    sudo grub-install --target=i386-pc --boot-directory="$MNT/boot" "$LOOP"
    sudo grub-install --target=x86_64-efi --efi-directory="$MNT/boot/efi" --boot-directory="$MNT/boot" --removable

    sudo umount "$MNT/boot/efi"
    sudo umount "$MNT"

    sudo losetup -d "$LOOP"
fi

# Build qemu command
QEMU_CMD=( qemu-system-x86_64 )

if [ "$KVM" = true ]; then
    QEMU_CMD+=( -enable-kvm )
fi

QEMU_CMD+=( -m "$QEMU_MEM" )

if [ "$DISK_IMG" = true ]; then
    QEMU_CMD+=( -hda "$QEMU_DISK" )
else
    QEMU_CMD+=( -kernel "$QEMU_KERNEL" -initrd "$QEMU_INITRD" -append "$QEMU_APPEND" )
fi

if [ "$QEMU_NET" = true ]; then
    QEMU_CMD+=( -netdev user,id=mynet0 -device e1000,netdev=mynet0 )
fi

if [ "$QEMU_GRAPHICAL" = false ]; then
    QEMU_CMD+=( -nographic )
fi

QEMU_CMD+=( -machine "$QEMU_MACHINE" )

# KVM may need superuser
if [ "$KVM" = true ]; then
    sudo "${QEMU_CMD[@]}"
else
    "${QEMU_CMD[@]}"
fi