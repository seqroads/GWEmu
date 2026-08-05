/* paths.c - files and the per-user directory */

#include "paths.h"
#include <SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

void *file_load(const char *path, u32 *len_out)
{
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }

    void *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (len_out) *len_out = (u32)n;
    return buf;
}

bool file_write(const char *path, const void *data, u32 len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* SDL already knows where each platform keeps user data, and creates it. */
const char *user_dir(void)
{
    static char *cached;
    if (!cached) cached = SDL_GetPrefPath("", "gwemu");
    return cached ? cached : "./";
}

void user_path(char *out, size_t cap, const char *name)
{
    SDL_snprintf(out, cap, "%s%s", user_dir(), name);
}

const char *path_basename(const char *path)
{
    if (!path) return "";
    const char *s = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    return s;
}

void path_dirname(char *out, size_t cap, const char *path)
{
    const char *base = path_basename(path);
    size_t n = (size_t)(base - path);
    if (n == 0) { SDL_strlcpy(out, ".", cap); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    /* Drop the trailing separator, unless the whole path is one. */
    if (n > 1) n--;
    out[n] = 0;
}

/* FNV-1a: stable across platforms, and enough to tell two dumps apart. */
u64 hash_bytes(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int entry_cmp(const void *a, const void *b)
{
    const DirEntry *x = (const DirEntry *)a, *y = (const DirEntry *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return SDL_strcasecmp(x->name, y->name);
}

int dir_list(const char *dir, DirEntry **out)
{
    int cap = 64, n = 0;
    DirEntry *list = (DirEntry *)malloc((size_t)cap * sizeof *list);
    if (!list) { *out = NULL; return 0; }

#ifdef _WIN32
    char pattern[1024];
    SDL_snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".")) continue;
            if (n == cap) {
                cap *= 2;
                DirEntry *p = (DirEntry *)realloc(list, (size_t)cap * sizeof *list);
                if (!p) break;
                list = p;
            }
            SDL_strlcpy(list[n].name, fd.cFileName, sizeof list[n].name);
            list[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            list[n].size = (long)fd.nFileSizeLow;
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".")) continue;
            /* Hidden entries are noise in a file picker, but ".." is not. */
            if (e->d_name[0] == '.' && strcmp(e->d_name, "..")) continue;
            if (n == cap) {
                cap *= 2;
                DirEntry *p = (DirEntry *)realloc(list, (size_t)cap * sizeof *list);
                if (!p) break;
                list = p;
            }
            char full[1024];
            SDL_snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            SDL_strlcpy(list[n].name, e->d_name, sizeof list[n].name);

            struct stat st;
            bool ok = stat(full, &st) == 0;
            list[n].is_dir = ok && S_ISDIR(st.st_mode);
            list[n].size = (ok && S_ISREG(st.st_mode)) ? (long)st.st_size : -1;
            n++;
        }
        closedir(d);
    }
#endif

    qsort(list, (size_t)n, sizeof *list, entry_cmp);
    *out = list;
    return n;
}
