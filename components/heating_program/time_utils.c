#include "time_utils.h"

// Variables statiques pour contrôler le temps simulé
static struct tm mock_time = {
    .tm_wday = 1, // Lundi par défaut (0 = Dimanche, 1 = Lundi...)
    .tm_hour = 12,
    .tm_min = 0,
    .tm_sec = 0,
    .tm_mday = 1,
    .tm_mon = 0,  // Janvier
    .tm_year = 126 // 2026 (années depuis 1900)
};

struct tm time_utils_get_local_time(void)
{
    return mock_time;
}

void mock_time_utils_set_time(int day_of_week, int hour, int min, int sec)
{
    mock_time.tm_wday = day_of_week;
    mock_time.tm_hour = hour;
    mock_time.tm_min = min;
    mock_time.tm_sec = sec;
}
