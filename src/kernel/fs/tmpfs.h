#ifndef TMPFS_H
#define TMPFS_H

#include "vfs.h"

typedef struct tmpfs_node {
    char name[64];
    uint8_t is_dir;
    void* data;
    uint64_t size;
    struct tmpfs_node* children;
    struct tmpfs_node* next;
} tmpfs_node_t;

void    tmpfs_init(void);
int     tmpfs_create_file(const char* path, void* data, uint64_t size);
int     tmpfs_create_dir(const char* path);
file_t* tmpfs_open(const char* path, uint64_t flags);
int     tmpfs_list(const char* path, char* buf, int max);

#endif