#include "timeutil.h"
#include "msg_display.h"
#include "lcmtype_db.h"

#include <glib.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <assert.h>
#include <lcm/lcm.h>
#include <lcm/lcm_coretypes.h>

#define SELECT_TIMEOUT 20000
#define ESCAPE_KEY 0x1B
#define DEL_KEY 0x7f

/* this needs to be global unfortunately, so the sig_handler can set it to 1 */
static volatile int64_t quit = 0;

#define DEBUG_LEVEL 2  /* 0=nothing, higher values mean more verbosity */
#define DEBUG_FILENAME "/tmp/spy-lite-debug.log"
static FILE *DEBUG_FILE = NULL;
static void DEBUG_INIT(void) { DEBUG_FILE = fopen(DEBUG_FILENAME, "w"); }

static void DEBUG(int level, const char *fmt, ...)
{
    if(!DEBUG_LEVEL || level > DEBUG_LEVEL)
        return;

    va_list vargs;
    va_start(vargs, fmt);
    vfprintf(DEBUG_FILE, fmt, vargs);
    va_end(vargs);

    fflush(DEBUG_FILE);
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////// Structs /////////////////////////////
//////////////////////////////////////////////////////////////////////

typedef struct msg_info msg_info_t;

enum display_mode { MODE_OVERVIEW, MODE_DECODE };
typedef struct spyinfo spyinfo_t;
struct spyinfo
{
    GArray *names_array;
    GHashTable *minfo_hashtbl;
    lcmtype_db_t *type_db;
    pthread_mutex_t mutex;
    float display_hz;

    enum display_mode mode;
    int is_selecting;

    int decode_index;
    msg_info_t *decode_msg_info;
    const char *decode_msg_channel;
};


#define QUEUE_PERIOD (4*1000*1000)   /* queue up to 4 sec of utimes */
#define QUEUE_SIZE   (400)           /* hold up to 400 utimes */

struct msg_info
{
    const char *channel;

    uint64_t queue[QUEUE_SIZE];
    int front;  // front: the next index to dequeue
    int back;   // back-1: the last index enqueued
    int is_full; // when front == back: the queue could be either full or empty (this disambiguates)

    spyinfo_t *spy;
    int64_t hash;
    const lcmtype_metadata_t *metadata;
    msg_display_state_t disp_state;
    void *last_msg;

    uint64_t num_msgs;
};

static msg_info_t *msg_info_create(spyinfo_t *spy, const char *channel)
{
    msg_info_t *this = calloc(1, sizeof(msg_info_t));
    this->channel = channel;

    this->front = 0;
    this->back = 0;
    this->is_full = 0; /* false */

    this->spy = spy;
    this->hash = 0;
    this->metadata = NULL;
    this->disp_state.cur_depth = 0;
    this->last_msg = NULL;

    this->num_msgs = 0;

    return this;
}

static void _msg_info_ensure_hash(msg_info_t *this, int64_t hash)
{
    if(this->hash == hash)
        return;

    // if this not the first message, warn user
    if(this->hash != 0) {
        DEBUG(1, "WRN: hash changed, searching for new lcmtype on channel %s\n", this->channel);
    }

    // cleanup old memory if needed
    if(this->metadata != NULL && this->last_msg != NULL) {
        this->metadata->typeinfo->decode_cleanup(this->last_msg);
        free(this->last_msg);
        this->last_msg = NULL;
    }

    this->hash = hash;
    this->metadata = lcmtype_db_get_using_hash(this->spy->type_db, hash);
    if(this->metadata == NULL) {
        DEBUG(1, "WRN: failed to find lcmtype for hash: 0x%"PRIx64"\n", hash);
        return;
    }
}

static inline int _msg_info_is_empty(msg_info_t *this)
{
    return !this->is_full && (this->front == this->back);
}

static inline int _msg_info_get_size(msg_info_t *this)
{
    if(this->is_full)
        return QUEUE_SIZE;
    if(this->front <= this->back)
        return this->back - this->front;
    else /* this->front > this->back */
        return QUEUE_SIZE - (this->front - this->back);
}

static inline void _msg_info_dequeue(msg_info_t *this)
{
    /* assert not empty */
    assert(!_msg_info_is_empty(this));
    ++this->front;
    this->front %= QUEUE_SIZE;
    this->is_full = 0; /* false */
}

static inline void _msg_info_enqueue(msg_info_t *this, uint64_t utime)
{
    assert(!this->is_full);
    this->queue[this->back++] = utime;
    this->back %= QUEUE_SIZE;
    if(this->front == this->back)
        this->is_full = 1; /* true */
}

static uint64_t _msg_info_latest_utime(msg_info_t *this)
{
    assert(!_msg_info_is_empty(this));

    int i = this->back - 1;
    if(i < 0)
        i = QUEUE_SIZE - 1;

    return this->queue[i];
}

static uint64_t _msg_info_oldest_utime(msg_info_t *this)
{
    assert(!_msg_info_is_empty(this));
    return this->queue[this->front];
}

static void _msg_info_remove_old(msg_info_t *this)
{
    uint64_t oldest_allowed = timestamp_now() - QUEUE_PERIOD;

    // discard old messages
    while(!_msg_info_is_empty(this)) {
        uint64_t last = this->queue[this->front];
        if(last < oldest_allowed)
            _msg_info_dequeue(this);
        else
            break;
    }

}

static void msg_info_add_msg(msg_info_t *this, uint64_t utime, const lcm_recv_buf_t *rbuf)
{
    if(this->is_full)
        _msg_info_dequeue(this);

    _msg_info_enqueue(this, utime);

    this->num_msgs++;

    /* decode the data */
    int64_t hash;
    __int64_t_decode_array(rbuf->data, 0, rbuf->data_size, &hash, 1);
    _msg_info_ensure_hash(this, hash);

    if(this->metadata != NULL) {

        // do we need to allocate memory for 'last_msg' ?
        if(this->last_msg == NULL) {
            size_t sz = this->metadata->typeinfo->struct_size();
            this->last_msg = malloc(sz);
        } else {
            this->metadata->typeinfo->decode_cleanup(this->last_msg);
        }

        // actually decode it
        this->metadata->typeinfo->decode(rbuf->data, 0, rbuf->data_size, this->last_msg);

        DEBUG(1, "INFO: successful decode on %s\n", this->channel);
    }

}

static float msg_info_get_hz(msg_info_t *this)
{
    _msg_info_remove_old(this);
    if(_msg_info_is_empty(this))
        return 0.0;

    int sz = _msg_info_get_size(this);
    uint64_t old = _msg_info_oldest_utime(this);
    uint64_t new = _msg_info_latest_utime(this);
    uint64_t dt = new - old;
    if(dt == 0.0)
        return 0.0;

    return (float) sz / ((float) dt / 1000000.0);
}

static msg_info_t *get_current_msg_info(spyinfo_t *spy, const char **channel)
{
    const char *ch = g_array_index(spy->names_array, const char *, spy->decode_index);
    assert(ch != NULL);
    msg_info_t *minfo = (msg_info_t *) g_hash_table_lookup(spy->minfo_hashtbl, ch);
    assert(minfo != NULL);
    if(channel != NULL) *channel = ch;
    return minfo;
}

static void msg_info_destroy(msg_info_t *this)
{
    free(this);
}

static int is_valid_channel_num(spyinfo_t *spy, int index)
{
    return (0 <= index && index < spy->names_array->len);
}

static void keyboard_handle_overview(spyinfo_t *spy, char ch)
{
    if(ch == '-') {
        spy->is_selecting = 1; /* true */
        spy->decode_index = -1;
    } else if('0' <= ch && ch <= '9') {
        // shortcut for single digit channels
        if(!spy->is_selecting) {
            spy->decode_index = ch - '0';
            if(is_valid_channel_num(spy, spy->decode_index)) {
                spy->decode_msg_info = get_current_msg_info(spy, &spy->decode_msg_channel);
                spy->mode = MODE_DECODE;
            }
        } else {
            if(spy->decode_index == -1) {
                spy->decode_index = ch - '0';
            } else if(spy->decode_index < 10000) {
                spy->decode_index *= 10;
                spy->decode_index += (ch - '0');
            }
        }
    } else if(ch == '\n') {
        if(spy->is_selecting) {
            if(is_valid_channel_num(spy, spy->decode_index)) {
                spy->decode_msg_info = get_current_msg_info(spy, &spy->decode_msg_channel);
                spy->mode = MODE_DECODE;
            }
            spy->is_selecting = 0; /* false */
        }
    } else if(ch == '\b' || ch == DEL_KEY) {
        if(spy->is_selecting) {
            if(spy->decode_index < 10)
                spy->decode_index = -1;
            else
                spy->decode_index /= 10;
        }
    } else {
        DEBUG(1, "INFO: unrecognized input: '%c' (0x%2x)\n", ch, ch);
    }
}

static void keyboard_handle_decode(spyinfo_t *spy, char ch)
{
    msg_info_t *minfo = spy->decode_msg_info;
    msg_display_state_t *ds = &minfo->disp_state;

    if(ch == ESCAPE_KEY) {
        if(ds->cur_depth > 0)
            ds->cur_depth--;
        else
            spy->mode = MODE_OVERVIEW;
    } else if('0' <= ch && ch <= '9') {
        // if number is pressed, set and increase sub-msg decoding depth
        if(ds->cur_depth < MSG_DISPLAY_RECUR_MAX) {
            ds->recur_table[ds->cur_depth++] = (ch - '0');
        } else {
            DEBUG(1, "INFO: cannot recurse further: reached maximum depth of %d\n",
                  MSG_DISPLAY_RECUR_MAX);
        }
    } else {
        DEBUG(1, "INFO: unrecognized input: '%c' (0x%2x)\n", ch, ch);
    }
}

void *keyboard_thread_func(void *arg)
{
    spyinfo_t *spy = (spyinfo_t *)arg;

    struct termios old = {0};
    if (tcgetattr(0, &old) < 0)
        perror("tcsetattr()");

    struct termios new = old;
    new.c_lflag &= ~ICANON;
    new.c_lflag &= ~ECHO;
    new.c_cc[VMIN] = 1;
    new.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &new) < 0)
        perror("tcsetattr ICANON");

    char ch;
    while(!quit) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);

        struct timeval timeout = { 0, SELECT_TIMEOUT };
        int status = select(1, &fds, 0, 0, &timeout);

        if(quit)
            break;

        if(status != 0 && FD_ISSET(0, &fds)) {

            if(read(0, &ch, 1) < 0)
                perror ("read()");

            pthread_mutex_lock(&spy->mutex);
            {
                switch(spy->mode) {
                    case MODE_OVERVIEW: keyboard_handle_overview(spy, ch); break;
                    case MODE_DECODE:   keyboard_handle_decode(spy, ch);  break;
                    default:
                        DEBUG(1, "INFO: unrecognized keyboard mode: %d\n", spy->mode);
                }
            }
            pthread_mutex_unlock(&spy->mutex);

        } else {
            DEBUG(4, "INFO: keyboard_thread_func select() timeout\n");
        }
    }

    if (tcsetattr(0, TCSADRAIN, &old) < 0)
        perror ("tcsetattr ~ICANON");

    return NULL;
}
//////////////////////////////////////////////////////////////////////
////////////////////////// Helper Functions //////////////////////////
//////////////////////////////////////////////////////////////////////

