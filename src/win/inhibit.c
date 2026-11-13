/* Tahan layar mati, kunci otomatis, dan tidur selama iPad tersambung.
 * Memakai iPad sebagai layar sentuh berarti komputer tidak menerima masukan
 * lokal; tanpa ini Windows mematikan layar setelah beberapa menit dan
 * Desktop Duplication berhenti mengirim gambar.
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>

#include "win.h"

static gboolean held;

void idle_inhibit_set(gboolean on)
{
    if (on == held)
        return;
    held = on;
    /* Selalu dipanggil dari thread utama; status ini melekat pada thread itu. */
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
                               : ES_CONTINUOUS);
}
