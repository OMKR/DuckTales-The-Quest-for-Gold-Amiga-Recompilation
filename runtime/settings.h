/* User settings shared by the game and the settings program, stored in
 * ducktales.ini next to the executable. Precedence in the game:
 * defaults < ducktales.ini < environment (DT_*) < command line. */
#ifndef SETTINGS_H
#define SETTINGS_H

typedef struct {
    int fps;            /* 1-200, game speed */
    int music_hz;       /* 1-200, music player rate */
    int sound;          /* 0/1 */
    int volume;         /* 0-100 % */
    int fullscreen;     /* 0/1 */
    int scale;          /* window size: 1-5 x 320x256 */
    int deadzone;       /* controller stick dead zone, 5-90 % */
    int copy_protection;  /* 0/1: show the code wheel check */
    int keyboard_mode;  /* 0 = Amiga keyboard, 1 = keys drive the joystick */
    char gamedir[512];  /* folder holding DT1/ and DT2/ */
} Settings;

extern Settings settings;

void settings_defaults(Settings *s);
/* path of ducktales.ini in the executable's folder */
const char *settings_path(void);
int settings_load(Settings *s, const char *path);   /* 1 if the file was read */
int settings_save(const Settings *s, const char *path);
void settings_clamp(Settings *s);

#endif