void clearscreen()
{
    // clear
    printf("\033[2J");

    // move cursor to (0, 0)
    printf("\033[0;0H");
}

//////////////////////////////////////////////////////////////////////
//////////////////////////// Print Thread ////////////////////////////
//////////////////////////////////////////////////////////////////////

static void display_overview(spyinfo_t *spy)
{
    printf("         %-28s\t%12s\t%8s\n", "Channel", "Num Messages", "Hz (ave)");
    printf("   ----------------------------------------------------------------\n");

    DEBUG(5, "start-loop\n");

    for(int i = 0; i < spy->names_array->len; i++) {
        const char *channel = g_array_index(spy->names_array, const char *, i);
        if(channel == NULL)
            break;

        msg_info_t *minfo = (msg_info_t *) g_hash_table_lookup(spy->minfo_hashtbl, channel);
        assert(minfo != NULL);
        float hz = msg_info_get_hz(minfo);
        printf("   %3d)  %-28s\t%9"PRIu64"\t%7.2f\n", i, (char *)channel, minfo->num_msgs, hz);
    }

    printf("\n");

    if(spy->is_selecting) {
        printf("   Decode channel: ");
        if(spy->decode_index != -1)
            printf("%d", spy->decode_index);
        fflush(stdout);
    }
}

static void display_decode(spyinfo_t *spy)
{
    msg_info_t *minfo = spy->decode_msg_info;
    const char *channel = spy->decode_msg_channel;

    const char *typename = (minfo->metadata != NULL) ? minfo->metadata->typename : NULL;
    int64_t hash = (minfo->metadata != NULL) ? minfo->metadata->typeinfo->get_hash() : 0;
    printf("         Decoding %s (%s) %"PRIu64":\n", channel, typename, (uint64_t) hash);

    if(minfo->last_msg != NULL)
        msg_display(spy->type_db, minfo->metadata, minfo->last_msg, &minfo->disp_state);
}

