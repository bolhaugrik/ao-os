/* MicroPython az AO-OS-en: python [FAJL [ARG..]] | python -c KOD | python (REPL, Ctrl+D kilep).
 * A heap a sajat malloc-bol jon (a manifest mem-korlatjan belul), a GC a vermet es a regisztereket
 * is bejarja; a fajlrendszer a VfsAO a gyokeren; sys.path: "", /state/lib, /project. */
#include "aolib.h"
#include "malloc.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mphal.h"
#include "shared/runtime/pyexec.h"
#include "shared/runtime/gchelper.h"
#include "shared/readline/readline.h"
#include "extmod/vfs.h"
#include "vfs_ao.h"

#define HEAP_SIZE   (16u * 1024 * 1024)
#define STACK_SIZE  (1024u * 1024)          /* = AOX_STACK a crt0-ban (tools/mpbuild.py) */

const char ao_help_text[] =
    "MicroPython az AO-OS-en.\n"
    "  help('modules')   beepitett modulok        import ao   az AO-OS sajat hivasai\n"
    "  Ctrl+D            kilepes a REPL-bol       Ctrl+E      tobbsoros beillesztes\n"
    "  python FAJL       szkript futtatasa        python -c KOD\n"
    "Modulok fajlbol: /state/lib es /project (sys.path). Fajlok: open(), os.listdir() stb.\n";

int main(int argc, char **argv)
{
    volatile int stack_dummy;
    mp_stack_set_top((void *)&stack_dummy);
    mp_stack_set_limit(STACK_SIZE - 64 * 1024);
    mp_hal_init();

    void *heap = malloc(HEAP_SIZE);
    if (!heap) { ao_puts("python: nincs memoria a heapnek (manifest mem?)\n"); return 1; }
    gc_init(heap, (u8 *)heap + HEAP_SIZE);
    mp_init();

    /* a gyoker a VfsAO */
    {
        mp_obj_t args[2] = {
            MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_ao, make_new)(&mp_type_vfs_ao, 0, 0, NULL),
            MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
        };
        mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
    }
    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_state_slash_lib));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_project));
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);
    readline_init0();

    int ret = 0;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        /* a shell szokozoknel darabol, idezojel nelkul: a darabokat visszaragasztjuk */
        vstr_t src;
        vstr_init(&src, 256);
        for (int i = 2; i < argc; i++) {
            if (i > 2) vstr_add_char(&src, ' ');
            vstr_add_str(&src, argv[i]);
        }
        if (src.len >= 2 && src.buf[0] == '"' && src.buf[src.len - 1] == '"') {
            src.buf[src.len - 1] = 0;
            vstr_t inner;
            vstr_init(&inner, src.len);
            vstr_add_str(&inner, src.buf + 1);
            vstr_clear(&src);
            src = inner;
        }
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR__dash_c));
        ret = pyexec_vstr(&src, true) ? 0 : 1;
    } else if (argc >= 2) {
        for (int i = 1; i < argc; i++) mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr(argv[i]));
        struct stat st;
        if (ao_stat(argv[1], &st) != 0) { ao_printf("python: nincs ilyen fajl: %s\n", argv[1]); ret = 2; }
        else ret = pyexec_file(argv[1]) ? 0 : 1;
    } else {
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
        ret = pyexec_friendly_repl();
        ret = ret & PYEXEC_FORCED_EXIT ? (ret & 0xFF) : 0;
        mp_hal_stdout_tx_str("\n");
    }
    mp_deinit();
    return ret;
}

void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val)
{
    (void)val;
    ao_puts("python: kezeletlen kivetel a futtatokornyezetben\n");
    ao_exit(3);
}

MP_NORETURN void __fatal_error(const char *msg)
{
    ao_printf("python: vegzetes hiba: %s\n", msg);
    ao_exit(3);
}
