#pragma once

#include "sdkconfig.h"
#if CONFIG_WATCH_CLOCK_LAB
#define WATCH_NAME    "Clock lab"
#else
#define WATCH_NAME    "Electric Pocket Watch"
#endif
#define WATCH_VERSION "0.15.10"