void *print_thread_func(void *arg)
{
    spyinfo_t *spy = (spyinfo_t *)arg;

    const double MAX_FREQ = 100.0;
    int period;
    float hz = 10.0;

    if (spy->display_hz <= 0) {
        DEBUG(1, "WRN: Invalid Display Hz, defaulting to %3.3fHz\n", hz);
    } else if (spy->display_hz > MAX_FREQ) {
        DEBUG(1, "WRN: Invalid Display Hz, defaulting to %1.0f Hz\n", MAX_FREQ);
        hz = MAX_FREQ;
    } else {
        hz = spy->display_hz;
    }

    period = 1000000 / hz;

    DEBUG(1, "INFO: %s: Starting\n", "print_thread");
    while (!quit) {
        usleep(period);

        clearscreen();
        printf("  **************************************************************************** \n");
        printf("  ************************** LCM-SPY (lite) [%3.1f Hz] ************************ \n", hz);
        printf("  **************************************************************************** \n");

        pthread_mutex_lock(&spy->mutex);
        {
            switch(spy->mode) {

                case MODE_OVERVIEW:
                    display_overview(spy);
                    break;

                case MODE_DECODE:
                    display_decode(spy);
                    break;

                default:
                    DEBUG(1, "ERR: unknown mode\n");
            }
        }
        pthread_mutex_unlock(&spy->mutex);

        // flush the stdout buffer (required since we use full buffering)
        fflush(stdout);
    }

    DEBUG(1, "INFO: %s: Ending\n", "print_thread");

    return NULL;
}

