#include "lcmtype_db.h"
#include "symtab_elf.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <dlfcn.h>
#include <glib.h>

#define MAXBUFSZ 256

static int DEBUG = 0;

typedef struct
{
    char *paths;
    char *next;

} path_iter_t;

static path_iter_t *path_iter_create(const char *paths)
{
    path_iter_t *this = calloc(1, sizeof(path_iter_t));
    this->paths = strdup(paths);
    this->next = this->paths;
    return this;
}

const char *path_iter_next(path_iter_t *this)
{
    const char *ret = this->next;
    if(ret == NULL)
        return NULL;

    char *ptr = this->next;
    for(; *ptr && *ptr != ':'; ++ptr);

    if(*ptr == ':') {
        *ptr = '\0';
        this->next = ptr + 1;
    } else {
        this->next = NULL;
    }

    return ret;
}

static void path_iter_destroy(path_iter_t *this)
{
    free(this->paths);
    free(this);
}

struct lcmtype_db
{
    void *lib;
    GHashTable *hash_to_type;
    GHashTable *name_to_hash;
};

void lcmtype_metadata_destroy(lcmtype_metadata_t *md)
{
    free(md->typename);
    free(md);
}

static void *open_lib(const char *libname)
{
    void *lib = NULL;

    // verify the .so library
    int len = strlen(libname);
    if(len < 3 || strcmp(libname+len-3, ".so")) {
        fprintf(stderr, "ERR: bad library name, expected a .so file, not '%s'\n", libname);
        goto fail;
    }

    // attempt to open the .so
    lib = dlopen(libname, RTLD_LAZY);
    if(lib == NULL) {
        fprintf(stderr, "ERR: failed to open '%s'\n", libname);
        fprintf(stderr, "ERR: %s\n", dlerror());
        goto fail;
    }

    return lib;

 fail:
    if(lib != NULL) dlclose(lib);
    return NULL;
}

static const char *lcmtype_functions[] =
{
    "_t_copy",
    "_t_decode",
    "_t_decode_cleanup",
    "_t_destroy",
    "_t_encode",
    "_t_encoded_size",
    "_t_get_field",
    "_t_get_type_info",
    "_t_num_fields",
    "_t_publish",
    "_t_struct_size",
    "_t_subscribe",
    "_t_subscription_set_queue_capacity",
    "_t_unsubscribe"
};

static void print_missing_methods(int mask)
{
    printf("  Missing methods:\n");

    int count = sizeof(lcmtype_functions) / sizeof(const char *);
    for(int i = 0; i < count; i++) {
        if(!(mask&(1<<i))) {
            printf("    %s\n", lcmtype_functions[i]);
        }
    }
}

// find all lcm types by post-processing the symbols
// extracted from the ELF file's symbol table
// caller is responsible for free'ing the array and its strings
static char **find_all_typenames(const char *libname)
{
    // read the library's symbol table
    symtab_elf_iter_t *stbl = symtab_elf_iter_create(libname);
    if(stbl == NULL) {
        fprintf(stderr, "ERR: failed to load symbol table for ELF file\n");
        return NULL;
    }

    // cache the string lengths
    int num_required_func = sizeof(lcmtype_functions) / sizeof(const char *);
    size_t *lcmtype_functions_sz = malloc(num_required_func * sizeof(size_t));
    for(int i = 0; i < num_required_func; i++)
        lcmtype_functions_sz[i] = strlen(lcmtype_functions[i]);

    // TODO use a hashtable instead of a O(n) search time unordered array
    size_t alloc = 4;
    size_t used = 0;
    char **names = malloc(alloc * sizeof(char *));
    int *masks = malloc(alloc * sizeof(int));

    // process the symbols
    while(1) {
        const char *s = symtab_elf_iter_get_next(stbl);
        if(s == NULL)
            break;

        //printf("Symbol: '%s'\n", s);

        size_t len = strlen(s);

        for(int i = 0; i < num_required_func; i++) {
            const char *f = lcmtype_functions[i];
            size_t flen = lcmtype_functions_sz[i];

            if(flen > len)
                continue;

            // did we find a potential lcmtype?
            if(strcmp(s + (len-flen), f) == 0) {

                // construct the typename
                char *typename = strdup(s);
                typename[len-flen+2] = '\0';
                if(DEBUG) printf("found potential typename='%s'\n", typename);

                // have we seen this candidate before?
                // TODO use a hashtable here
                int j = 0;
                for(; j < used; j++)
                    if(strcmp(typename, names[j]) == 0)
                        break;

                // not found?
                if(j == used) {

                    // need to realloc ?
                    if(used+1 >= alloc) {
                        alloc *= 2;
                        names = realloc(names, alloc * sizeof(char *));
                        masks = realloc(masks, alloc * sizeof(int));
                    }

                    // initialize new candidate
                    names[used] = typename;
                    masks[used] = 0;
                    used++;
                }

                // we've added this before, cleanup the string copy
                else {
                    free(typename);
                }

                // set this mask
                masks[j] |= 1<<i;

                break;
            }
        }
    }

    // prune the names array using the masks
    int valid_mask = (1 << num_required_func) - 1;
    int j = 0;
    for(int i = 0; i < used; i++) {
        if(masks[i] == valid_mask) {
            if(DEBUG) printf("verified new lcmtype: %s\n", names[i]);
            names[j++] = names[i];
        } else {
            if(DEBUG) printf("rejecting type '%s' with mask 0x%x\n", names[i], masks[i]);
            if(DEBUG) print_missing_methods(masks[i]);
        }
    }

    // add NULL sentinel
    names[j] = NULL;

    // cleanup
    free(lcmtype_functions_sz);
    free(masks);

    // user must free the names array and its strings
    return names;
}

