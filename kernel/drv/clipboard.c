#include "clipboard.h"
#include "kbd.h"
#include "console.h"
#include "../arch/io.h"
#include "../shell/shell.h"
#include "../fs/vfs.h"
#include "../task/task.h"
#include "../cap/cap.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

#define CLIP_FILE "/tmp/clip.txt"
#define CLIP_MAX  (256 * 1024)

static struct waitq clip_q;
static volatile u32 pending, pending_shot;

void clipboard_request(void)
{
    pending++;
    waitq_wake_all(&clip_q);
}

void clipboard_request_shot(void)
{
    pending_shot++;
    waitq_wake_all(&clip_q);
}

/* a clip-agent futtatasa a clip.cap jogaival: agentd ARGV (mint a shell 'paste'/'shot' parancsa) */
static bool run_clip_agent(int argc, char **argv)
{
    void *text;
    usize len;
    if (vfs_read_all("/etc/agents/clip.cap", &text, &len)) { kprintf("\n[vagolap: nincs /etc/agents/clip.cap]\n"); return false; }
    struct capset cs;
    char err[64];
    bool ok = capset_parse(&cs, text, len, err, sizeof err);
    kfree(text);
    if (!ok) { kprintf("\n[vagolap: clip.cap: %s]\n", err); return false; }
    void *img;
    usize size;
    if (vfs_read_all("/bin/agentd.aox", &img, &size) && vfs_read_all("/rd/bin/agentd.aox", &img, &size)) {
        kprintf("\n[vagolap: nincs agentd]\n");
        return false;
    }
    int e;
    struct task *t = task_create_user("vagolap", img, size, argc, argv, &cs, task_current(), &e);
    kfree(img);
    if (!t) { kprintf("\n[vagolap: agent inditasa: hiba %d]\n", e); return false; }
    int status = 0;
    task_wait(t->id, &status);
    return status == 0;
}

/* Ctrl+F12: a lathato kepernyo szovege -> /tmp/shot.txt -> a PC shots/ mappaja (a shell 'shot'-ja szerint) */
static void text_shot(void)
{
    u32 rows = console_rows(), cols = console_cols();
    usize cap = (usize)rows * (cols * 3 + 2) + 1;
    char *buf = kmalloc(cap);
    if (!buf) return;
    usize n = 0;
    for (u32 r = 0; r < rows; r++) {
        console_get_screen_line(r, buf + n, cap - n - 2);
        n += strlen(buf + n);
        buf[n++] = '\n';
    }
    int e = vfs_write_all("/tmp/shot.txt", buf, n);
    kfree(buf);
    if (e) { kprintf("\n[shot: /tmp/shot.txt: hiba %d]\n", e); return; }
    char *argv[] = { "agentd", "--file", "shot", "shot", "/tmp/shot.txt" };
    run_clip_agent(5, argv);
}

static void fetch_and_inject(void)
{
    vfs_unlink(CLIP_FILE);
    char *argv[] = { "agentd", "--clip", CLIP_FILE };
    bool ok = run_clip_agent(3, argv);
    void *b;
    usize n;
    if (vfs_read_all(CLIP_FILE, &b, &n)) {
        if (ok) kprintf("\n[vagolap: ures]\n");
        else { kprintf("\n[vagolap: nem jott tartalom (fut a hid?)]\n"); console_flush(); }
        return;
    }
    if (n > CLIP_MAX) n = CLIP_MAX;
    kbd_inject(b, n);
    kfree(b);
}

static void clip_thread(void *arg)
{
    (void)arg;
    for (;;) {
        cli();
        while (!pending && !pending_shot) {
            task_block_on(&clip_q);     /* IF=1-gyel ter vissza */
            cli();
        }
        bool do_paste = pending != 0, do_shot = pending_shot != 0;
        pending = pending_shot = 0;
        sti();
        if (do_shot) text_shot();
        if (do_paste) fetch_and_inject();
    }
}

void clipboard_init(void)
{
    task_create_kernel("clip", clip_thread, NULL);
}
