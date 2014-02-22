#ifndef MSG_DISPLAY_H
#define MSG_DISPLAY_H

#include "lcmtype_db.h"

#ifdef __cplusplus
extern "C" {
#endif

void msg_display(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg);

#ifdef __cplusplus
}
#endif

#endif  /* MSG_DISPLAY_H */
