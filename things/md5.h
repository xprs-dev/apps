/*
 * md5 -- only because RTSP Digest needs it (RFC 2069/7616).
 *
 * The camera refuses Basic: its 401 offers Digest and nothing else, and a
 * digest is an MD5 of two MD5s. Nothing else in this wapp hashes anything,
 * and nothing here is a security primitive: the core owns every key this
 * device has (docs/architecture.md). This is a checksum a camera asks for.
 */
#ifndef THINGS_MD5_H
#define THINGS_MD5_H

/* [out] receives 32 lowercase hex characters and a null. */
void th_md5_hex(const char *in, unsigned len, char *out);

#endif