static int load_types(const char *libname, void *lib,
                      GHashTable *hash_to_type, GHashTable *name_to_hash)
{
    char **names = find_all_typenames(libname);
    if(names == NULL) {
        fprintf(stderr, "ERR: failed to find lcm typenames in %s\n", libname);
        return 1;
    }

    // fetch each lcmtype_methods_t*, compute each hash, and add to hashtable
    int count = 0;
    for(char **ptr = names; *ptr; ptr++) {
        if(DEBUG) printf("Attempting load for type %s\n", *ptr);

        char funcname[MAXBUFSZ];
        int n = snprintf(funcname, MAXBUFSZ, "%s_get_type_info", *ptr);
        if(n == MAXBUFSZ) {
            fprintf(stderr, "ERR: get_type_info function name too long for %s\n", *ptr);
            continue;
        }

        lcm_type_info_t *(*get_type_info)(void) = NULL;
        *(void **) &get_type_info = dlsym(lib, funcname);
        if(get_type_info == NULL) {
            fprintf(stderr, "ERR: failed to load %s\n", funcname);
            continue;
        }

        lcm_type_info_t *typeinfo = get_type_info();
        int64_t msghash = typeinfo->get_hash();
        lcmtype_metadata_t *metadata = malloc(sizeof(lcmtype_metadata_t));
        metadata->hash = msghash;
        metadata->typename = *ptr; /* metadata->typename now "owns" the string */
        metadata->typeinfo = typeinfo;

        g_hash_table_insert(hash_to_type, &metadata->hash, metadata);
        g_hash_table_insert(name_to_hash, metadata->typename, &metadata->hash);

        if(DEBUG) printf("Success loading type %s (0x%"PRIx64")\n", *ptr, msghash);
        count++;
    }

    if(DEBUG) printf("Loaded %d lcmtypes from %s\n", count, libname);

    // cleanup
    free(names);

    return 0;
}

void destroy_types(GHashTable *types)
{
    g_hash_table_destroy(types);
}

lcmtype_db_t *lcmtype_db_create(const char *paths, int debug)
{
    /* TODO: put this in the lcmtype_db_t struct */
    DEBUG = debug;

    lcmtype_db_t *this = calloc(1, sizeof(lcmtype_db_t));

    this->hash_to_type = g_hash_table_new_full(
           g_int64_hash, g_int64_equal,
           NULL, (GDestroyNotify) lcmtype_metadata_destroy);

    this->name_to_hash = g_hash_table_new_full(
           g_str_hash, g_str_equal,
           NULL, NULL);

    path_iter_t *pi = path_iter_create(paths);

    const char *libname;
    while((libname=path_iter_next(pi))) {
        if(DEBUG) printf("Loading types from '%s'\n", libname);
        void *lib = open_lib(libname);
        if(lib == NULL) {
            fprintf(stderr, "Err: failed to open '%s'\n", libname);
            continue;
        }
        int ret = load_types(libname, lib,
                             this->hash_to_type,
                             this->name_to_hash);
        if(ret != 0) {
            fprintf(stderr, "Err: failed to load types from '%s'\n", libname);
            continue;
        }
    }

    path_iter_destroy(pi);
    return this;
}

void lcmtype_db_destroy(lcmtype_db_t *this)
{
    if(this == NULL)
        return;

    destroy_types(this->hash_to_type);
    destroy_types(this->name_to_hash);
}


const lcmtype_metadata_t *lcmtype_db_get_using_hash(lcmtype_db_t *this, int64_t hash)
{
    return g_hash_table_lookup(this->hash_to_type, &hash);
}

const lcmtype_metadata_t *lcmtype_db_get_using_name(lcmtype_db_t *this, const char *name)
{
    int64_t *hash = g_hash_table_lookup(this->name_to_hash, name);
    if(hash == NULL)
        return NULL;
    return g_hash_table_lookup(this->hash_to_type, hash);
}
