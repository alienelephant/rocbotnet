// Runs test/test_frame.c under PlatformIO (see ../suite_runner.h).
#define main roclink_suite_main
#include "../../test/test_frame.c"
#undef main
#include "../suite_runner.h"
