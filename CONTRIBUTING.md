# 🤝 Contributing to NexOS

<p align="center">
Welcome to the NexOS Project! 🚀
</p>

Thank you for your interest in contributing to **NexOS**.

Every contribution—whether it's fixing bugs, improving documentation, writing drivers, or adding new features—helps make NexOS a better open-source operating system.

---

# 📚 Table of Contents

- 🇮🇩 Kontribusi
- 🇬🇧 Contributing
- 🛠 Getting Started
- 🌿 Branch Naming
- 💻 Coding Style
- 📦 Commit Messages
- 🐞 Reporting Bugs
- 💡 Feature Requests
- 📜 Pull Request Guidelines
- ❤️ Community

---

# 🇮🇩 Kontribusi

NexOS adalah proyek **Open Source** yang dikembangkan untuk pembelajaran, penelitian, dan eksplorasi sistem operasi modern.

Kami menerima kontribusi dari siapa saja.

Anda dapat membantu dengan:

- 🐞 Memperbaiki bug
- 📖 Memperbaiki dokumentasi
- ⚡ Optimasi performa
- 🔧 Menambahkan driver baru
- 🌐 Mengembangkan networking
- 🧠 Memory Management
- 💾 File System
- 🖥 Kernel Development
- ✨ Fitur baru

Tidak masalah jika Anda masih pemula.

Setiap kontribusi sangat berarti.

---

# 🇬🇧 Contributing

NexOS is an educational open-source operating system.

We welcome developers from all experience levels.

You can contribute by:

- Fixing bugs
- Improving documentation
- Optimizing performance
- Writing device drivers
- Developing networking
- Improving memory management
- Adding kernel features
- Testing on ARM hardware

Every contribution matters.

---

# 🛠 Getting Started

1. Fork this repository.

2. Clone your fork.

```bash
git clone https://github.com/YOUR_USERNAME/NexOS-Kernel.git
```

3. Enter the project.

```bash
cd NexOS-Kernel
```

4. Create a new branch.

```bash
git checkout -b feature/my-feature
```

5. Make your changes.

6. Commit your work.

```bash
git commit -m "Add UART optimization"
```

7. Push your branch.

```bash
git push origin feature/my-feature
```

8. Open a Pull Request.

---

# 🌿 Branch Naming

Please use meaningful branch names.

Examples:

```text
feature/network-stack
feature/usb-driver
bugfix/uart
bugfix/memory
docs/readme-update
```

---

# 💻 Coding Style

### C

- Use 4 spaces indentation.
- Keep code readable.
- Use meaningful variable names.
- Keep functions small.
- Comment complex logic.

Example

```c
void uart_init(void)
{
    uart_enable();
}
```

---

### Assembly

- Use descriptive labels.
- Comment important instructions.
- Keep startup code organized.

---

# 📦 Commit Messages

Use clear commit messages.

Good examples:

```text
Add VirtIO block driver

Fix UART interrupt bug

Improve TCP performance

Update README

Refactor scheduler
```

Avoid:

```text
Update

Fix

asdf

123
```

---

# 🐞 Reporting Bugs

If you find a bug, please create an Issue.

Include:

- NexOS version
- ARM platform
- Compiler version
- Steps to reproduce
- Expected behavior
- Actual behavior
- Logs
- Screenshots (optional)

---

# 💡 Feature Requests

Before requesting a feature:

- Search existing Issues.
- Explain the problem.
- Describe the proposed solution.
- Explain why it would improve NexOS.

---

# 📜 Pull Request Guidelines

Before opening a Pull Request:

- Build successfully.
- Test your changes.
- Keep commits clean.
- Update documentation if needed.
- Follow the existing coding style.

Large changes should be discussed before implementation.

---

# ❤️ Community

Please respect all contributors.

We value:

- Respect
- Collaboration
- Learning
- Open communication
- Clean code

NexOS is built by the community, for the community.

---

# 📄 License

By contributing to NexOS, you agree that your contributions will be licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

---

# 🙏 Thank You

Thank you for helping improve NexOS.

Every line of code, every bug report, and every idea helps this project grow.

<p align="center">

🦉 **Happy Coding!**

**The NexOS Project**

</p>
