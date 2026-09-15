/* MicroPython AO-OS port: konzol be/ki, ido, alvas a rendszerhivasainkkal.
 * A konzol szoveges modban van: a kernel alkalmazza a kiosztast, a specialis billentyuk ESC-szekvenciak,
 * az Enter '\n' (a readline '\r'-t var, ezert forditjuk), a Ctrl+D (4) a REPL vege. */
#include "aolib.h"
#include "py/mphal.h"
#include "py/runtime.h"

static u8 inq[16];
static int inq_n, inq_p;
static u64 tsc_hz;

void mp_hal_init(void)
{
    struct sysinfo si;
    if (ao_sysinfo(&si) == 0 && si.tsc_hz) tsc_hz = si.tsc_hz;
    else tsc_hz = 1000000000ULL;
}

int mp_hal_stdin_rx_chr(void)
{
    if (inq_p >= inq_n) {
        isize n = ao_read(0, inq, sizeof inq);
        if (n <= 0) return 4;                   /* leallitva (Ctrl+C a shellbol): mint a Ctrl+D */
        inq_n = (int)n;
        inq_p = 0;
    }
    u8 c = inq[inq_p++];
    return c == '\n' ? '\r' : c;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    if (len) ao_write(1, str, len);
    return len;
}

void mp_hal_delay_ms(mp_uint_t ms) { ao_sleep((u32)ms); }

void mp_hal_delay_us(mp_uint_t us)
{
    if (us >= 1000) { ao_sleep((u32)(us / 1000)); return; }
    u64 end = mp_hal_ticks_cpu() + us * (tsc_hz / 1000000);
    while (mp_hal_ticks_cpu() < end) ao_yield();
}

mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)(ao_ticks() * 10); }

mp_uint_t mp_hal_ticks_us(void) { return (mp_uint_t)(mp_hal_ticks_cpu() / (tsc_hz / 1000000)); }

uint64_t mp_hal_time_ns(void) { return (uint64_t)mp_hal_ticks_us() * 1000; }
