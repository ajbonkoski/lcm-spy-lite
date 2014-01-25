#ifndef SYMTAB_ELF_H
#define SYMTAB_ELF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct symtab_elf_iter symtab_elf_iter_t;

symtab_elf_iter_t *symtab_elf_iter_create(const char *libname);
void symtab_elf_iter_destroy(symtab_elf_iter_t *this);
const char *symtab_elf_iter_get_next(symtab_elf_iter_t *this);

#ifdef __cplusplus
}
#endif

#endif  /* SYMTAB_ELF_H */
