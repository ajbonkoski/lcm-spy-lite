#include "msg_display.h"

#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>

static inline int is_ascii(int8_t c)
{
    return (32 <= c && c <= 126);
}
static void print_value_scalar(lcmtype_db_t *db, lcm_field_t *field, void *data, int *usertype_count)
{

    switch(field->type) {

        case LCM_FIELD_BYTE:
        case LCM_FIELD_INT8_T: {
            int8_t i = *(int8_t *) data;
            printf(" %d", i);
            if(is_ascii(i))
                printf(" (%c)", i);
            break;
        }

        case LCM_FIELD_INT16_T:
            printf("% d", *(int16_t *) data);
            break;

        case LCM_FIELD_INT32_T:
            printf("% d", *(int32_t *) data);
            break;

        case LCM_FIELD_INT64_T:
            printf("% " PRIi64 "", *(int64_t *) data);
            break;

        case LCM_FIELD_FLOAT:
            printf("% f", *(float *) data);
            break;

        case LCM_FIELD_DOUBLE:
            printf("% f", *(double *) data);
            break;

        case LCM_FIELD_STRING:
            printf("\"%s\"", *(const char **) data);
            break;

        case LCM_FIELD_BOOLEAN:
            printf("%s", (*(int8_t*) data) == 1 ? "true" : "false");
            break;

        case LCM_FIELD_USER_TYPE: {
            const lcmtype_metadata_t *md = lcmtype_db_get_using_name(db, field->typestr);
            if(md == NULL) {
                printf("<unknown-user-type>");
            } else {
                if(usertype_count == NULL) {
                    printf("<USER>");
                } else {
                    int n = ++*usertype_count;
                    printf("<%d>", n);
                }
            }
            break;
        }

        default:
            printf("???");
            fprintf(stderr, "ERR: failed to handle lcm message field type: %s\n", field->typestr);
            break;
    }
}

static size_t typesize(lcm_field_type_t type)
{
    switch(type) {
        case LCM_FIELD_INT8_T:   return sizeof(int8_t);
        case LCM_FIELD_INT16_T:  return sizeof(int16_t);
        case LCM_FIELD_INT32_T:  return sizeof(int32_t);
        case LCM_FIELD_INT64_T:  return sizeof(int64_t);
        case LCM_FIELD_BYTE:     return sizeof(int8_t);
        case LCM_FIELD_FLOAT:    return sizeof(float);
        case LCM_FIELD_DOUBLE:   return sizeof(double);
        case LCM_FIELD_STRING:   return sizeof(const char *);
        case LCM_FIELD_BOOLEAN:  return sizeof(int8_t);

        case LCM_FIELD_USER_TYPE:
        default:
            return 0;
    }
}

static void print_value_array(lcmtype_db_t *db, lcm_field_t *field, void *data, int *usertype_count)
{
    if(field->num_dim == 1) {
        printf("[");
        int len = field->dim_size[0];
        size_t elt_size = typesize(field->type);
        void *p = (!field->dim_is_variable[0]) ? field->data : *(void **) field->data;
        for(int i = 0; i < len; i++) {
            print_value_scalar(db, field, p, usertype_count);
            if(i+1 != len)
                printf(", ");
            p = (void *)((uint8_t *) p + elt_size);
        }
        printf(" ]");
    } else {
        printf("<Multi-dim array: not yet supported>");
    }
}

static inline void strnfmtappend(char *buf, size_t sz, size_t *used, const char *fmt, ...)
{
    assert(*used <= sz);

    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf+*used, sz-*used, fmt, va);
    va_end(va);

    *used += n;
    if(*used > sz)
        *used = sz;
}

void msg_display(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg, const msg_display_state_t *state)
{
    assert(state != NULL);

    /* first, we need to resolve/recurse to the proper submessage */
    #define TRAVERSAL_BUFSZ 1024
    char traversal[TRAVERSAL_BUFSZ];
    size_t traversal_used = 0;

    strnfmtappend(traversal, TRAVERSAL_BUFSZ, &traversal_used,
                  "top");

    size_t i;
    for(i = 0; i < state->cur_depth; i++) {

        // get the desired <USER> id # to recurse on
        size_t recur_i = state->recur_table[i];

        // iterate through the fields until we find the corresponding one
        lcm_field_t field;
        const lcm_type_info_t *typeinfo = metadata->typeinfo;
        int num_fields = typeinfo->num_fields();
        size_t user_field_count = 0;
        int inside_array;
        int index;
        for(int j = 0; j < num_fields; j++) {
            typeinfo->get_field(msg, j, &field);
            inside_array = 0;

            if(field.type == LCM_FIELD_USER_TYPE) {

                // two possiblities here: 1) scalar or 2) array

                // 1) its a scalar
                if(field.num_dim == 0)  {
                    if(++user_field_count == recur_i)
                        break;
                }

                // 2) its an array
                else if(field.num_dim == 1) {
                    inside_array = 1;
                    for(index = 0; index < field.dim_size[0]; index++) {
                        if(++user_field_count == recur_i)
                            goto break_loop;
                    }
                }

                else {
                    //DEBUG(1, "NOTE: Multi-dim arrays not supported\n");
                }
            }
        }

    break_loop:
        // not found?
        if(user_field_count != recur_i)
            break;

        msg = field.data;
        metadata = lcmtype_db_get_using_name(db, field.typestr);
        if(metadata == NULL) {
            printf("ERROR: failed to find %s\n", field.typestr);
            return;
        }

        strnfmtappend(traversal, TRAVERSAL_BUFSZ, &traversal_used,
                      " -> %s", field.name);

        if(inside_array) {

            // If its an array, there are a couple of options:
            //   the array is laid out differently in memory depending on whether
            //   its a fixed-size or variable-size array

            if(!field.dim_is_variable[0]) {
                size_t typesz = metadata->typeinfo->struct_size();
                msg += typesz * index;
            } else {
                // a variable-size array is simply an array of pointers
                // thus, to get the correct USER data ptr, we must index pointers
                // and then derefernce it... this leads to the following mess:
                msg = *(void **)(msg + index * sizeof(void *));
            }
            strnfmtappend(traversal, TRAVERSAL_BUFSZ, &traversal_used,
                          "[%d]", index);
        }

    }

    // sub-message recurse failed?
    if(i != state->cur_depth) {
        printf("ERROR: failed recurse to find sub-messages\n");
        return;
    }

    const lcm_type_info_t *typeinfo = metadata->typeinfo;
    lcm_field_t field;
    int num_fields = typeinfo->num_fields();
    int usertype_count = 0;

    printf("         Traversal: %s \n", traversal);
    printf("   ----------------------------------------------------------------\n");

    for(int i = 0; i < num_fields; i++) {
        typeinfo->get_field(msg, i, &field);

        printf("    %-20.20s %-20.20s ", field.name, field.typestr);

        if(field.num_dim == 0)
            print_value_scalar(db, &field, field.data, &usertype_count);
        else
            print_value_array(db, &field, field.data, &usertype_count);

        printf("\n");
    }
}
