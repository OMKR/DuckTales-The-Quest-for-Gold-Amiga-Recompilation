#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

void video_init(void);
void video_load_view(uint32_t view);
void video_frame(void);
int video_save_bmp(const char *path);
void video_set_title(const char *title);
void video_toggle_fullscreen(void);
void video_set_vsync(int on);

/* frame rate 1-200 (chips.c) */
void chips_set_fps(int value);
int chips_get_fps(void);
void chips_set_music_hz(int value);

/* chips.c */
void chips_init(void);
void hle_poll(void);
void hle_wait_frame(void);
void add_int_server(int num, uint32_t is);
void rem_int_server(int num, uint32_t is);
uint32_t set_int_vector(int num, uint32_t is);
extern int poll_count;
extern uint32_t frame_counter;

#endif
