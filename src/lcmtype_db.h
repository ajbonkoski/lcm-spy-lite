#ifndef LCMTYPE_DB_H
#define LCMTYPE_DB_H

#include <stdint.h>
#include <lcm/lcm_coretypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int64_t hash;
    char *typename;  /* owned by this struct */
    const lcm_type_info_t *typeinfo;

} lcmtype_metadata_t;

typedef struct lcmtype_db lcmtype_db_t;

lcmtype_db_t *lcmtype_db_create(const char *paths, int debug);
void lcmtype_db_destroy(lcmtype_db_t *this);

// returns NULL when "not found"
const lcmtype_metadata_t *lcmtype_db_get_metadata(lcmtype_db_t *this, int64_t hash);

#ifdef __cplusplus
}
#endif

#endif  /* LCMTYPE_DB_H */
