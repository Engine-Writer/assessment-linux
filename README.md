# Assessment Linux

This project provides a **minimal Linux-based assessment environment** that builds a custom kernel and filesystem image, boots under QEMU, and runs BusyBox as the userland (not really userland when you have superuser, but still helps with general-purpose use)
---

## Project Overview

* **Init + BusyBox**: A minimal root filesystem running BusyBox with a custom init process.
* **Race Condition Handling**: Special flags (`ttyS0_active`, `tty0_active`) prevent input-stealing race conditions between the init process and BusyBox.
* **Build System**: Shell/Python-based automation for compiling, packaging, and running the kernel + rootfs under QEMU.
* **Image Creation**: Automatically creates an initramfs and wraps it with a kernel to form a bootable Linux system.
* **Emulation**: Supports running in QEMU for easy testing.

---

## Key Features

* Minimal bootable Linux environment with BusyBox.
* Custom `init` program to handle startup logic.
* Boolean flags to avoid keypress race conditions between init and BusyBox.
* Automated build process for filesystem + kernel image.
* Optional QEMU integration for direct testing.
* Clean, reproducible environment suitable for assessment or OS learning tasks.

---

## Directory Structure

```
/assessment-linux
|
+-- build.sh
+-- grub.cfg
+-- init.c
+-- busybox_amd64                       # Prebuilt busybox for AMD64
+-- busybox_*_amd64.deb                 # Busybox source package for additional diclaimer
+-- linux-(Linux kernel version)/       # Linux kernel source copy in CWD
+-- linux-(Linux kernel version).tar.xz # Linux kernel source tar in CWD
+-- build-(Linux kernel version)/       # Build output for the given version of the linux kernel
+-- initramfs/                          # initramfs data to be packed.
|   +-- init                            # Initial code to be executed by the kernel
|   +-- bin/                            # Contains busybox and any additional binaries
|   +-- usr/                            # Contains busybox licenses and any additional binaries
+-- README.md                           # This file
+-- .gitignore                          # Hide editor junk
```

---

## How to Build and Run

One very simple command to do everything

```bash
./build.sh
```

You should see a QEMU no-graphic appear in your CLI, to close it, press Ctrl+A then X

---

## Dependencies (Required)

* GCC toolchain (`gcc`, `ld`)
* GNU Make
* Grub-install (grub-pc-bin, grub-efi-amd64-bin) for BIOS + UEFI install
* Any Debian-based distro (for apt). Tested on Ubuntu 24 (WSL2)
* QEMU (qemu-system-x86, qemu-kvm if you want to test real-time speed)
* mkfs toolset (mkfs.fat (may require dosfstools), mkfs.ext4)
* A Personal Computer (Optional)

---

## Project Goals

* Provide a minimal Linux-based system that boots quickly.
* Offer a reproducible testbed for assessment or educational purposes.

---

## License

This project is not licensed because it is an interview project. The code may be used freely anywhere as long as the file is equiped with a visible disclaimer linking to this GitHub repository