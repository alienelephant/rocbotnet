// Runs test/test_secure.c under PlatformIO (see ../suite_runner.h).
#define main roclink_suite_main
#include "../../test/test_secure.c"
#undef main
#include "../suite_runner.h"
