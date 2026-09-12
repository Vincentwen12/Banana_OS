"""Generate a minimal ELF64 "hello world" binary for BananaOS testing.

Ring 3: uses the real `syscall` instruction with the Linux ABI
(rax=nr, rdi=a1, rsi=a2, rdx=a3), then exits via exit(0).
"""
import struct
import sys

# Fixed addresses
VADDR = 0x400000

msg = b"Hello from BananaOS ELF!\n"

# --- Constants ---
code_offset = 64 + 56  # ehdr + phdr = 120

code = bytearray()

# write(1, msg, len)
code += bytes([0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00])  # mov rax, 1
code += bytes([0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00])  # mov rdi, 1
lea_pos = len(code)
code += bytes([0x48, 0x8D, 0x35, 0x00, 0x00, 0x00, 0x00])  # lea rsi, [rip+msg]
code += bytes([0x48, 0xC7, 0xC2, 0x19, 0x00, 0x00, 0x00])  # mov rdx, len(25)
code += bytes([0x0F, 0x05])                               # syscall

# exit(0)
code += bytes([0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00])  # mov rax, 60
code += bytes([0x48, 0x31, 0xFF])                         # xor rdi, rdi
code += bytes([0x0F, 0x05])                               # syscall

code = bytes(code)

# --- Patch lea offset ---
msg_pos_in_file = code_offset + len(code)  # msg starts right after code
# lea instruction is at lea_pos in code, so its RIP-relative base is:
# VADDR + code_offset + lea_pos + 7 (next instruction)
lea_rip_base = VADDR + code_offset + lea_pos + 7
msg_vaddr = VADDR + msg_pos_in_file
lea_offset = msg_vaddr - lea_rip_base

code = (code[:lea_pos + 3] +
        struct.pack('<i', lea_offset) +
        code[lea_pos + 7:])

# --- Build ELF ---
entry = VADDR + code_offset
filesz = code_offset + len(code) + len(msg)

# Pad to 8-byte alignment
while filesz % 8 != 0:
    filesz += 1

ehdr = struct.pack(
    '<16sHHIQQQIHHHHHH',
    b'\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00',  # e_ident
    2,        # e_type = ET_EXEC
    0x3E,     # e_machine = EM_X86_64
    1,        # e_version
    entry,    # e_entry
    64,       # e_phoff
    0,        # e_shoff
    0,        # e_flags
    64,       # e_ehsize
    56,       # e_phentsize
    1,        # e_phnum
    0,        # e_shentsize
    0,        # e_shnum
    0,        # e_shstrndx
)

phdr = struct.pack(
    '<IIQQQQQQ',
    1,           # p_type = PT_LOAD
    7,           # p_flags = PF_R|PF_W|PF_X
    0,           # p_offset
    VADDR,       # p_vaddr
    VADDR,       # p_paddr
    filesz,      # p_filesz
    filesz,      # p_memsz
    0x1000,      # p_align
)

padding = filesz - code_offset - len(code) - len(msg)
binary = ehdr + phdr + code + msg + b'\x00' * padding

with open('hello.elf', 'wb') as f:
    f.write(binary)

print(f"Generated hello.elf: {len(binary)} bytes")
print(f"  Entry point: 0x{entry:x}")
print(f"  Code size: {len(code)} bytes")
print(f"  lea offset: {lea_offset} (0x{lea_offset & 0xFFFFFFFF:x})")
print(f"  Message: '{msg.decode()}'")

# Also generate a C header file with the binary embedded
with open('hello_elf.h', 'w') as f:
    f.write('/* Auto-generated ELF64 "hello world" binary */\n')
    f.write('/* Ring 3: uses real `syscall` instruction (Linux ABI) */\n')
    f.write(f'#define HELLO_ELF_SIZE {len(binary)}\n')
    f.write('static const unsigned char hello_elf_data[] = {\n')
    for i in range(0, len(binary), 16):
        chunk = binary[i:i+16]
        hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
        f.write(f'    {hex_str},\n')
    f.write('};\n')

print(f"Generated hello_elf.h")