# Makefile — NexOS v0.3
# Requires: arm-none-eabi-gcc, qemu-system-arm

# ── Toolchain ────────────────────────────────────────────
PREFIX   = arm-none-eabi
CC       = $(PREFIX)-gcc
AS       = $(PREFIX)-as
LD       = $(PREFIX)-ld
OBJCOPY  = $(PREFIX)-objcopy
OBJDUMP  = $(PREFIX)-objdump

# ── Files ─────────────────────────────────────────────────
TARGET   = nexos
SRCS_C   = kernel_v0_3.c virtio_net.c virtio_blk.c nfs.c tcp.c dns.c dhcp.c
SRCS_S   = startup.s
LDSCRIPT = linker.ld

OBJS     = startup.o kernel_v0_3.o virtio_net.o virtio_blk.o nfs.o tcp.o dns.o dhcp.o

# ── Flags ─────────────────────────────────────────────────
# -mcpu=cortex-a15   : target CPU QEMU virt
# -marm              : generate ARM (32-bit) instructions, bukan Thumb
# -ffreestanding     : tidak ada stdlib, tidak ada main() assumption
# -nostdlib          : jangan link libc/libgcc startup
# -O1                : optimisasi ringan, masih debug-friendly
# -Wall              : semua warning
# -g                 : debug info (untuk gdb)
CFLAGS   = -mcpu=cortex-a15 -marm -ffreestanding -nostdlib \
           -O1 -Wall -Wextra \
           -Wno-unused-parameter \
           -g

ASFLAGS  = -mcpu=cortex-a15

LDFLAGS  = -T $(LDSCRIPT) -nostdlib

# ── QEMU ──────────────────────────────────────────────────
QEMU        = qemu-system-arm
QEMU_MACHINE= virt
QEMU_CPU    = cortex-a15
QEMU_MEM    = 128M
QEMU_FLAGS  = -machine $(QEMU_MACHINE) \
              -cpu $(QEMU_CPU)         \
              -m $(QEMU_MEM)           \
              -nographic               \
              -kernel $(TARGET).elf    \
              -netdev user,id=net0     \
              -device virtio-net-device,netdev=net0 \
              -drive file=nexos_disk.img,format=raw,if=none,id=blk0 \
              -device virtio-blk-device,drive=blk0

# ── Targets ───────────────────────────────────────────────

.PHONY: all run debug clean dump

## Default: build ELF
all: $(TARGET).elf
	@echo ""
	@echo "  ✓ Build sukses: $(TARGET).elf"
	@echo "  Jalankan dengan: make run"
	@echo ""

## Compile startup assembly
startup.o: startup.s
	$(CC) $(CFLAGS) -c $< -o $@

## Compile kernel C
kernel_v0_3.o: kernel_v0_3.c
	$(CC) $(CFLAGS) -c $< -o $@

## Compile TCP stack
tcp.o: tcp.c tcp.h virtio_net.h
	$(CC) $(CFLAGS) -c $< -o $@
	$(CC) $(CFLAGS) -c $< -o $@

## Compile DNS resolver
dns.o: dns.c dns.h virtio_net.h nexos_time.h
	$(CC) $(CFLAGS) -c $< -o $@

## Compile DHCP client
dhcp.o: dhcp.c dhcp.h virtio_net.h nexos_time.h
	$(CC) $(CFLAGS) -c $< -o $@


## Buat disk image (jalankan sekali sebelum pertama kali run)
disk:
	@if [ ! -f nexos_disk.img ]; then \
		dd if=/dev/zero of=nexos_disk.img bs=1M count=32; \
		echo "  ✓ nexos_disk.img (32MB) dibuat"; \
	else \
		echo "  nexos_disk.img sudah ada"; \
	fi

## Compile virtio-blk driver
virtio_blk.o: virtio_blk.c virtio_blk.h
	$(CC) $(CFLAGS) -c $< -o $@

## Compile NFS filesystem
nfs.o: nfs.c nfs.h virtio_blk.h
	$(CC) $(CFLAGS) -c $< -o $@

## Link semua jadi ELF
$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

## Jalankan di QEMU
## Ctrl+A lalu X untuk keluar dari QEMU
run: $(TARGET).elf
	@echo "  Starting NexOS di QEMU..."
	@echo "  [Ctrl+A lalu X untuk keluar]"
	@echo ""
	$(QEMU) $(QEMU_FLAGS)

## Debug mode: QEMU nunggu GDB di port 1234
## Di terminal lain: arm-none-eabi-gdb nexos.elf
##   (gdb) target remote :1234
##   (gdb) continue
debug: $(TARGET).elf
	@echo "  Debug mode — tunggu GDB di port 1234"
	@echo "  Di terminal lain: arm-none-eabi-gdb nexos.elf"
	@echo "    (gdb) target remote :1234"
	$(QEMU) $(QEMU_FLAGS) -s -S

## Dump disassembly (berguna untuk debug)
dump: $(TARGET).elf
	$(OBJDUMP) -d -S $(TARGET).elf > $(TARGET).dump
	@echo "  Disassembly tersimpan di $(TARGET).dump"

## Bersihkan build artifacts
clean:
	rm -f *.o *.elf *.dump
	@echo "  Cleaned."

## Info: tampil ukuran sections
size: $(TARGET).elf
	$(PREFIX)-size $(TARGET).elf
