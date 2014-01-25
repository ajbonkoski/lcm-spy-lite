#include "symtab_elf.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <assert.h>

#define MAXBUF 256

typedef struct
{
    char *data;
    size_t datasz;
    size_t len;

} string_t;

static inline string_t *string_create(void)
{
    string_t *this = calloc(1, sizeof(string_t));
    this->datasz = 8;
    this->data = malloc(this->datasz * sizeof(char));
    this->data[0] = '\0';
    this->len = 0;

    return this;
}

static inline void string_append(string_t *this, char c)
{
    // do we need to reallocate?
    if(this->len+1 == this->datasz) {
        this->datasz *= 2;
        this->data = realloc(this->data, this->datasz);
    }

    this->data[this->len++] = c;
    this->data[this->len] = '\0';
}

static inline void string_clear(string_t *this)
{
    this->len = 0;
    this->data[this->len] = '\0';
}

static inline void string_destroy(string_t *this)
{
    free(this->data);
    free(this);
}

struct symtab_elf_iter
{
    FILE *f;

    char buffer[MAXBUF];
    int num_read;
    int index;

    string_t *s;
};

symtab_elf_iter_t *symtab_elf_iter_create(const char *libname)
{
    FILE *f = fopen(libname, "rb");
    if(f == NULL)
        return NULL;

    // successfully opened!
    symtab_elf_iter_t *this = calloc(1, sizeof(symtab_elf_iter_t));
    this->f = f;
    this->num_read = 0;
    this->index = 0;
    this->s = string_create();

    return this;
}

void symtab_elf_iter_destroy(symtab_elf_iter_t *this)
{
    if(this == NULL)
        return;

    fclose(this->f);
    string_destroy(this->s);
    free(this);
}


// returns success (0) or failure (1)
static inline int refill_buffer(symtab_elf_iter_t *this)
{
    assert(this->num_read == this->index);
    int n = fread(this->buffer, 1, MAXBUF, this->f);
    if(n == 0)
        return 1;

    this->num_read = n;
    this->index = 0;
    return 0;
}

static inline int is_ident_char(uint8_t ch)
{
    return isalpha(ch) || isdigit(ch) || ch == '_';
}

static inline int is_first_ident_char(uint8_t ch)
{
    return isalpha(ch) || ch == '_';
}

// a very crude symbol extraction method
// we simply search for byte sequences that look like valid
// C language identifiers
const char *symtab_elf_iter_get_next(symtab_elf_iter_t *this)
{
    if(this == NULL)
        return NULL;

    // cleanup the last string
    string_clear(this->s);

    while(1) {

        while(this->index < this->num_read) {
            uint8_t c = this->buffer[this->index++];

            // null byte, end of a string?
            if(c == '\0') {
                if(this->s->len > 0) {
                    return this->s->data;
                }
            }

            // is it an identifier char?
            else if(this->s->len > 0 && is_ident_char(c)) {
                string_append(this->s, c);
            }

            else if(this->s->len == 0 && is_first_ident_char(c)) {
                string_append(this->s, c);
            }

            // otherwise its garbage, reset the string
            else {
                string_clear(this->s);
            }
        }

        // we've exhausted the buffer, let's try to get more...
        int res = refill_buffer(this);
        if(res != 0) {
            // end-of-file or error
            return NULL;
        }

    }
}
