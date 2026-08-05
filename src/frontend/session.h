/* session.h - what firmware is loaded, and where its save data lives */
#ifndef GW_SESSION_H
#define GW_SESSION_H

#include "options.h"

#define SESSION_PATH_MAX 512

typedef struct {
    bool open;
    char int_path[SESSION_PATH_MAX];
    char ext_path[SESSION_PATH_MAX];
    char save_path[SESSION_PATH_MAX];
    char title[64];
    u64  hash;
    bool save_enabled;
} Session;

extern Session session;

/* Either path may be NULL: the other one's directory is searched for a
   matching image. Returns NULL on success, or the reason it failed. */
const char *session_load(const char *int_path, const char *ext_path,
                         const AppOptions *o);

void session_close(bool graceful);
void session_flush(void);

/* 128 KiB is internal flash, 1 or 4 MiB external. Returns 'i', 'e' or 0. */
char session_classify(long len);

#endif /* GW_SESSION_H */
