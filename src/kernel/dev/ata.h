#ifndef ATA_H
#define ATA_H

#include "axion.h"

/* Primary IDE channel (QEMU / 真实 IDE 兼容) */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECTOR_SIZE     512

/* Port offsets relative to ATA_PRIMARY_IO */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECT_COUNT  0x02
#define ATA_REG_LBA_LOW     0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HIGH    0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07

/* Commands */
#define ATA_CMD_READ        0x20   /* READ SECTORS (28-bit LBA) */
#define ATA_CMD_WRITE       0x30   /* WRITE SECTORS (28-bit LBA) */
#define ATA_CMD_IDENTIFY    0xEC

/* Status register bits */
#define ATA_SR_ERR          0x01
#define ATA_SR_DRQ          0x08
#define ATA_SR_DRDY         0x40
#define ATA_SR_BSY          0x80

/* Drive/Head register: LBA mode + master / slave */
#define ATA_MASTER_LBA      0xE0
#define ATA_SLAVE_LBA       0xF0

void ata_init(void);
int  ata_read_sector(uint32_t lba, uint8_t* buffer);
int  ata_write_sector(uint32_t lba, const uint8_t* buffer);
int  ata_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);

/* 从主通道 slave 设备读取（第二块 IDE 盘，承载 EXT2 文件系统） */
int  ata_read_sector_slave(uint32_t lba, uint8_t* buffer);
int  ata_read_sectors_slave(uint32_t lba, uint32_t count, uint8_t* buffer);

/* 从主通道 slave 设备写入 */
int  ata_write_sector_slave(uint32_t lba, const uint8_t* buffer);
int  ata_write_sectors_slave(uint32_t lba, uint32_t count, const uint8_t* buffer);

#endif /* ATA_H */