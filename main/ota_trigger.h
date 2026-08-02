#ifndef OTA_TRIGGER_H
#define OTA_TRIGGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configure the OTA trigger GPIO with an internal pull-up, then check whether
 * it remains low for the complete configured hold time.
 *
 * The function returns true only when the GPIO is continuously low for at
 * least two seconds. A high level observed at any point cancels the request.
 * It is intended to run once during startup, before normal application work.
 */
bool ota_trigger_is_held(void);

#ifdef __cplusplus
}
#endif

#endif