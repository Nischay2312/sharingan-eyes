#pragma once

/*
 * Board-agnostic clip player with a playlist. Plays packed .eyv clips and a flick
 * (or console next/prev) advances to the next clip.
 *
 * Sources (board picks one before eye_video_init; default = 'anim' partition):
 *   - eye_video_set_partition("anim") -- flash data partition holding either a
 *       single .eyv clip OR an .eyp pack (multiple clips). LCD board.
 *   - eye_video_set_dir("/sdcard/eye") -- a folder; every *.eyv in it is one clip
 *       in the playlist (sorted by name). AMOLED board / SD card.
 *   - eye_video_set_file("/path.eyv")  -- a single clip file.
 *
 * Tolerant: no source / no clips -> eye_video_available() is false and the demo
 * stays on procedural effects.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void eye_video_set_partition(const char *label);
void eye_video_set_dir(const char *dir);
void eye_video_set_file(const char *path);

/** Build the playlist + load the first clip. Safe to call always. */
esp_err_t eye_video_init(void);

/** Rebuild the playlist from the same source (e.g. after a web upload added a
 *  file to the SD folder). Thread-safe vs. playback. */
esp_err_t eye_video_rescan(void);

bool eye_video_available(void);
int  eye_video_count(void);          /* number of clips in the playlist */
int  eye_video_current_index(void);  /* 0-based index of the playing clip */

/** Switch clip (+1 next, -1 prev), wrapping. Thread-safe vs. playback. */
void eye_video_step(int delta);

/** Jump to clip `index` (restarts it if already current). Thread-safe. */
void eye_video_select(int index);

/** Short display name of clip `index` (filename, or "clip N" in flash mode).
 *  Returns a static buffer -- copy it if you need it to persist. */
const char *eye_video_clip_name(int index);

/** Playlist index of the clip whose display name equals `name` (case-insensitive),
 *  or -1 if absent. Used for name-based selection across boards / startup clip. */
int eye_video_index_for_name(const char *name);

/** Decode the current clip's next frame into fb (16-aligned) and advance. */
esp_err_t eye_video_next_frame(uint16_t *fb240);

/** ms per frame of the current clip (for the render loop's pacing). */
uint16_t eye_video_frame_ms(void);

/** Reset playback to frame 0 of the current clip. Thread-safe. */
void eye_video_restart(void);

/** Pause or resume playback (frame position is frozen while paused). */
void eye_video_pause(bool on);
bool eye_video_is_paused(void);

/** Print playlist + current-clip info to stdout (console `clip`). */
void eye_video_print_info(void);

#ifdef __cplusplus
}
#endif
