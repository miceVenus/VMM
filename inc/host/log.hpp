#pragma once

#include <stdio.h>

/*
 * Standard: ECMA-48/ANSI Select Graphic Rendition (SGR) control sequences;
 * these macros are terminal-output prefixes only and are not part of the VMM
 * protocol or Guest ABI.
 */
/* Standard: SGR 32, set the foreground color to green. */
#define GREEN_PREFIX "\x1b[32m"
/* Standard: SGR 31, set the foreground color to red. */
#define RED_PREFIX "\x1b[31m"
/* Standard: SGR 33, which is actually yellow; DARK_GREEN is a historical project name. */
#define DARK_GREEN "\x1b[33m"
/* Standard: SGR 0, clear/reset terminal display attributes. */
#define NORMAL_PREFIX "\x1b[0m"

/* Project helper macro: prints one log line as color + source + text + color reset. */
#define LOG(src, txt, color) { \
    printf("%s%s%s %s\n", color, src, NORMAL_PREFIX, txt); \
}
