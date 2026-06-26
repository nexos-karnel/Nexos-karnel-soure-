# 🛠 Building NexOS

<p align="center">

**Build, Run, and Test the NexOS Kernel**

</p>

---

# 📚 Table of Contents

- Introduction
- System Requirements
- Required Tools
- Clone Repository
- Build
- Run
- Clean Build
- Testing
- Real Hardware
- Troubleshooting

---

# 📖 Introduction

This document explains how to build, run, and test the NexOS kernel.

NexOS is developed entirely from scratch using **C** and **Assembly**, targeting **ARM Bare-Metal** platforms.

---

# 💻 System Requirements

Recommended operating systems:

- Linux
- macOS
- Windows (WSL2 recommended)

---

# 🔧 Required Tools

Install the following tools before building NexOS:

- ARM GCC Toolchain
- GNU Make
- Git
- QEMU (ARM)
- GDB *(Optional)*

---

# 📥 Clone Repository

```bash
git clone https://github.com/nexos-kernel/NexOS-Kernel.git
cd nexos
```

Replace the repository URL with your fork if necessary.

---

# ⚙ Build

Compile the kernel:

```bash
make
```

If the build completes successfully, the kernel image will be generated in the output directory.

---

# ▶ Run

Run NexOS using QEMU:

```bash
make run
```

If your Makefile uses a different target, update this command accordingly.

---

# 🧹 Clean Build

Remove all generated files:

```bash
make clean
```

To rebuild from scratch:

```bash
make clean
make
```

---

# 🧪 Testing

Recommended testing methods:

- QEMU ARM Emulator
- Raspberry Pi (supported models)
- ARM development boards
- Supported TV Boxes (when hardware support is available)

Always test new features before submitting a Pull Request.

---

# 🔍 Debugging

Run QEMU with GDB support if available:

```bash
make debug
```

Then connect using:

```bash
gdb
```

Refer to your Makefile for the exact debug configuration.

---

# 📦 Build Output

Typical build artifacts include:

```text
build/
├── kernel.elf
├── kernel.bin
├── kernel.img
└── mapfile.map
```

The exact filenames may vary depending on your build configuration.

---

# ❗ Troubleshooting

## Build Failed

Check that:

- ARM GCC Toolchain is installed.
- GNU Make is available.
- All project dependencies are installed.
- You are using a supported compiler version.

---

## QEMU Does Not Start

Verify:

- QEMU is installed correctly.
- The kernel image was built successfully.
- The QEMU machine type matches your target platform.

---

## Hardware Does Not Boot

Possible causes:

- Incorrect bootloader configuration.
- Unsupported hardware.
- Invalid kernel image.
- Missing device initialization.

---

# 🤝 Contributing

If you improve the build system, documentation, or toolchain support, please submit a Pull Request.

Community contributions are always welcome.

---

# ❤️ Thank You

Thank you for helping improve the NexOS Project.

Happy Building!

<p align="center">

🦉 **The NexOS Project**

Designed and developed from scratch using C & Assembly.

Made with ❤️ in Indonesia.

</p>
