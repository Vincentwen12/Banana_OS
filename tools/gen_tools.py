#!/usr/bin/env python3
"""Generate minimal ELF64 /bin/cat and /bin/rm for BananaOS W6 tests.

Ring 3: uses the real `syscall` instruction with the Linux ABI
(rax=nr, rdi=a1, rsi=a2, rdx=a3). User stack layout built by the kernel:
rsp -> argc, rsp+8 -> argv[0], rsp+16 -> argv[1].

Usage: python gen_tools.py
Output: tools/cat.elf, tools/rm.elf (injected into fs.img via fs_manifest.txt)
"""
import struct

VADDR = 0x400000
code_offset = 64 + 56  # ehdr + phdr = 120


class Asm:
    def __init__(self):
        self.b = bytearray()

    def mov_rax_imm32(self, v):
        self.b += bytes([0x48, 0xC7, 0xC0]) + struct.pack('<I', v & 0xFFFFFFFF)

    def mov_rdi_imm32(self, v):
        self.b += bytes([0x48, 0xC7, 0xC7]) + struct.pack('<I', v & 0xFFFFFFFF)

    def mov_rdi_mem_rsp_16(self):
        self.b += bytes([0x48, 0x8B, 0x7C, 0x24, 0x10])

    def mov_rsi_mem_rsp_16(self):
        self.b += bytes([0x48, 0x8B, 0x74, 0x24, 0x10])

    def mov_rsi_imm64(self, addr):
        self.b += bytes([0x48, 0xBE]) + struct.pack('<Q', addr)

    def mov_rdx_imm32(self, v):
        self.b += bytes([0x48, 0xC7, 0xC2]) + struct.pack('<I', v & 0xFFFFFFFF)

    def xor_rsi(self):
        self.b += bytes([0x48, 0x31, 0xF6])

    def xor_rdx(self):
        self.b += bytes([0x48, 0x31, 0xD2])

    def xor_rdi(self):
        self.b += bytes([0x48, 0x31, 0xFF])

    def mov_r12_rax(self):
        self.b += bytes([0x49, 0x89, 0xC4])

    def mov_r13_rax(self):
        self.b += bytes([0x49, 0x89, 0xC5])

    def mov_rdi_r12(self):
        self.b += bytes([0x4C, 0x89, 0xE7])

    def mov_rdx_r13(self):
        self.b += bytes([0x4C, 0x89, 0xEA])

    def syscall(self):
        self.b += bytes([0x0F, 0x05])

    def test_rax_rax(self):
        self.b += bytes([0x48, 0x85, 0xC0])

    def js(self, target_rel8):
        self.b += bytes([0x7C, target_rel8 & 0xFF])

    def jle(self, target_rel8):
        self.b += bytes([0x7E, target_rel8 & 0xFF])

    def jmp(self, target_rel8):
        self.b += bytes([0xEB, target_rel8 & 0xFF])

    def off(self):
        return len(self.b)


def build_cat():
    a = Asm()
    a.mov_rax_imm32(2)               # SYS_open
    a.mov_rdi_mem_rsp_16()           # path = argv[1]
    a.xor_rsi()                      # O_RDONLY
    a.xor_rdx()                      # mode = 0
    a.syscall()
    a.test_rax_rax()
    js_fail_off = a.off(); a.js(0)
    a.mov_r12_rax()                  # fd
    loop_off = a.off()
    a.mov_rax_imm32(0)               # SYS_read
    a.mov_rdi_r12()
    buf_read_off = a.off(); a.mov_rsi_imm64(0)
    a.mov_rdx_imm32(4096)
    a.syscall()
    a.test_rax_rax()
    jle_done_off = a.off(); a.jle(0)
    a.mov_r13_rax()                  # n
    a.mov_rax_imm32(1)               # SYS_write
    a.mov_rdi_imm32(1)               # stdout
    buf_write_off = a.off(); a.mov_rsi_imm64(0)
    a.mov_rdx_r13()
    a.syscall()
    a.jmp(loop_off - (a.off() + 2))
    done_off = a.off()
    a.mov_rax_imm32(60)              # SYS_exit
    a.xor_rdi()
    a.syscall()
    fail_off = a.off()
    a.mov_rax_imm32(1)               # SYS_write
    a.mov_rdi_imm32(2)               # stderr
    err_off = a.off(); a.mov_rsi_imm64(0)
    a.mov_rdx_imm32(17)              # len("cat: open failed\n")
    a.syscall()
    a.mov_rax_imm32(60)
    a.xor_rdi()
    a.syscall()

    code = bytes(a.b)
    buf_addr = VADDR + code_offset + len(code)
    err_addr = buf_addr + 4096

    # Patch the three rsi imm64 slots.
    for off in (buf_read_off, buf_write_off):
        struct.pack_into('<Q', a.b, off + 2, buf_addr)
    struct.pack_into('<Q', a.b, err_off + 2, err_addr)

    # Patch relative jumps.
    a.b[js_fail_off + 1] = (fail_off - (js_fail_off + 2)) & 0xFF
    a.b[jle_done_off + 1] = (done_off - (jle_done_off + 2)) & 0xFF

    payload = bytes(a.b) + b"\x00" * 4096 + b"cat: open failed\n"
    return payload


def build_rm():
    a = Asm()
    a.mov_rax_imm32(263)             # SYS_unlinkat
    a.mov_rdi_imm32(-100 & 0xFFFFFFFF)  # AT_FDCWD
    a.mov_rsi_mem_rsp_16()           # path = argv[1]
    a.xor_rdx()                      # flags = 0
    a.syscall()
    a.test_rax_rax()
    js_fail_off = a.off(); a.js(0)
    a.mov_rax_imm32(60)              # SYS_exit(0)
    a.xor_rdi()
    a.syscall()
    fail_off = a.off()
    a.mov_rax_imm32(1)               # SYS_write
    a.mov_rdi_imm32(2)               # stderr
    err_off = a.off(); a.mov_rsi_imm64(0)
    a.mov_rdx_imm32(19)              # len("rm: unlink failed\n")
    a.syscall()
    a.mov_rax_imm32(60)
    a.xor_rdi()
    a.syscall()

    err_addr = VADDR + code_offset + len(a.b)
    struct.pack_into('<Q', a.b, err_off + 2, err_addr)
    a.b[js_fail_off + 1] = (fail_off - (js_fail_off + 2)) & 0xFF

    return bytes(a.b) + b"rm: unlink failed\n"


def emit(path, payload):
    # PT_LOAD 覆盖整个文件（含 ehdr+phdr，偏移 0 起），否则 entry 落在映射外
    filesz = code_offset + len(payload)
    while filesz % 8 != 0:
        filesz += 1
    ehdr = struct.pack(
        '<16sHHIQQQIHHHHHH',
        b'\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00',
        2, 0x3E, 1, VADDR + code_offset, 64, 0, 0, 64, 56, 1, 0, 0, 0,
    )
    phdr = struct.pack(
        '<IIQQQQQQ',
        1, 7, 0, VADDR, VADDR, filesz, filesz, 0x1000,
    )
    pad = filesz - code_offset - len(payload)
    with open(path, 'wb') as f:
        f.write(ehdr + phdr + payload + b'\x00' * pad)
    print(f"Generated {path}: {filesz} bytes, entry=0x{VADDR + code_offset:x}")


def main():
    emit('cat.elf', build_cat())
    emit('rm.elf', build_rm())


if __name__ == '__main__':
    main()
