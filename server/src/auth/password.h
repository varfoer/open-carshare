#ifndef OCS_AUTH_PASSWORD_H
#define OCS_AUTH_PASSWORD_H

#include <stdbool.h>

/* Argon2id, PHC string: $argon2id$v=19$m=19456,t=2,p=1$<b64salt>$<b64hash>.
 * Returns 0 ok, -1 on malloc or random failure. */
int pw_hash(const char *pw, char out[160]);
/* Constant-time compare; malformed or out-of-bounds params -> false. */
bool pw_verify(const char *pw, const char *encoded);

#endif
