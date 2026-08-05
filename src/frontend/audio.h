/* audio.h - host audio output */
#ifndef GW_AUDIO_H
#define GW_AUDIO_H

#include "../core/emu.h"

bool audio_open(void);
void audio_close(void);
void audio_set_paused(bool paused);
void audio_reconfigure(void);

#endif /* GW_AUDIO_H */
