/* session.c - firmware loading and save-file bookkeeping
 *
 * The device has two flash images: 128 KiB of internal flash holding the ARM
 * firmware, and 1 or 4 MiB of encrypted external flash holding the assets and
 * the save area. Dumps are named inconsistently, so the pair is identified by
 * size and the partner looked up beside whichever file was picked.
 */

#include "session.h"
#include "paths.h"
#include <SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

Session session;

char session_classify(long len)
{
    if (len == FLASH_SIZE) return 'i';
    for (long sz = 1024 * 1024; sz <= (long)EXTFLASH_MAX; sz *= 2)
        if (len == sz) return 'e';
    return 0;
}

/* Looks in the given file's directory for one of the wanted kind. */
static bool find_partner(const char *beside, char want, char *out, size_t cap)
{
    char dir[SESSION_PATH_MAX];
    path_dirname(dir, sizeof dir, beside);
    const char *self = path_basename(beside);

#ifdef _WIN32
    char pattern[SESSION_PATH_MAX];
    SDL_snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!strcmp(fd.cFileName, self)) continue;
        char full[SESSION_PATH_MAX];
        SDL_snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (session_classify(file_size(full)) == want) {
            SDL_strlcpy(out, full, cap);
            FindClose(h);
            return true;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, self)) continue;
        char full[SESSION_PATH_MAX];
        SDL_snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (session_classify(file_size(full)) == want) {
            SDL_strlcpy(out, full, cap);
            closedir(d);
            return true;
        }
    }
    closedir(d);
#endif
    return false;
}

/* Keyed by content, so two dumps never share a save and renaming one does not
   orphan it. The readable part is for humans browsing the folder. */
static void derive_save_path(char *out, size_t cap, const char *int_path, u64 hash)
{
    char stem[48];
    SDL_strlcpy(stem, path_basename(int_path), sizeof stem);
    for (char *p = stem; *p; p++) {
        if (*p == '.') { *p = 0; break; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) *p = '_';
    }
    if (!stem[0]) SDL_strlcpy(stem, "firmware", sizeof stem);

    char name[128];
    SDL_snprintf(name, sizeof name, "%s-%016llx.sav", stem, (unsigned long long)hash);
    user_path(out, cap, name);
}

const char *session_load(const char *int_path, const char *ext_path,
                         const AppOptions *o)
{
    static char err[320];
    void *intbuf = NULL, *extbuf = NULL;
    u32 intlen = 0, extlen = 0;
    char rint[SESSION_PATH_MAX] = {0}, rext[SESSION_PATH_MAX] = {0};
    const char *fail = NULL;

    if ((!int_path || !*int_path) && (!ext_path || !*ext_path))
        return "No firmware image given.";

    if (int_path && *int_path && (!ext_path || !*ext_path)) {
        void *buf = file_load(int_path, &intlen);
        if (!buf) { SDL_snprintf(err, sizeof err, "Cannot read %s", int_path); return err; }

        if (session_classify(intlen) == 'e') {
            extbuf = buf; extlen = intlen; intlen = 0;
            SDL_strlcpy(rext, int_path, sizeof rext);
            if (!find_partner(int_path, 'i', rint, sizeof rint)) {
                fail = "That is the external flash image, and there is no 128 KiB "
                       "internal flash dump beside it.";
                goto done;
            }
            intbuf = file_load(rint, &intlen);
        } else {
            intbuf = buf;
            SDL_strlcpy(rint, int_path, sizeof rint);
            if (!find_partner(int_path, 'e', rext, sizeof rext)) {
                fail = "Found the internal flash image, but no external flash "
                       "dump beside it.";
                goto done;
            }
            extbuf = file_load(rext, &extlen);
        }
    } else if (!int_path || !*int_path) {
        extbuf = file_load(ext_path, &extlen);
        if (!extbuf) { SDL_snprintf(err, sizeof err, "Cannot read %s", ext_path); return err; }
        SDL_strlcpy(rext, ext_path, sizeof rext);
        if (!find_partner(ext_path, 'i', rint, sizeof rint)) {
            fail = "No 128 KiB internal flash dump found beside that image.";
            goto done;
        }
        intbuf = file_load(rint, &intlen);
    } else {
        intbuf = file_load(int_path, &intlen);
        extbuf = file_load(ext_path, &extlen);
        SDL_strlcpy(rint, int_path, sizeof rint);
        SDL_strlcpy(rext, ext_path, sizeof rext);
    }

    if (!intbuf || !extbuf) { fail = "One of the two flash images could not be read."; goto done; }

    if (session_classify(intlen) == 'e' && extlen == FLASH_SIZE) {
        fail = "Those two images are the wrong way round.";
        goto done;
    }

    session_close(false);

    EmuFirmware fw;
    memset(&fw, 0, sizeof fw);
    fw.int_data = intbuf; fw.int_len = intlen;
    fw.ext_data = extbuf; fw.ext_len = extlen;
    fw.core_hz  = o ? o->core_hz : 0;
    fw.rtc_host = o ? o->rtc_host : false;

    if (!emu_open(&fw)) { fail = "The emulator could not start on this firmware."; goto done; }

    session.open = true;
    session.hash = hash_bytes(intbuf, intlen);
    SDL_strlcpy(session.int_path, rint, sizeof session.int_path);
    SDL_strlcpy(session.ext_path, rext, sizeof session.ext_path);
    SDL_strlcpy(session.title, path_basename(rint), sizeof session.title);

    session.save_enabled = !(o && o->no_save);
    if (o && o->save_path) SDL_strlcpy(session.save_path, o->save_path, sizeof session.save_path);
    else derive_save_path(session.save_path, sizeof session.save_path, rint, session.hash);

    if (session.save_enabled && !emu_load_nvram(session.save_path))
        gwlog("[session] no save data at %s; starting factory fresh\n", session.save_path);

    gwlog("[session] loaded %s + %s\n", rint, rext);

done:
    free(intbuf);
    free(extbuf);
    return fail;
}

void session_flush(void)
{
    if (session.open && session.save_enabled) emu_save_nvram(session.save_path);
}

void session_close(bool graceful)
{
    if (!session.open) return;
    if (graceful && !emu_halted()) emu_power_off_and_wait();
    session_flush();
    emu_close();
    memset(&session, 0, sizeof session);
}
