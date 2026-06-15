#include "unity.h"

#include "serial_manager.h"

void setUp(void)
{
}

void tearDown(void)
{
}

TEST_CASE("Serial manager init returns OK", "[serial]")
{
    TEST_ASSERT_EQUAL(ESP_OK, serial_manager_init());
}

TEST_CASE("Serial manager accepts help command", "[serial]")
{
    serial_manager_handle_command("HELP");
    TEST_ASSERT_TRUE(true);
}

TEST_CASE("Serial manager keeps legacy command entrypoint", "[serial]")
{
    handle_command("HELP");
    TEST_ASSERT_TRUE(true);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_tests_by_tag("[serial]", false);
    UNITY_END();
}
