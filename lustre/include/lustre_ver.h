#ifndef _LUSTRE_VER_H_
#define _LUSTRE_VER_H_
/* This file automatically generated from lustre/include/lustre_ver.h.in,
 * based on parameters in lustre/autoconf/lustre-version.ac.
 * Changes made directly to this file will be lost. */

#define LUSTRE_MAJOR 2
#define LUSTRE_MINOR 8
#define LUSTRE_PATCH 50
#define LUSTRE_FIX 0
#define LUSTRE_VERSION_STRING "2.8.50"

#define OBD_OCD_VERSION(major, minor, patch, fix)			\
	(((major) << 24) + ((minor) << 16) + ((patch) << 8) + (fix))

#define OBD_OCD_VERSION_MAJOR(version)	((int)((version) >> 24) & 255)
#define OBD_OCD_VERSION_MINOR(version)	((int)((version) >> 16) & 255)
#define OBD_OCD_VERSION_PATCH(version)	((int)((version) >>  8) & 255)
#define OBD_OCD_VERSION_FIX(version)	((int)((version) >>  0) & 255)

#define LUSTRE_VERSION_CODE						\
	OBD_OCD_VERSION(LUSTRE_MAJOR, LUSTRE_MINOR, LUSTRE_PATCH, LUSTRE_FIX)

/* liblustre clients are only allowed to connect if their LUSTRE_FIX mismatches
 * by this amount (set in lustre/autoconf/lustre-version.ac). */
#define LUSTRE_VERSION_ALLOWED_OFFSET OBD_OCD_VERSION(0, 0, 1, 32)

#ifdef __KERNEL__
/* If lustre version of client and servers it connects to differs by more
 * than this amount, client would issue a warning.
 * (set in lustre/autoconf/lustre-version.ac) */
#define LUSTRE_VERSION_OFFSET_WARN OBD_OCD_VERSION(0, 4, 50, 0)
#endif

#endif
