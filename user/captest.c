/* Capability-teszt: a manifest szerint engedett es tiltott muveletek.
 * Elvart eredmeny az etc/agent-test.cap manifesttel:
 *   /tmp/project/src/a.txt irasa: OK
 *   /tmp/project/Makefile irasa: E_CAP
 *   /sys/version olvasasa: OK, /etc/motd olvasasa: E_CAP
 *   sysinfo: E_CAP (nincs sysinfo cap) */
#include "aolib.h"

static int try_write(const char *path, const char *text)
{
    int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) { ao_printf("  write %-26s -> %s\n", path, ao_errstr(fd)); return fd; }
    isize n = ao_write(fd, text, strlen(text));
    ao_close(fd);
    ao_printf("  write %-26s -> OK (%ld bajt)\n", path, (i64)n);
    return 0;
}

static int try_read(const char *path)
{
    int fd = ao_open(path, O_READ);
    if (fd < 0) { ao_printf("  read  %-26s -> %s\n", path, ao_errstr(fd)); return fd; }
    char buf[64];
    isize n = ao_read(fd, buf, sizeof buf - 1);
    ao_close(fd);
    if (n < 0) { ao_printf("  read  %-26s -> %s\n", path, ao_errstr((int)n)); return (int)n; }
    buf[n] = 0;
    for (isize i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = ' ';
    ao_printf("  read  %-26s -> OK \"%s\"\n", path, buf);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char caps[512];
    int n = ao_getcaps(caps, sizeof caps);
    if (n > 0) { ao_puts("capability-keszlet:\n"); ao_write(1, caps, (usize)n); }

    ao_mkdir("/tmp/project");
    ao_mkdir("/tmp/project/src");
    try_write("/tmp/project/src/a.txt", "agent irta\n");
    try_write("/tmp/project/Makefile", "tiltott\n");
    try_read("/tmp/project/src/a.txt");
    try_read("/sys/version");
    try_read("/etc/motd");
    struct sysinfo si;
    int e = ao_sysinfo(&si);
    ao_printf("  sysinfo                          -> %s\n", e ? ao_errstr(e) : "OK");
    return 0;
}
