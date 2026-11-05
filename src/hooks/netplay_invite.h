/* Invite codes: a lobby's addresses as one short code to send a friend (netplay_invite.c).
 *
 * No server is involved: the code holds the host's port, its public address (when the router
 * opened the port) and its address on its home network, with a checksum, in Crockford base32:
 * "DJ-XXXX-XXXX-XXXX-XXXX-XXXX". Joining with it tries the home-network address first (a
 * friend in the same house), then the public one. */
#ifndef DEFJAM_NETPLAY_INVITE_H
#define DEFJAM_NETPLAY_INVITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_INVITE_LEN      40      /* a code with its dashes, and its end */
#define NETPLAY_INVITE_MAX      2       /* addresses in one code */

/* The code for a lobby on `port` at `public_ip` and `lan_ip` (dotted IPv4, either may be NULL or
 * empty, not both). Returns 1, or 0 when neither address is a dotted IPv4 or `out` is too small. */
int netplay_invite_make(uint16_t port, const char *public_ip, const char *lan_ip, char *out, size_t size);

/* Is `text` an invite code? Its addresses as "a.b.c.d:port", the home-network one first; returns
 * how many (0: not a code, or one with a typo the checksum caught). Case, spaces and dashes do
 * not matter, nor the "DJ" in front; O reads as 0, I and L as 1. */
int netplay_invite_read(const char *text, char out[][32], int max);

#ifdef __cplusplus
}
#endif

#endif
