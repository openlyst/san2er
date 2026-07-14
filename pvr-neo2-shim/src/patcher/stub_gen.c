#include "stub_gen.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>

static int is_hook(const char *name, const char **hooks) {
    if (!hooks) return 0;
    for (int i = 0; hooks[i]; i++) {
        if (!strcmp(name, hooks[i])) return 1;
    }
    return 0;
}

int stub_gen_generate(const elf_symbol_list_t *symbols,
                      const char **hook_names,
                      const char *stubs_path,
                      const char *vars_path)
{
    /* ensure output directory exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", stubs_path);
    char *d = dirname(dir);
    mkdir(d, 0755);

    FILE *f = fopen(stubs_path, "w");
    if (!f) {
        perror("fopen stubs");
        return -1;
    }
    fprintf(f, "/* Auto-generated forward stubs. Do not edit by hand. */\n");
    fprintf(f, "    .text\n");
    for (int i = 0; i < symbols->count; i++) {
        const char *name = symbols->symbols[i].name;
        if (is_hook(name, hook_names)) continue;
        fprintf(f, "    .global %s\n", name);
        fprintf(f, "    .type %s, %%function\n", name);
        fprintf(f, "%s:\n", name);
        fprintf(f, "    adrp x16, :got:%s_orig\n", name);
        fprintf(f, "    ldr x16, [x16, :got_lo12:%s_orig]\n", name);
        fprintf(f, "    ldr x16, [x16]\n");
        fprintf(f, "    br x16\n\n");
    }
    fclose(f);

    f = fopen(vars_path, "w");
    if (!f) {
        perror("fopen vars");
        return -1;
    }
    fprintf(f, "/* Auto-generated forward symbol resolver. Do not edit by hand. */\n");
    fprintf(f, "#include <cstddef>\n");
    fprintf(f, "#include \"loader.h\"\n");
    fprintf(f, "#include \"log.h\"\n\n");
    fprintf(f, "extern \"C\" {\n");
    for (int i = 0; i < symbols->count; i++) {
        const char *name = symbols->symbols[i].name;
        if (is_hook(name, hook_names)) continue;
        fprintf(f, "    void* %s_orig = nullptr;\n", name);
    }
    fprintf(f, "}\n\n");
    fprintf(f, "namespace pvr_shim {\n\n");
    fprintf(f, "struct Symbol { const char* name; void** ptr; };\n");
    fprintf(f, "static const Symbol kSymbols[] = {\n");
    int count = 0;
    for (int i = 0; i < symbols->count; i++) {
        const char *name = symbols->symbols[i].name;
        if (is_hook(name, hook_names)) continue;
        fprintf(f, "    {\"%s\", &%s_orig},\n", name, name);
        count++;
    }
    fprintf(f, "};\n\n");
    fprintf(f, "void pvr_shim_resolve_forward_symbols() {\n");
    fprintf(f, "    for (const auto& s : kSymbols) {\n");
    fprintf(f, "        *s.ptr = resolve_original(s.name);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    LOGI(\"resolved %%zu forward symbols\", sizeof(kSymbols)/sizeof(kSymbols[0]));\n");
    fprintf(f, "}\n\n");
    fprintf(f, "} // namespace pvr_shim\n");
    fclose(f);

    return 0;
}
