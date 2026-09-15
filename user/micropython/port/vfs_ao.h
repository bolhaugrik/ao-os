/* VfsAO: a MicroPython VFS-protokollja az AO-OS rendszerhivasain (open/read/write/seek/list/stat/...) */
#pragma once
#include "py/obj.h"

extern const mp_obj_type_t mp_type_vfs_ao;
extern const mp_obj_type_t mp_type_vfs_ao_fileio;
extern const mp_obj_type_t mp_type_vfs_ao_textio;

mp_obj_t mp_vfs_ao_file_open(const mp_obj_type_t *type, mp_obj_t path_in, mp_obj_t mode_in);
