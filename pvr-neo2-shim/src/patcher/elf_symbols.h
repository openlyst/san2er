#ifndef ELF_SYMBOLS_H
#define ELF_SYMBOLS_H

#include <stddef.h>

#define MAX_SYMBOLS 1024
#define MAX_SYM_NAME 256

typedef struct {
    char name[MAX_SYM_NAME];
} elf_symbol_t;

typedef struct {
    elf_symbol_t symbols[MAX_SYMBOLS];
    int count;
} elf_symbol_list_t;

/* Parse an ELF .so file and extract exported FUNC symbols matching
   Pvr_, PVR_, Java_, JNI_ prefixes. Returns 0 on success. */
int elf_extract_pvr_symbols(const char *so_path, elf_symbol_list_t *out);

#endif
