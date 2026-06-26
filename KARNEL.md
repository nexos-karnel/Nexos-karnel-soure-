# 🧠 NexOS Kernel

<p align="center">

**The Heart of the NexOS Operating System**

Built from Scratch using **C** and **Assembly**

</p>

---

# 📚 Table of Contents

- Overview
- Design Philosophy
- Kernel Responsibilities
- Boot Sequence
- Kernel Initialization
- Memory Management
- Interrupt Handling
- Driver Subsystem
- Networking
- File System
- Future Development

---

# 📖 Overview

The NexOS Kernel is the core component of the operating system.

It is responsible for managing hardware resources, providing system services, handling interrupts, managing memory, and supporting communication between hardware and software.

NexOS is developed entirely from scratch without using the Linux kernel or any existing operating system kernel.

---

# 🎯 Design Philosophy

The kernel is designed with the following goals:

- Simplicity
- Performance
- Stability
- Modularity
- Readability
- Educational Value

Every subsystem is kept as independent as possible to simplify development and maintenance.

---

# ⚙ Kernel Responsibilities

The kernel is responsible for:

- Hardware initialization
- CPU management
- Memory management
- Interrupt handling
- Device driver management
- Networking
- File system support
- System services

---

# 🚀 Boot Sequence

The kernel starts after the bootloader transfers execution.

```text
Power On
    │
    ▼
Boot ROM
    │
    ▼
Bootloader
    │
    ▼
startup.s
    │
    ▼
kernel_main()
    │
    ▼
Initialize Memory
    │
    ▼
Initialize Interrupts
    │
    ▼
Initialize Drivers
    │
    ▼
Initialize Network
    │
    ▼
Kernel Ready
```

---

# 🛠 Kernel Initialization

During startup the kernel performs:

1. CPU initialization
2. Stack initialization
3. Memory setup
4. Interrupt controller setup
5. Timer initialization
6. UART initialization
7. Driver initialization
8. Networking initialization
9. File system initialization
10. Start kernel services

---

# 🧠 Memory Management

The memory manager is responsible for:

- Physical memory allocation
- Heap management
- Stack management
- Memory protection
- Future virtual memory support

Future improvements include paging and virtual memory.

---

# ⚡ Interrupt Handling

The interrupt subsystem manages:

- IRQs
- CPU exceptions
- Timer interrupts
- Hardware interrupts
- Software interrupts (future)

Fast interrupt handling is essential for system stability and responsiveness.

---

# 🔌 Driver Subsystem

Drivers provide communication between the kernel and hardware devices.

Current drivers include:

- UART
- Generic Interrupt Controller (GIC)
- VirtIO Network
- VirtIO Block

Future drivers:

- USB
- PCI
- GPIO
- Display
- Audio
- Wi-Fi
- Bluetooth

---

# 🌐 Networking

The networking subsystem currently supports:

- Ethernet
- ARP
- IPv4
- ICMP
- UDP
- TCP
- DHCP
- DNS

The long-term goal is to provide a complete networking stack suitable for real hardware.

---

# 💾 File System

The file system layer provides:

- File management
- Directory management
- Storage abstraction

Future support:

- FAT32
- EXT2
- EXT4
- NexFS

---

# 🚧 Future Development

Planned kernel features:

- Preemptive Scheduler
- Multitasking
- User Space
- ELF Loader
- Virtual Memory
- SMP Support
- Security Framework
- Power Management
- Module Loader
- Real Hardware Optimization

---

# ❤️ Kernel Philosophy

The NexOS Kernel is built to be:

- Lightweight
- Fast
- Modular
- Easy to understand
- Easy to maintain
- Easy to extend

The primary objective is to provide a modern educational operating system while remaining suitable for experimentation on real ARM hardware.

---

# 👨‍💻 Author

**rizky**

Founder of the NexOS Project

Designed and developed from scratch using **C** and **Assembly**.

Made with ❤️ in Indonesia.
