#pragma once

#include <stdio.h>

/* Project logging colors using ANSI Select Graphic Rendition escape codes. */
/* ANSI SGR 32: green foreground. */
#define GREEN_PREFIX "\x1b[32m"
/* ANSI SGR 31: red foreground. */
#define RED_PREFIX "\x1b[31m"
/* ANSI SGR 33: yellow foreground; the historical macro name says DARK_GREEN. */
#define DARK_GREEN "\x1b[33m"
/* ANSI SGR 0: reset all terminal text attributes. */
#define NORMAL_PREFIX "\x1b[0m"

/* Project helper that prints a colored "source message" log line and resets
 * the terminal color afterwards. This is a function-like macro, not a value. */
#define LOG(src, txt, color) { \
    printf("%s%s%s %s\n", color, src, NORMAL_PREFIX, txt); \
}
