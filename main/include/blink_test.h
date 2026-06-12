#include <stdio.h>
#include "sdkconfig.h"
#include "stdint.h"

void blink_led(void);
 void configure_led(void);

uint8_t s_led_state;