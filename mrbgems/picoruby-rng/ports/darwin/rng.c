/* Apple/Darwin port of picoruby-rng. Implements the include/ port-side ABI
 * (rng_random_byte_impl) used by the gem's src, mirroring ports/posix/rng.c.
 *
 * iOS sandboxes direct open() of /dev/urandom (the posix port's mechanism), so
 * draw entropy from Security.framework's SecRandomCopyBytes instead — the same
 * "POSIX-absent, Apple-native API" justification as picoruby-ble's CoreBluetooth
 * port. Link with -framework Security (set in the example build_config). */
#include <stdint.h>
#include <stdlib.h>
#include <Security/SecRandom.h>

uint8_t
rng_random_byte_impl(void)
{
  uint8_t byte;

  if (SecRandomCopyBytes(kSecRandomDefault, 1, &byte) != errSecSuccess) {
    abort();
  }

  return byte;
}
