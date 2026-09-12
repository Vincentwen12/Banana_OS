#include "hello_elf.h"
#include "fs/tmpfs.h"

void hello_elf_init(void)
{
    /* Register the hello ELF binary in tmpfs.
     * NOTE: /bin/bash is no longer registered here — the real glibc dynamic
     * bash lives on the EXT2 image (injected via tools/fs_manifest.txt). */
    tmpfs_create_dir("/bin");
    tmpfs_create_file("/bin/hello", (void*)hello_elf_data, HELLO_ELF_SIZE);
}