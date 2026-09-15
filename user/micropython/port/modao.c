/* 'ao' modul: az AO-OS sajat rendszerhivasai Pythonbol (a fajlok az os modulon at mennek).
 *   ao.sysinfo()        dict: mem_total, mem_free, ticks, ntasks, pid, version, cols, rows
 *   ao.screen()         (oszlopok, sorok)
 *   ao.ticks_ms()       10 ms-es tick-bol
 *   ao.sleep_ms(ms)
 *   ao.getkey()         egy billentyu a konzolrol (str; a specialisak ESC-szekvenciak, pl. '\x1b[A')
 *   ao.con_mode(m)      ao.CON_TEXT / ao.CON_RAW [| ao.CON_NONBLOCK]
 *   ao.caps()           a program capability-listaja szovegkent
 *   ao.pid()
 */
#include "aolib.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"

static mp_obj_t ao_sysinfo_(void)
{
    struct sysinfo si;
    if (ao_sysinfo(&si) != 0) mp_raise_OSError(MP_EIO);
    mp_obj_t d = mp_obj_new_dict(8);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mem_total), mp_obj_new_int_from_ull(si.mem_total));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mem_free), mp_obj_new_int_from_ull(si.mem_free));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_ticks), mp_obj_new_int_from_ull(si.ticks));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_ntasks), MP_OBJ_NEW_SMALL_INT(si.ntasks));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_pid), MP_OBJ_NEW_SMALL_INT(si.pid));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_version), mp_obj_new_str_from_cstr(si.version));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_cols), MP_OBJ_NEW_SMALL_INT(si.con_cols));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rows), MP_OBJ_NEW_SMALL_INT(si.con_rows));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ao_sysinfo_obj, ao_sysinfo_);

static mp_obj_t ao_screen_(void)
{
    struct sysinfo si;
    if (ao_sysinfo(&si) != 0) mp_raise_OSError(MP_EIO);
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(si.con_cols), MP_OBJ_NEW_SMALL_INT(si.con_rows) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ao_screen_obj, ao_screen_);

static mp_obj_t ao_ticks_ms_(void) { return mp_obj_new_int_from_uint(mp_hal_ticks_ms()); }
static MP_DEFINE_CONST_FUN_OBJ_0(ao_ticks_ms_obj, ao_ticks_ms_);

static mp_obj_t ao_sleep_ms_(mp_obj_t ms)
{
    mp_int_t v = mp_obj_get_int(ms);
    if (v > 0) mp_hal_delay_ms((mp_uint_t)v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ao_sleep_ms_obj, ao_sleep_ms_);

static mp_obj_t ao_getkey_(void)
{
    char buf[8];
    isize n = ao_read(0, buf, sizeof buf);
    if (n < 0) mp_raise_OSError(MP_EIO);
    return mp_obj_new_str(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ao_getkey_obj, ao_getkey_);

static mp_obj_t ao_con_mode_(mp_obj_t mode)
{
    int e = ao_con_mode((u32)mp_obj_get_int(mode));
    if (e) mp_raise_OSError(MP_EACCES);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ao_con_mode_obj, ao_con_mode_);

static mp_obj_t ao_caps_(void)
{
    char buf[1024];
    int n = ao_getcaps(buf, sizeof buf);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf) n = sizeof buf;
    return mp_obj_new_str(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ao_caps_obj, ao_caps_);

static mp_obj_t ao_pid_(void) { return MP_OBJ_NEW_SMALL_INT(ao_getpid()); }
static MP_DEFINE_CONST_FUN_OBJ_0(ao_pid_obj, ao_pid_);

static const mp_rom_map_elem_t ao_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ao) },
    { MP_ROM_QSTR(MP_QSTR_sysinfo), MP_ROM_PTR(&ao_sysinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_screen), MP_ROM_PTR(&ao_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&ao_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&ao_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_getkey), MP_ROM_PTR(&ao_getkey_obj) },
    { MP_ROM_QSTR(MP_QSTR_con_mode), MP_ROM_PTR(&ao_con_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_caps), MP_ROM_PTR(&ao_caps_obj) },
    { MP_ROM_QSTR(MP_QSTR_pid), MP_ROM_PTR(&ao_pid_obj) },
    { MP_ROM_QSTR(MP_QSTR_CON_TEXT), MP_ROM_INT(CON_TEXT) },
    { MP_ROM_QSTR(MP_QSTR_CON_RAW), MP_ROM_INT(CON_RAW) },
    { MP_ROM_QSTR(MP_QSTR_CON_NONBLOCK), MP_ROM_INT(CON_NONBLOCK) },
};
static MP_DEFINE_CONST_DICT(ao_module_globals, ao_module_globals_table);

const mp_obj_module_t ao_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ao_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ao, ao_module);
