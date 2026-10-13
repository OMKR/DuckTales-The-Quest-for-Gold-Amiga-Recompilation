/* ducktales.ini reading and writing (simple key = value lines). */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

Settings settings;

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->fps = 25;
    s->music_hz = 50;
    s->sound = 1;
    s->volume = 100;
    s->fullscreen = 0;
    s->scale = 3;
    s->deadzone = 40;
    s->copy_protection = 1;
    s->keyboard_mode = 0;
    strcpy(s->gamedir, "../files");
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_clamp(Settings *s)
{
    s->fps = clampi(s->fps, 1, 200);
    s->music_hz = clampi(s->music_hz, 1, 200);
    s->sound = s->sound != 0;
    s->volume = clampi(s->volume, 0, 100);
    s->fullscreen = s->fullscreen != 0;
    s->scale = clampi(s->scale, 1, 5);
    s->deadzone = clampi(s->deadzone, 5, 90);
    s->copy_protection = s->copy_protection != 0;
    s->keyboard_mode = s->keyboard_mode != 0;
}

const char *settings_path(void)
{
    static char path[1024];
    if (!path[0]) {
        char *base = SDL_GetBasePath();
        snprintf(path, sizeof path, "%sducktales.ini", base ? base : "");
        SDL_free(base);
    }
    return path;
}

int settings_load(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[640];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        val[strcspn(val, "\r\n")] = 0;
        int n = atoi(val);
        if (!strcmp(key, "fps")) s->fps = n;
        else if (!strcmp(key, "music_hz")) s->music_hz = n;
        else if (!strcmp(key, "sound")) s->sound = n;
        else if (!strcmp(key, "volume")) s->volume = n;
        else if (!strcmp(key, "fullscreen")) s->fullscreen = n;
        else if (!strcmp(key, "scale")) s->scale = n;
        else if (!strcmp(key, "deadzone")) s->deadzone = n;
        else if (!strcmp(key, "copy_protection")) s->copy_protection = n;
        else if (!strcmp(key, "keyboard_mode")) s->keyboard_mode = n;
        else if (!strcmp(key, "gamedir") && *val)
            snprintf(s->gamedir, sizeof s->gamedir, "%s", val);
    }
    fclose(f);
    settings_clamp(s);
    return 1;
}

int settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# DuckTales: The Quest for Gold - settings (settings.exe)\n");
    fprintf(f, "fps = %d\n", s->fps);
    fprintf(f, "music_hz = %d\n", s->music_hz);
    fprintf(f, "sound = %d\n", s->sound);
    fprintf(f, "volume = %d\n", s->volume);
    fprintf(f, "fullscreen = %d\n", s->fullscreen);
    fprintf(f, "scale = %d\n", s->scale);
    fprintf(f, "deadzone = %d\n", s->deadzone);
    fprintf(f, "copy_protection = %d\n", s->copy_protection);
    fprintf(f, "keyboard_mode = %d\n", s->keyboard_mode);
    fprintf(f, "gamedir = %s\n", s->gamedir);
    fclose(f);
    return 1;
}
