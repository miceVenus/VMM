#pragma once

#include <stdio.h>

#define GREEN_PREFIX "\x1b[32m"
#define RED_PREFIX "\x1b[31m"
#define DARK_GREEN "\x1b[33m"
#define NORMAL_PREFIX "\x1b[0m"

#define LOG(src, txt, color) { \
    printf("%s%s%s %s\n", color, src, NORMAL_PREFIX, txt); \
}
