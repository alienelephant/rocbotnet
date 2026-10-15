// Runs test/test_client.c under PlatformIO (see ../suite_runner.h).
#define main roclink_suite_main
#include "../../test/test_client.c"
#undef main
#include "../suite_runner.h"
