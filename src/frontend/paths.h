/* paths.h - files and the per-user directory */
#ifndef GW_PATHS_H
#define GW_PATHS_H

#include "../core/gw.h"

/* Reads a whole file. Caller frees. NULL on failure. */
void *file_load(const char *path, u32 *len_out);
bool  file_write(const char *path, const void *data, u32 len);
long  file_size(const char *path);          /* -1 if absent */

/* Writable per-user directory, created on demand, with a trailing separator. */
const char *user_dir(void);

/* Joins the user directory and a name into `out`. */
void user_path(char *out, size_t cap, const char *name);

const char *path_basename(const char *path);
/* Copies the directory part of `path` into `out`, or "." if there is none. */
void path_dirname(char *out, size_t cap, const char *path);

u64 hash_bytes(const void *data, u32 len);

typedef struct {
    char name[256];
    bool is_dir;
    long size;
} DirEntry;

/* Lists a directory, folders first then files, each alphabetical. Returns the
   count and stores a malloc'd array in *out, which the caller frees. */
int dir_list(const char *dir, DirEntry **out);

#endif /* GW_PATHS_H */
