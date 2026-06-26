
<p align="center">
  <img src="https://i.ibb.co.com/tT8VC9rZ/file-00000000af6c7208a660b807c37bf297.png" width="180" alt="NexOS Logo">
</p>

<h1 align="center">🦉 NexOS Kernel</h1>

<p align="center">
  <strong>Modern ARM Bare-Metal Operating System</strong><br>
  Built from Scratch using <strong>C</strong> &amp; <strong>Assembly</strong>
</p>

<p align="center">
  Fast • Lightweight • Modular • Open Source
</p>

<p align="center">

![License](https://img.shields.io/badge/License-GPL--3.0-blue?style=for-the-badge)
![Architecture](https://img.shields.io/badge/Architecture-ARM-success?style=for-the-badge)
![Language](https://img.shields.io/badge/Language-C%20%7C%20Assembly-orange?style=for-the-badge)
![Status](https://img.shields.io/badge/Status-Active%20Development-red?style=for-the-badge)

</p>

---

# 📖 Table of Contents

- 🇮🇩 Tentang
- 🇬🇧 About
- 🎯 Why NexOS?
- ✨ Current Features
- 🏗 Architecture
- 📂 Project Structure
- 🛠 Requirements
- 🚀 Build
- ▶ Run
- 🤝 Contributing
- 🐞 Report Bugs
- 🗺 Roadmap
- 📜 License
- 👨‍💻 Author

---

# 🇮🇩 Tentang

**NexOS** adalah sistem operasi **ARM Bare-Metal** yang dikembangkan sepenuhnya dari nol menggunakan bahasa **C** dan **Assembly**.

Proyek ini dibuat untuk mempelajari bagaimana sebuah sistem operasi bekerja dari tingkat paling rendah, mulai dari proses booting, interrupt, driver perangkat keras, hingga networking.

NexOS merupakan proyek **Open Source** yang dapat dipelajari, dimodifikasi, dan dikembangkan bersama oleh komunitas.

---

# 🇬🇧 About

**NexOS** is an **ARM Bare-Metal Operating System** built entirely from scratch using **C** and **Assembly**.

The project focuses on learning operating system development, low-level programming, hardware interaction, and kernel internals.

NexOS is open source and welcomes contributions from developers around the world.

---

# 🎯 Why NexOS?

## 🇮🇩

NexOS dibuat untuk:

- Mempelajari cara kerja sistem operasi.
- Memahami bootloader dan kernel.
- Mengembangkan driver perangkat keras.
- Membangun networking stack sendiri.
- Menjadi proyek Open Source untuk belajar bersama.
- Menyediakan kode yang bersih dan mudah dipahami.

## 🇬🇧

NexOS was created to:

- Learn operating system development.
- Understand the boot process.
- Build hardware drivers.
- Implement networking.
- Create an educational Open Source project.
- Maintain clean and modular source code.

---

# ✨ Current Features

- ARM Bare-Metal Kernel
- Bootloader
- Startup Assembly
- Interrupt Handling (IRQ)
- UART Driver
- Ring Buffer
- Generic Interrupt Controller (GIC)
- VirtIO Network Driver
- VirtIO Block Driver
- DHCP Client
- DNS Resolver
- TCP/IP Stack
- Basic File System
- Modular Kernel Design

---

# 🏗 Architecture

```text
Power On
    │
Bootloader
    │
startup.s
    │
kernel_main()
    │
Memory Initialization
    │
Interrupt Controller
    │
Drivers
    │
Networking
    │
Shell
```

---

# 📂 Project Structure

```text
boot/       Bootloader & Startup Code
kernel/     Core Kernel
drivers/    Hardware Drivers
net/        TCP/IP, DHCP, DNS
fs/         File System
include/    Header Files
lib/        Utility Library
scripts/    Build Scripts
docs/       Documentation
```

---

# 🛠 Requirements

- ARM GCC Toolchain
- GNU Make
- Git
- QEMU

---

# 🚀 Build

```bash
make
```

---

# ▶ Run

```bash
make run
```

---

# 🤝 Contributing

Everyone is welcome to contribute.

You can:

- 🐞 Fix bugs
- ✨ Add new features
- ⚡ Improve performance
- 📖 Improve documentation
- 🔧 Add new drivers

Workflow:

1. Fork this repository.
2. Create a branch.
3. Commit your changes.
4. Push your branch.
5. Open a Pull Request.

---

# 🐞 Report Bugs

If you find a bug, please open an **Issue**.

Include:

- NexOS Version
- Build Environment
- Steps to Reproduce
- Expected Result
- Actual Result
- Logs or Screenshots

---

# 🗺 Roadmap

- ✅ Bootloader
- ✅ ARM Startup
- ✅ UART Driver
- ✅ IRQ
- ✅ VirtIO Network
- ✅ TCP/IP
- ✅ DHCP
- ✅ DNS
- 🔄 Scheduler
- 🔄 Virtual Memory
- 🔄 User Space
- 🔄 ELF Loader
- 🔄 USB Driver
- 🔄 SMP Support
- 🔄 GUI

---

📜 GNU General Public License (GPL v3) Summary

The GNU General Public License (GPL) version 3 is a free and open-source software license created by the Free Software Foundation (FSF).

Its purpose is to ensure that software remains free to use, study, modify, and share, while protecting those freedoms for everyone.

✅ What You Can Do

- Use the software for any purpose.
- Study how the software works.
- Access the source code.
- Modify the source code.
- Share original copies.
- Distribute modified versions.

📌 Your Responsibilities

If you distribute the software or a modified version:

- Keep the software under the GPL v3 license.
- Make the source code available.
- Include the copyright notice.
- Include a copy of the GPL v3 license.
- Clearly indicate that changes have been made.

❌ What You Cannot Do

- Remove users' GPL rights.
- Distribute modified versions under a closed-source license.
- Prevent recipients from accessing the source code when required by GPL.
- Misrepresent the original authorship of the project.

⚠ No Warranty

Software released under the GPL v3 is provided "as is", without any warranty. The authors are not responsible for any damages or problems resulting from its use.

❤️ Open Source Philosophy

GPL v3 is designed to protect software freedom. It encourages collaboration, learning, and community-driven development while ensuring that improvements shared with others remain open under the same license.

---

This is only a summary.

For the complete legal terms and conditions, please read the official GNU General Public License Version 3 included in the "LICENSE" file.

---

# 👨‍💻 developer 

**rizky**

Founder of the **NexOS Project**

Made with ❤️ in Indonesia.

---

# ⭐ Support

If you like NexOS:

- ⭐ Star this repository.
- 🍴 Fork the project.
- 🐞 Report bugs.
- 💡 Suggest new ideas.
- 🤝 Contribute to the kernel.

**Together we can build NexOS into a real operating system.**
