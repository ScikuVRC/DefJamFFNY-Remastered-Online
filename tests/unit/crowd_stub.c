/* Telemetry fixtures run without the crowd's controls (crowd_control.c): nothing to hash. */
#include <stdint.h>
#include "test_observations.h"
uint32_t defjam_crowd_spectator_hash(void) { return 0; }
uint32_t defjam_crowd_background_hash(void) { return 0; }
int defjam_crowd_hash_part(int part, uint32_t *base, uint32_t *bytes) { (void)part; (void)base; (void)bytes; return 0; }
