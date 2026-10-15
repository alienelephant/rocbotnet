/*
 * Runs one of the host test programs from test/ as a PlatformIO Unity test,
 * on the PC (env:native) or on a board (env:test_*). Each suite in this
 * folder includes its test program with main() renamed, then this file:
 *
 *   #define main roclink_suite_main
 *   #include "../../test/test_frame.c"
 *   #undef main
 *   #include "../suite_runner.h"
 *
 * The program's own CHECK lines print as before; Unity reports one pass or
 * fail per suite (a fail means at least one "FAIL file:line" line above).
 */
#ifndef ROCLINK_PIO_SUITE_RUNNER_H
#define ROCLINK_PIO_SUITE_RUNNER_H

#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_roclink_suite(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, roclink_suite_main(), "see FAIL lines above");
}

static int run_suite(void) {
    UNITY_BEGIN();
    RUN_TEST(test_roclink_suite);
    return UNITY_END();
}

#ifdef ARDUINO
void setup() {
    delay(2000); /* let the test runner's serial monitor attach */
    run_suite();
}
void loop() {}
#else
int main(void) { return run_suite(); }
#endif

#endif
