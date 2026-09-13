#include "screenshot.h"
#include "fb.h"
#include "../arch/io.h"
#include "console.h"
#include "../fs/vfs.h"
#include "../task/task.h"
#include "../shell/shell.h"
#include "../lib/fmt.h"
#include "../lib/string.h"

static struct waitq shot_q;
static volatile u32 pending;
static u32 counter;

void screenshot_request(void)
{
    pending++;
    waitq_wake_all(&shot_q);
}

static bool write_all(struct handle *h, const void *buf, usize n)
{
    const u8 *p = buf;
    usize done = 0;
    while (done < n) {
        isize w = vfs_write(h, p + done, n - done);
        if (w <= 0) return false;
        done += (usize)w;
    }
    return true;
}

static void take(void)
{
    if (!fb.base || fb.bpp != 32) return;
    vfs_mkdir("/state/shots");
    char path[64];
    struct stat st;
    do {
        counter++;
        snformat(path, sizeof path, "/state/shots/%u.ppm", counter);
    } while (vfs_stat(path, &st) == 0 && counter < 10000);
    struct handle h;
    int e = vfs_open(path, O_WRITE | O_CREATE | O_TRUNC, &h);
    if (e) { kprintf("\n[kepernyokep: %s: hiba %d]\n", path, e); return; }
    char hdr[32];
    usize hl = snformat(hdr, sizeof hdr, "P6\n%u %u\n255\n", fb.width, fb.height);
    bool ok = write_all(&h, hdr, hl);
    static u8 row[4096 * 3];
    for (u32 y = 0; y < fb.height && ok; y++) {
        const u32 *px = (const u32 *)(fb.base + (u64)y * fb.pitch);
        for (u32 x = 0; x < fb.width; x++) {
            u32 v = px[x];
            row[x * 3] = (u8)(v >> fb.rpos);
            row[x * 3 + 1] = (u8)(v >> fb.gpos);
            row[x * 3 + 2] = (u8)(v >> fb.bpos);
        }
        ok = write_all(&h, row, (usize)fb.width * 3);
    }
    vfs_close(&h);
    vfs_sync();
    kprintf("\n[kepernyokep: %s%s]\n", path, ok ? "" : " (hianyos)");
    console_flush();
}

static void shot_thread(void *arg)
{
    (void)arg;
    for (;;) {
        cli();
        while (!pending) {
            task_block_on(&shot_q);     /* IF=1-gyel ter vissza */
            cli();
        }
        pending = 0;
        sti();
        take();
    }
}

void screenshot_init(void)
{
    task_create_kernel("shot", shot_thread, NULL);
}
