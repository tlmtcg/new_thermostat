#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <time.h>

/**
 * @brief Mock de la fonction de récupération du temps local
 * Permet de simuler l'heure et le jour de la semaine pour tester le planning.
 */
struct tm time_utils_get_local_time(void);

/**
 * @brief Fonction utilitaire du mock pour injecter une heure précise dans les tests
 */
void mock_time_utils_set_time(int day_of_week, int hour, int min, int sec);

#endif // TIME_UTILS_H