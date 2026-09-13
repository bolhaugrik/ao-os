/* malloctest: a felhasznaloi foglalo probaja (mintak, felszabaditas, realloc, nagy blokk, korlat). */
#include "aolib.h"
#include "malloc.h"

static bool check(const u8 *p, usize n, u8 v)
{
    for (usize i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        /* korlat-proba: 6 MiB egy 4 MiB-os manifesttel NULL kell legyen, nem osszeomlas */
        void *p = malloc(6 * 1024 * 1024);
        ao_printf("malloctest: korlat %s\n", p ? "HIBA (a 6 MiB sikerult)" : "ok (6 MiB: NULL)");
        return p ? 1 : 0;
    }
    enum { N = 200 };
    static u8 *ptrs[N];
    static usize sizes[N];
    bool ok = true;
    for (int i = 0; i < N; i++) {
        sizes[i] = (usize)(i * 37 % 3000) + 1;
        ptrs[i] = malloc(sizes[i]);
        if (!ptrs[i]) { ao_printf("malloctest: malloc(%lu) NULL\n", (u64)sizes[i]); return 1; }
        memset(ptrs[i], (u8)i, sizes[i]);
    }
    for (int i = 0; i < N; i += 2) { free(ptrs[i]); ptrs[i] = NULL; }
    for (int i = 1; i < N; i += 2)
        if (!check(ptrs[i], sizes[i], (u8)i)) { ok = false; ao_printf("malloctest: %d. blokk serult\n", i); }
    for (int i = 0; i < N; i += 2) {
        ptrs[i] = malloc(sizes[i] * 2);
        if (!ptrs[i]) { ao_puts("malloctest: ujrafoglalas NULL\n"); return 1; }
        memset(ptrs[i], (u8)(i + 1), sizes[i] * 2);
    }
    u8 *r = realloc(ptrs[1], 50000);
    if (!r || !check(r, sizes[1], 1)) { ok = false; ao_puts("malloctest: realloc hiba\n"); }
    ptrs[1] = r;
    u8 *big = malloc(2 * 1024 * 1024);
    if (!big) { ok = false; ao_puts("malloctest: 2 MiB NULL\n"); }
    else { memset(big, 0x5A, 2 * 1024 * 1024); ok &= check(big + 1000000, 1000, 0x5A); free(big); }
    for (int i = 0; i < N; i++) free(ptrs[i]);
    /* korlat: hatalmas blokk -> NULL, nem osszeomlas */
    void *huge = malloc(3ULL * 1024 * 1024 * 1024);
    if (huge) { ok = false; ao_puts("malloctest: a 3 GiB nem lett volna szabad\n"); }
    ao_printf("malloctest: %s (heap %lu KiB, foglalt %lu B)\n", ok ? "ok" : "HIBA", (u64)malloc_heap() / 1024, (u64)malloc_used());
    return ok ? 0 : 1;
}
