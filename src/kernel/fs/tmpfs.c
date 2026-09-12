#include "tmpfs.h"
#include "vga.h"

static tmpfs_node_t* root = (tmpfs_node_t*)0;

/* Helper: find or create a node by path segments */
static tmpfs_node_t* tmpfs_lookup(const char* path, int create_dir)
{
    if (!path || path[0] != '/') return (tmpfs_node_t*)0;

    if (!root) {
        if (!create_dir) return (tmpfs_node_t*)0;
        root = (tmpfs_node_t*)0;
        /* Will be created below */
    }

    /* If path is just "/", return root */
    if (path[1] == '\0') {
        if (!root && create_dir) {
            /* Allocate root in BSS */
            static tmpfs_node_t root_node;
            root_node.is_dir = 1;
            root_node.children = (tmpfs_node_t*)0;
            root_node.next = (tmpfs_node_t*)0;
            root_node.size = 0;
            root_node.data = (void*)0;
            root_node.name[0] = '/';
            root_node.name[1] = '\0';
            root = &root_node;
        }
        return root;
    }

    /* Create root if needed */
    if (!root) {
        static tmpfs_node_t root_node;
        root_node.is_dir = 1;
        root_node.children = (tmpfs_node_t*)0;
        root_node.next = (tmpfs_node_t*)0;
        root_node.size = 0;
        root_node.data = (void*)0;
        root_node.name[0] = '/';
        root_node.name[1] = '\0';
        root = &root_node;
    }

    /* Parse path segments */
    const char* p = path + 1;  /* Skip leading '/' */
    tmpfs_node_t* cur = root;

    while (*p) {
        /* Extract segment name */
        char seg[64];
        int i = 0;
        while (*p && *p != '/' && i < 63) {
            seg[i++] = *p++;
        }
        seg[i] = '\0';
        if (*p == '/') p++;

        if (i == 0) continue;  /* Skip empty segments */

        /* Search children */
        tmpfs_node_t* child = cur->children;
        tmpfs_node_t* found = (tmpfs_node_t*)0;
        while (child) {
            int match = 1;
            for (int j = 0; seg[j] || child->name[j]; j++) {
                if (seg[j] != child->name[j]) { match = 0; break; }
            }
            if (match) { found = child; break; }
            child = child->next;
        }

        if (found) {
            cur = found;
        } else if (create_dir) {
            /* Create new node */
            static tmpfs_node_t pool[64];
            static int pool_idx = 0;
            if (pool_idx >= 64) return (tmpfs_node_t*)0;

            tmpfs_node_t* node = &pool[pool_idx++];
            node->is_dir = create_dir ? 1 : 0;
            node->children = (tmpfs_node_t*)0;
            node->next = (tmpfs_node_t*)0;
            node->size = 0;
            node->data = (void*)0;
            for (int j = 0; j < 64; j++) node->name[j] = 0;
            for (int j = 0; j < i; j++) node->name[j] = seg[j];

            /* Append to parent's children list */
            node->next = cur->children;
            cur->children = node;
            cur = node;
        } else {
            return (tmpfs_node_t*)0;  /* Not found */
        }
    }

    return cur;
}

int tmpfs_create_file(const char* path, void* data, uint64_t size)
{
    tmpfs_node_t* node = tmpfs_lookup(path, 1);
    if (!node) return -1;

    node->is_dir = 0;
    node->data = data;
    node->size = size;
    return 0;
}

int tmpfs_create_dir(const char* path)
{
    tmpfs_node_t* node = tmpfs_lookup(path, 1);
    if (!node) return -1;

    node->is_dir = 1;
    return 0;
}

/* Tmpfs file operations */
static uint64_t tmpfs_read_op(void* f, uint64_t off, void* buf, uint64_t size)
{
    file_t* file = (file_t*)f;
    tmpfs_node_t* node = (tmpfs_node_t*)file->private_data;
    if (!node || !node->data) return 0;

    if (off >= node->size) return 0;
    uint64_t remain = node->size - off;
    if (size > remain) size = remain;

    uint8_t* src = (uint8_t*)node->data + off;
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    return size;
}

static uint64_t tmpfs_write_op(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf;
    return size;  /* tmpfs files are read-only for now */
}

static uint64_t tmpfs_close_op(void* f)
{
    file_t* file = (file_t*)f;
    file->ref_count--;
    return 0;
}

static file_ops_t tmpfs_ops;

void tmpfs_init(void)
{
    root = (tmpfs_node_t*)0;
    tmpfs_ops.read = tmpfs_read_op;
    tmpfs_ops.write = tmpfs_write_op;
    tmpfs_ops.open = 0;
    tmpfs_ops.close = tmpfs_close_op;
    tmpfs_ops.ioctl = 0;
    tmpfs_ops.lseek = 0;
}

file_t* tmpfs_open(const char* path, uint64_t flags)
{
    (void)flags;

    tmpfs_node_t* node = tmpfs_lookup(path, 0);
    if (!node) {
        return (file_t*)0;   /* W7: ENOENT 静默，不再打印噪音 */
    }
    if (node->is_dir) {
        return (file_t*)0;
    }

    /* Allocate file descriptor (static pool) */
    static file_t file_pool[16];
    static int file_pool_idx = 0;
    if (file_pool_idx >= 16) return (file_t*)0;

    file_t* f = &file_pool[file_pool_idx++];
    f->inode = (uint64_t)(uintptr_t)node;
    f->size = node->size;
    f->offset = 0;
    f->ops = &tmpfs_ops;
    f->private_data = node;
    f->ref_count = 1;
    for (int i = 0; i < 64; i++) f->name[i] = 0;
    for (int i = 0; path[i] && i < 63; i++) f->name[i] = path[i];

    /* Ensure ops are properly set (workaround for static init issue) */
    if (!f->ops || !f->ops->read) {
        vga_puts("[tmpfs] ops check failed!\n");
        return (file_t*)0;
    }

    return f;
}

int tmpfs_list(const char* path, char* buf, int max)
{
    (void)max;
    tmpfs_node_t* node = tmpfs_lookup(path, 0);
    if (!node || !node->is_dir) return -1;

    int pos = 0;
    tmpfs_node_t* child = node->children;
    while (child && pos < max - 32) {
        for (int i = 0; child->name[i] && pos < max - 1; i++) {
            buf[pos++] = child->name[i];
        }
        buf[pos++] = child->is_dir ? '/' : ' ';
        buf[pos++] = ' ';
        child = child->next;
    }
    if (pos < max) buf[pos] = '\0';
    return pos;
}