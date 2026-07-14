#ifndef STUB_GEN_H
#define STUB_GEN_H

#include "elf_symbols.h"

/* Generate forward_stubs.S and forward_vars.cpp from the symbol list.
   hook_names is a NULL-terminated array of function names that have
   hand-written hooks and should be skipped in the generated stubs.
   Returns 0 on success. */
int stub_gen_generate(const elf_symbol_list_t *symbols,
                      const char **hook_names,
                      const char *stubs_path,
                      const char *vars_path);

#endif
