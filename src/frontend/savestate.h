/* savestate.h - whole-machine snapshots in numbered slots */
#ifndef GW_SAVESTATE_H
#define GW_SAVESTATE_H

#include "../core/emu.h"

#define SAVESTATE_SLOTS 10

int  savestate_slot(void);
void savestate_set_slot(int n);

bool savestate_exists(int n);
bool savestate_save(int n);
bool savestate_load(int n);

#endif /* GW_SAVESTATE_H */
