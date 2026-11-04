/* Fixtures run with lockstep play off (lockstep.c): no shared state, and, as there, only a
 * test's RECOMP_TEST_RNG_SEED pins a seed, while the test observations are on. */
#include <stdint.h>
#include <stdlib.h>
#include "lockstep.h"
#include "test_observations.h"
int defjam_lockstep_enabled(void) { return 0; }
int defjam_pinned_seed(uint32_t *seed)
{
    const char *text = getenv("RECOMP_TEST_RNG_SEED");
    if (!defjam_test_observations_enabled() || !text || !*text) return 0;
    *seed = (uint32_t)strtoul(text, NULL, 0);
    return 1;
}
int defjam_pinned_increment(uint32_t *increment) { (void)increment; return 0; }
uint32_t defjam_lockstep_utility_state(void) { return 0; }
void defjam_lockstep_shared_seed(void) {}
