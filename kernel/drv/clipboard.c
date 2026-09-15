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
static volatile u32 pending;

void clipboard_request(void)
{
    pending++;
    waitq_wake_all(&clip_q);
}

/* a clip-agent futtatasa a clip.cap jogaival: agentd --clip /tmp/clip.txt (mint a shell 'paste'-je) */
static bool run_clip_agent(void)
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
    char *argv[] = { "agentd", "--clip", CLIP_FILE };
    int e;
    struct task *t = task_create_user("vagolap", img, size, 3, argv, &cs, task_current(), &e);
    kfree(img);
    if (!t) { kprintf("\n[vagolap: agent inditasa: hiba %d]\n", e); return false; }
    int status = 0;
    task_wait(t->id, &status);
    return status == 0;
}

static void fetch_and_inject(void)
{
    vfs_unlink(CLIP_FILE);
    bool ok = run_clip_agent();
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
        while (!pending) {
            task_block_on(&clip_q);     /* IF=1-gyel ter vissza */
            cli();
        }
        pending = 0;
        sti();
        fetch_and_inject();
    }
}

void clipboard_init(void)
{
    task_create_kernel("clip", clip_thread, NULL);
}
