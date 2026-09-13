/* fputest: lebegopontos szamitas ket parhuzamos taskban, taskvaltasokkal a kozepen.
 * Ha a kernel nem mentene az FPU/SSE allapotot, a ket task osszekeverne egymas ertekeit.
 *   run fputest        (a szulo elinditja a gyereket is) */
#include "aolib.h"

static double run(int seed, bool with_yields)
{
    double x = seed, y = 0.25 * seed, z = 1.0;
    for (int i = 0; i < 400; i++) {
        x = x * 1.0001 + y;
        z = z * 0.999 + x / 1000.0;
        y = y + z * 0.001;
        if (with_yields && (i & 3) == 0) ao_sleep(1);
    }
    return x + y + z;
}

static void print_double(double d)
{
    i64 ip = (i64)d;
    i64 frac = (i64)((d - (double)ip) * 1000.0);
    if (frac < 0) frac = -frac;
    ao_printf("%ld.%03ld", ip, frac);
}

int main(int argc, char **argv)
{
    (void)argv;
    bool child = argc > 1;
    int seed = child ? 7 : 3;
    int pid = -1;
    if (!child) {
        char *args[] = { "fputest", "gyerek", NULL };
        pid = ao_spawn("/bin/fputest.aox", args, NULL);
        if (pid < 0) { ao_printf("fputest: gyerek: %s\n", ao_errstr(pid)); }
    }
    double ref = run(seed, false);
    double got = run(seed, true);
    bool ok = ref == got;
    ao_printf("fputest%s: ", child ? " (gyerek)" : "");
    print_double(got);
    ao_printf(" %s\n", ok ? "= referencia" : "!= referencia HIBA");
    if (!child) {
        int st = 0;
        if (pid >= 0) ao_wait(pid, &st);
        ao_printf("fputest: %s\n", ok && st == 0 ? "ok" : "HIBA");
        return ok && st == 0 ? 0 : 1;
    }
    return ok ? 0 : 1;
}
