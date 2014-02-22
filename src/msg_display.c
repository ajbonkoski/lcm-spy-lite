#include "msg_display.h"

#include <stdio.h>
#include <inttypes.h>

static void _msg_display_internal(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg, int indent);

static inline int is_ascii(int8_t c)
{
    return (32 <= c && c <= 126);
}

static inline void do_indent(int indent)
{
    for(int i = 0; i <= indent; i++)
        printf("    ");
}

static void print_value_scalar(lcmtype_db_t *db, lcm_field_t *field, void *data, int indent)
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
                printf("<USER>");
                /* printf("\n"); */
                /* _msg_display_internal(db, md, data, indent+1); */
                /* do_indent(indent); */
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

static void print_value_array(lcmtype_db_t *db, lcm_field_t *field, void *data, int indent)
{
    if(field->num_dim == 1) {
        printf("[");
        int len = field->dim_size[0];
        size_t elt_size = typesize(field->type);
        void *p = (!field->dim_is_variable[0]) ? field->data : *(void **) field->data;
        for(int i = 0; i < len; i++) {
            print_value_scalar(db, field, p, indent);
            if(i+1 != len)
                printf(", ");
            p = (void *)((uint8_t *) p + elt_size);
        }
        printf(" ]");
    } else {
        printf("<Multi-dim array: not yet supported>");
    }
}

static void _msg_display_internal(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg, int indent)
{
    const lcm_type_info_t *typeinfo = metadata->typeinfo;
    lcm_field_t field;
    int num_fields = typeinfo->num_fields();

    for(int i = 0; i < num_fields; i++) {
        typeinfo->get_field(msg, i, &field);

        do_indent(indent);
        printf("%-15s %-15s ", field.name, field.typestr);

        if(field.num_dim == 0)
            print_value_scalar(db, &field, field.data, indent);
        else
            print_value_array(db, &field, field.data, indent);

        printf("\n");
    }
}

void msg_display(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg)
{
    _msg_display_internal(db, metadata, msg, 0);
}
