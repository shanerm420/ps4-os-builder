/* status.h - what is actually on this console, read from the files the
 * installer and the payload already use. No graphics, no dialogs: the screens
 * ask this, so a pill cannot say something the disk does not.
 *
 * root is a path prefix for tests on a PC ("" means this console's real root).
 */
#ifndef PS4UI_STATUS_H
#define PS4UI_STATUS_H

#include <stdint.h>

/* The space the installer always leaves free for the PS4 system. main.c's
 * RESERVE_FREE; keep the two in step. */
#define STATUS_KEEP_GB 10ULL

typedef struct ps4_status {
	int linux_installed;
	int linux_on_usb;          /* the manifest says target=external */
	int linux_size_gb;         /* the size chosen at install, 0 if unknown */

	uint64_t free_bytes;       /* free space plus the reserve already made */
	uint64_t keep_bytes;       /* STATUS_KEEP_GB, in bytes */

	int chromeos_found;
	int chromeos_on_usb;       /* chromeos-external.guid is present */
	int chromeos_image;        /* /user/app/chromeos.img is present */
	int chromeos_boot_files;   /* how many of the boot files are present */
	uint64_t chromeos_image_bytes;
	uint64_t chromeos_bytes;   /* image + boot files */
} ps4_status;

void status_read(const char *root, ps4_status *out);

#endif
