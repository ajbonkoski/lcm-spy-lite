#ifndef MSG_DISPLAY_H
#define MSG_DISPLAY_H

#include "lcmtype_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* this struct tracks the current message decoding state
   this msg_display module does *not* handle I/O and the user
   transitions between decoding states. Thus, this msg_display_state_t
   struct informs msg_display about how to decode/display the message.
   The struct is NOT modified by msg_display() and should be
   configured, owned, and passed by the caller.
*/

#define MSG_DISPLAY_RECUR_MAX 64
typedef struct
{
    size_t cur_depth;
    size_t recur_table[MSG_DISPLAY_RECUR_MAX];

} msg_display_state_t;

void msg_display(lcmtype_db_t *db, const lcmtype_metadata_t *metadata, void *msg, const msg_display_state_t *state);

#ifdef __cplusplus
}
#endif

#endif  /* MSG_DISPLAY_H */
