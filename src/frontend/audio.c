/* audio.c - SDL playback of the emulated SAI output */

#include "audio.h"
#include "config.h"
#include <SDL.h>

static SDL_AudioDeviceID device;
static int device_rate = 48000;

/* SDL hands over the whole buffer to fill; it is mono 16-bit. */
static void SDLCALL feed(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    s16 *out = (s16 *)stream;
    int frames = len / (int)sizeof(s16);

    emu_audio_render(out, (u32)frames, (u32)device_rate);

    if (config.volume != 100) {
        int v = config.volume;
        for (int i = 0; i < frames; i++) {
            int x = out[i] * v / 100;
            out[i] = (s16)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
        }
    }
}

bool audio_open(void)
{
    if (device) return true;
    if (!config.audio_enabled) return false;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = 48000;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = feed;

    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!device) {
        gwlog("[audio] cannot open a playback device: %s\n", SDL_GetError());
        return false;
    }
    if (have.freq) device_rate = have.freq;

    SDL_PauseAudioDevice(device, 0);
    gwlog("[audio] playing at %d Hz\n", device_rate);
    return true;
}

void audio_close(void)
{
    if (!device) return;
    SDL_CloseAudioDevice(device);
    device = 0;
}

void audio_set_paused(bool paused)
{
    if (device) SDL_PauseAudioDevice(device, paused ? 1 : 0);
}

void audio_reconfigure(void)
{
    audio_close();
    if (config.audio_enabled) audio_open();
}