//////////////////////////////////////////////////////////////////////
///////////////////////////// LCM HANDLER ////////////////////////////
//////////////////////////////////////////////////////////////////////

static gint names_array_cmp(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

void handler_all_lcm (const lcm_recv_buf_t *rbuf,
                      const char *channel, void *arg)
{
    spyinfo_t *spy = (spyinfo_t *)arg;
    msg_info_t *minfo;
    uint64_t utime = timestamp_now();

    pthread_mutex_lock(&spy->mutex);
    {
        minfo = (msg_info_t *) g_hash_table_lookup(spy->minfo_hashtbl, channel);
        if (minfo == NULL) {
            char *channel_copy = strdup(channel);
            minfo = msg_info_create(spy, channel_copy);
            g_array_append_val(spy->names_array, channel_copy);
            g_array_sort(spy->names_array, names_array_cmp);
            g_hash_table_insert(spy->minfo_hashtbl, channel_copy, minfo);
        }

        msg_info_add_msg(minfo, utime, rbuf);
    }
    pthread_mutex_unlock(&spy->mutex);
}

void *lcm_thread_func(void *usr)
{
    spyinfo_t *spy = (spyinfo_t *) usr;

    DEBUG(1, "INFO: %s: Starting\n", "lcm_thread");

    // lcm setup
    lcm_t *lcm = NULL;
    lcm_subscription_t *lcm_all = NULL;

    lcm = lcm_create(NULL);
    if(lcm == NULL) {
        DEBUG(1, "ERR: failed to create an lcm object!\n");
        quit = 1;
        goto done;
    }

    lcm_all = lcm_subscribe(lcm, ".*", handler_all_lcm, spy);
    if(lcm_all == NULL) {
        DEBUG(1, "ERR: failed to create an subscibe to all\n");
        quit = 1;
        goto done;
    }

    // all is good, lets handle it...
    while(!quit) {

        /* we use async lcm_handle(), so we can pthread_join() properly */
        int lcm_fd = lcm_get_fileno(lcm);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(lcm_fd, &fds);

        struct timeval timeout = { 0, SELECT_TIMEOUT };
        int status = select(lcm_fd + 1, &fds, 0, 0, &timeout);

        if(quit)
            break;

        if(status != 0 && FD_ISSET(lcm_fd, &fds)) {
            int err = lcm_handle(lcm);
            if (err) {
                DEBUG(1, "ERR: lcm_handle() returned an error\n");
                quit = 1;
            }
        } else {
            DEBUG(4, "INFO: lcm_handle() timeout\n");
        }

    }

    DEBUG(1, "INFO: %s: Ending\n", "lcm_thread");

 done:
    if(lcm_all != NULL)  lcm_unsubscribe(lcm, lcm_all);
    if(lcm != NULL)      lcm_destroy(lcm);
    return NULL;
}

//////////////////////////////////////////////////////////////////////
////////////////////////////////// MAIN //////////////////////////////
//////////////////////////////////////////////////////////////////////

static void sighandler(int s)
{
    switch(s) {
        case SIGQUIT:
        case SIGINT:
        case SIGTERM:
            DEBUG(1, "Caught signal...\n");
            quit = 1;
            break;
        default:
            DEBUG(1, "WRN: unrecognized signal fired\n");
            break;
    }
}

int main(int argc, char *argv[])
{
    DEBUG_INIT();
    int is_debug_mode = 0; /* false */

    // XXX get opt
    if(argc > 1 && strcmp(argv[1], "--debug") == 0)
        is_debug_mode = 1;

    // get the lcmtypes .so from LCM_SPY_LITE_PATH
    const char *lcm_spy_lite_path = getenv("LCM_SPY_LITE_PATH");
    if(is_debug_mode)
        printf("lcm_spy_lite_path='%s'\n", lcm_spy_lite_path);
    if(lcm_spy_lite_path == NULL) {
        fprintf(stderr, "ERR: invalid $LCM_SPY_LITE_PATH\n");
        return 1;
    }

    spyinfo_t spy = {
        .names_array = g_array_new(TRUE, TRUE, sizeof(char *)),
        .minfo_hashtbl = g_hash_table_new_full(g_str_hash, g_str_equal,
                       (GDestroyNotify) free, (GDestroyNotify) msg_info_destroy),
        .type_db = lcmtype_db_create(lcm_spy_lite_path, is_debug_mode),
        .display_hz = 10,
        .mode = MODE_OVERVIEW,
        .is_selecting = 0,
        .decode_index = 0
    };

    if(is_debug_mode)
        exit(0);


    if (spy.names_array == NULL) {
        DEBUG(1, "ERR: failed to create array\n");
        exit(-1);
    }

    if (spy.minfo_hashtbl == NULL) {
        DEBUG(1, "ERR: failed to create hashtable\n");
        exit(-1);
    }

    if (spy.type_db == NULL) {
        DEBUG(1, "ERR: failed to load lcmtypes\n");
        exit(-1);
    }

    signal(SIGINT, sighandler);
    signal(SIGQUIT, sighandler);
    signal(SIGTERM, sighandler);

    // configure stdout buffering: use FULL buffering to avoid flickering
    setvbuf(stdout, NULL, _IOFBF, 2048);

    // start threads
    pthread_mutex_init(&spy.mutex, NULL);

    pthread_t print_thread;
    if (pthread_create(&print_thread, NULL, (void *) print_thread_func, &spy)) {
        printf("ERR: %s: Failed to start thread\n", "print_thread");
        exit(-1);
    }

    pthread_t keyboard_thread;
    if (pthread_create(&keyboard_thread, NULL, (void *) keyboard_thread_func, &spy)) {
        printf("ERR: %s: Failed to start thread\n", "keyboard_thread");
        exit(-1);
    }

    // use this thread as the lcm thread
    lcm_thread_func(&spy);


    // cleanup
    pthread_join(keyboard_thread, NULL);
    pthread_join(print_thread, NULL);
    pthread_mutex_destroy(&spy.mutex);
    lcmtype_db_destroy(spy.type_db);
    g_array_free(spy.names_array, TRUE);
    g_hash_table_destroy(spy.minfo_hashtbl);

    DEBUG(1, "Exiting...\n");
    return 0;
}
