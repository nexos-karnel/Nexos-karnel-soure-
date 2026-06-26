# 🏗 NexOS Architecture

<p align="center">

**Modern ARM Bare-Metal Operating System**

Designed for Performance • Simplicity • Modularity • Learning

</p>

---

# 📚 Table of Contents

- Overview
- Design Philosophy
- Architecture Overview
- Boot Process
- Memory Layout
- Kernel Components
- Driver Architecture
- Networking Stack
- File System
- Source Tree
- Future Development

---

# 📖 Overview

NexOS is a modern **ARM Bare-Metal Operating System** developed entirely from scratch using **C** and **Assembly**.

Unlike traditional operating systems, NexOS runs directly on hardware without relying on the Linux kernel or any existing operating system.

The project is designed for education, research, and real-world kernel development.

---

# 🎯 Design Philosophy

NexOS follows several core principles:

- Clean and readable source code
- Modular kernel architecture
- High performance
- Easy to maintain
- Easy to extend
- Open Source collaboration

Every subsystem is designed to be as independent as possible.

---

# 🏛 Architecture Overview

```text
+------------------------------------------------------+
|                  User Applications                   |
+------------------------------------------------------+
|                System Call Interface                 |
+------------------------------------------------------+
|                    NexOS Kernel                      |
|------------------------------------------------------|
| Scheduler | Memory | VFS | Network | Drivers | IPC   |
+------------------------------------------------------+
|         Hardware Abstraction Layer (HAL)             |
+------------------------------------------------------+
|              ARM CPU & Hardware Devices              |
+------------------------------------------------------+
```

---

# ⚙ Boot Process

The system startup sequence:

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
Memory Initialization
    │
    ▼
Interrupt Controller
    │
    ▼
Device Drivers
    │
    ▼
Networking
    │
    ▼
Kernel Services
```

---

# 🧠 Memory Layout

```text
+------------------------------+
| Bootloader                   |
+------------------------------+
| Kernel Code (.text)          |
+------------------------------+
| Read-Only Data (.rodata)     |
+------------------------------+
| Initialized Data (.data)     |
+------------------------------+
| BSS                          |
+------------------------------+
| Kernel Heap                  |
+------------------------------+
| Kernel Stack                 |
+------------------------------+
| Device Memory                |
+------------------------------+
```

---

# 🧩 Kernel Components

## Bootloader

- Initializes the hardware
- Loads the kernel into memory
- Transfers execution to the kernel

---

## Kernel Core

Responsible for:

- Kernel initialization
- Exception handling
- Interrupt management
- Resource management
- Core system services

---

## Memory Manager

Handles:

- Physical memory
- Dynamic memory allocation
- Memory mapping
- Future virtual memory support

---

## Scheduler *(Planned)*

Future responsibilities:

- Process scheduling
- Thread scheduling
- Context switching
- CPU time allocation

---

## Interrupt Manager

Responsible for:

- IRQ handling
- Exception handling
- Timer interrupts
- Device interrupts

---

## Device Drivers

Current and future drivers include:

- UART
- GPIO
- Timer
- VirtIO
- Storage
- USB
- PCI
- Display

---

# 🌐 Networking Stack

Current networking architecture:

```text
Application
      │
TCP
UDP
ICMP
IPv4
ARP
Ethernet
VirtIO Network Driver
Hardware
```

Supported protocols include:

- ARP
- IPv4
- ICMP
- UDP
- TCP
- DHCP
- DNS

---

# 💾 File System

The file system layer provides:

- File management
- Directory management
- Storage abstraction

Future support may include:

- FAT32
- EXT2
- EXT4
- NexFS (Native File System)

---

# 🔌 Driver Architecture

```text
Application
      │
Kernel API
      │
Driver Manager
      │
 ┌────┼────┬────┐
 │    │    │    │
UART Timer GPIO VirtIO
 │    │    │    │
 └────┴────┴────┘
      │
Hardware
```

---

# 📂 Source Tree

```text
boot/
    Bootloader and startup code

kernel/
    Kernel core

drivers/
    Hardware drivers

net/
    Networking stack

fs/
    File system

include/
    Header files

lib/
    Utility libraries

scripts/
    Build scripts

docs/
    Documentation
```

---

# 🚀 Future Development

Planned features include:

- Preemptive Scheduler
- Virtual Memory
- User Space
- ELF Loader
- USB Stack
- PCI Support
- SMP Support
- Graphics Subsystem
- Package Manager
- Security Framework
- Power Management

---

# ❤️ Architecture Goals

The NexOS architecture is designed to provide:

- High performance
- Clean code structure
- Modular design
- Scalability
- Easy maintenance
- Educational value
- Long-term extensibility

Every subsystem is built with simplicity and maintainability in mind, making NexOS suitable for both learning and real hardware development.

---

# 👨‍💻 developer 

**Rizky**

Founder of the NexOS Project

**Designed and developed from scratch using C & Assembly.**

Made with ❤️ in Indonesia.
