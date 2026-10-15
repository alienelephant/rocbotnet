// Runs test/test_exchange.c under PlatformIO (see ../suite_runner.h).
#define main roclink_suite_main
#include "../../test/test_exchange.c"
#undef main
#include "../suite_runner.h"
