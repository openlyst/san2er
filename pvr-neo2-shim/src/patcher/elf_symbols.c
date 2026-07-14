#include "elf_symbols.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_interesting_prefix(const char *name) {
    if (!strncmp(name, "Pvr_", 4)) return 1;
    if (!strncmp(name, "PVR_", 4)) return 1;
    if (!strncmp(name, "Java_", 5)) return 1;
    if (!strncmp(name, "JNI_", 4)) return 1;
    return 0;
}

static void add_symbol(elf_symbol_list_t *list, const char *name) {
    if (list->count >= MAX_SYMBOLS) return;
    if (strlen(name) >= MAX_SYM_NAME) return;
    /* dedup */
    for (int i = 0; i < list->count; i++) {
        if (!strcmp(list->symbols[i].name, name)) return;
    }
    strncpy(list->symbols[list->count].name, name, MAX_SYM_NAME - 1);
    list->symbols[list->count].name[MAX_SYM_NAME - 1] = '\0';
    list->count++;
}

int elf_extract_pvr_symbols(const char *so_path, elf_symbol_list_t *out) {
    int fd = open(so_path, O_RDONLY);
    if (fd < 0) {
        perror("open .so");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    out->count = 0;

    /* verify ELF64 header */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)map;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "not an ELF file\n");
        munmap(map, st.st_size);
        return -1;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "not ELF64\n");
        munmap(map, st.st_size);
        return -1;
    }

    /* find .dynsym and .dynstr via section headers */
    Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
    Elf64_Shdr *dynsym = NULL, *dynstr = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM) {
            dynsym = &shdrs[i];
            dynstr = &shdrs[dynsym->sh_link];
            break;
        }
    }

    if (!dynsym || !dynstr) {
        fprintf(stderr, "no .dynsym section found\n");
        munmap(map, st.st_size);
        return -1;
    }

    Elf64_Sym *syms = (Elf64_Sym *)((char *)map + dynsym->sh_offset);
    const char *strtab = (const char *)map + dynstr->sh_offset;
    int nsyms = dynsym->sh_size / sizeof(Elf64_Sym);

    for (int i = 0; i < nsyms; i++) {
        if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC) continue;
        if (ELF64_ST_BIND(syms[i].st_info) != STB_GLOBAL) continue;
        if (syms[i].st_shndx == SHN_UNDEF) continue;

        const char *name = strtab + syms[i].st_name;
        if (!name[0]) continue;
        if (is_interesting_prefix(name)) {
            add_symbol(out, name);
        }
    }

    munmap(map, st.st_size);
    return 0;
}
