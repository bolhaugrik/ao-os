#include "aofs.h"
#include "../lib/string.h"

static const u8 *img;
static u32 img_size;
static const struct aofs_super *sb;
static const struct aofs_entry *tab;

bool aofs_mount(const void *image, u32 size)
{
    if (!image || size < 4096)
        return false;
    sb = image;
    if (sb->magic != AOFS_MAGIC || sb->version != 1)
        return false;
    if (4096 + (u64)sb->count * sizeof(struct aofs_entry) > size)
        return false;
    img = image;
    img_size = size;
    tab = (const struct aofs_entry *)(img + 4096);
    return true;
}

bool aofs_mounted(void) { return img != NULL; }
u32 aofs_count(void) { return img ? sb->count : 0; }
u32 aofs_image_size(void) { return img_size; }
const struct aofs_entry *aofs_entry(u32 i) { return (img && i < sb->count) ? &tab[i] : NULL; }

const struct aofs_entry *aofs_lookup(const char *path)
{
    if (!img)
        return NULL;
    while (*path == '/')
        path++;
    for (u32 i = 0; i < sb->count; i++)
        if (strcmp(tab[i].name, path) == 0)
            return &tab[i];
    return NULL;
}

const void *aofs_data(const struct aofs_entry *e)
{
    if (!e || e->offset + e->size > img_size)
        return NULL;
    return img + e->offset;
}
