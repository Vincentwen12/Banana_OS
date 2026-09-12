#include "ata.h"
#include "port.h"
#include "vga.h"

/* 轮询等待状态寄存器：BSY=0 且 DRDY=1 */
static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            if (status & ATA_SR_DRDY) return 0;
        }
    }
    return -1;   /* 超时 */
}

/* 轮询等待数据请求：BSY=0 且 DRQ=1 */
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    return -1;   /* 超时 */
}

static void ata_400ns_delay(void) {
    /* 每次读 status 消耗约 100ns，读 4 次满足 400ns 最小间隔 */
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

void ata_init(void) {
    /* 检测主通道 master 设备是否存在并已就绪 */
    int ok = ata_wait_ready();
    if (ok == 0) {
        vga_puts("[ATA] Primary master detected\n");
    } else {
        vga_puts("[ATA] No device on primary master\n");
    }
}

/* 轮询等待命令完成：BSY=0（数据读完后用，确保本命令真正结束，
 * 残留 DRQ/BSY 会导致下一扇区命令被丢弃或错位读）。返回 ERR 位。 */
static int ata_wait_done(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            return (status & ATA_SR_ERR) ? -1 : 0;
        }
    }
    return -1;   /* 超时 */
}

/* 单次 PIO 读命令：dev 为 ATA_MASTER_LBA 或 ATA_SLAVE_LBA */
static int ata_pio_read_cmd(uint8_t dev, uint32_t lba, uint8_t* buffer) {
    /* 等控制器空闲，避免与上一次写（PIO 写返回后控制器可能仍忙）竞争，
     * 否则 READ 命令会被丢弃导致 DRQ 等待超时 */
    if (ata_wait_ready() < 0) return -1;

    /* 28-bit LBA 寻址 */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, dev | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO + ATA_REG_ERROR, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_SECT_COUNT, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ);

    if (ata_wait_drq() < 0) return -1;

    /* 512 字节 = 256 字 */
    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
        buffer[i * 2]     = (uint8_t)(word & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    /* 数据读完后等命令真正完成并查 ERR；不清除状态就发下一条命令会让
     * QEMU/真机偶发丢弃命令，DRQ 残留导致读到上一个扇区的旧数据（内容
     * 错位而非长度错误——python 读 .py 时表现为类定义缺失/NameError）。 */
    ata_400ns_delay();
    if (ata_wait_done() < 0) return -1;
    return 0;
}

/* 带重试的 PIO 读：命令偶发被丢弃/ERR 时（wait_drq 超时）自动清状态并
 * 重发，最多 3 次。不重试的话 ext2_read_file 中途 break，read 返回短数据，
 * ld-linux 报 "file too short"、python 编译 .py 缺块崩溃。 */
static int ata_pio_read(uint8_t dev, uint32_t lba, uint8_t* buffer) {
    if (!buffer) return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ata_pio_read_cmd(dev, lba, buffer) == 0) return 0;
        (void)inb(ATA_PRIMARY_IO + ATA_REG_ERROR);   /* 清 ERR */
        ata_400ns_delay();
    }
    return -1;
}

int ata_read_sector(uint32_t lba, uint8_t* buffer) {
    return ata_pio_read(ATA_MASTER_LBA, lba, buffer);
}

int ata_read_sector_slave(uint32_t lba, uint8_t* buffer) {
    return ata_pio_read(ATA_SLAVE_LBA, lba, buffer);
}

/* 通用 PIO 写：dev 为 ATA_MASTER_LBA 或 ATA_SLAVE_LBA */
static int ata_pio_write(uint8_t dev, uint32_t lba, const uint8_t* buffer) {
    if (!buffer) return -1;

    /* 等待控制器空闲（BSY=0 且 DRDY=1），否则连续写时上一次写未完成会丢命令 */
    if (ata_wait_ready() < 0) return -1;

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, dev | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO + ATA_REG_ERROR, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_SECT_COUNT, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE);

    if (ata_wait_drq() < 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)(buffer[i * 2] | (buffer[i * 2 + 1] << 8));
        outw(ATA_PRIMARY_IO + ATA_REG_DATA, word);
    }
    ata_400ns_delay();
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    return ata_pio_write(ATA_MASTER_LBA, lba, buffer);
}

int ata_write_sector_slave(uint32_t lba, const uint8_t* buffer) {
    return ata_pio_write(ATA_SLAVE_LBA, lba, buffer);
}

int ata_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer) {
    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_sector(lba + i, buffer + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    }
    return 0;
}

int ata_read_sectors_slave(uint32_t lba, uint32_t count, uint8_t* buffer) {
    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_sector_slave(lba + i, buffer + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    }
    return 0;
}

int ata_write_sectors_slave(uint32_t lba, uint32_t count, const uint8_t* buffer) {
    for (uint32_t i = 0; i < count; i++) {
        if (ata_write_sector_slave(lba + i, buffer + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    }
    return 0;
}