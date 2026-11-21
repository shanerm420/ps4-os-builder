/* status.c - reads the install state, the free space and the ChromeOS scan.
 *
 * Every path here is one the installer already uses (see installer/main.c):
 * the manifest at /data/ps4linux/manifest.txt, the boot directory at
 * /data/linux/boot, the image at /user/app/ps4linux.img and the ChromeOS image
 * at /user/app/chromeos.img. Nothing is created, moved or deleted - this file
 * only looks.
 */
#include "status.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#else
#include <sys/statvfs.h>
#endif

#define BOOT_DIR "/data/linux/boot"
#define STATE_DIR "/data/ps4linux"
#define IMAGE "/user/app/chromeos.img"
#define ALLOC BOOT_DIR "/ps4linux.img.alloc"

/* The same files installer/main.c's cros_scan() counts, minus the image. */
static const char *const CROS_FILES[] = {
	"/data/linux/boot/chromeos.img.xz",
	"/data/linux/boot/chromeos.img.size",
	"/data/linux/boot/chromeos.img.alloc",
	"/data/linux/boot/chromeos.img.alloc.part",
	"/data/linux/boot/chromeos-install-choice",
	"/data/linux/boot/chromeos-target",
	"/data/linux/boot/chromeos-error.txt",
	"/data/linux/boot/chromeos-external.guid",
	"/data/linux/boot/ps4-layer.sqfs",
	"/data/linux/boot/ps4-chromeos-loader.bin",
	"/data/payloads/ps4-chromeos-loader.bin",
};

/* ---------------------------------------------------------------- files */
static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* open + lseek, the way main.c does it: the sandbox reports sizes this way. */
static uint64_t file_size(const char *path)
{
	int fd = open(path, O_RDONLY);
	off_t end;

	if (fd < 0)
		return 0;
	end = lseek(fd, 0, SEEK_END);
	close(fd);
	return end > 0 ? (uint64_t)end : 0;
}

static int read_small(const char *path, char *buf, size_t cap)
{
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return 0;
}

/* One key=value line out of a manifest / choice file. */
static int field(const char *text, const char *key, char *out, size_t cap)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (*p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1;
			size_t n = 0;

			while (v[n] && v[n] != '\n' && v[n] != '\r' && n + 1 < cap) {
				out[n] = v[n];
				n++;
			}
			out[n] = 0;
			return 1;
		}
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
	}
	return 0;
}

/* ---------------------------------------------------------------- free space */
#if defined(__ORBIS__)
/* FreeBSD statfs(2) = 396, fstatfs(2) = 397, getfsstat(2) = 395. Same reason as
 * installer/main.c: libc's statvfs wrapper does not translate this kernel's
 * struct, and statfs() in the sandbox returns EPERM. */
static long orbis_syscall3(long num, long a1, long a2, long a3)
{
	long ret;
	unsigned char err;

	__asm__ volatile("syscall\n\tsetc %1"
	                 : "=a"(ret), "=r"(err)
	                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
	                 : "rcx", "r11", "memory");
	return err ? -ret : ret;
}

#define STATFS_SIZE 472

static int fs_parse(const unsigned char *b, uint64_t *free_b)
{
	uint64_t bsize, blocks;
	int64_t bavail;

	memcpy(&bsize, b + 16, 8);
	memcpy(&blocks, b + 32, 8);
	memcpy(&bavail, b + 48, 8);
	if (bavail < 0)
		bavail = 0;
	if (bsize == 0 || blocks == 0)
		return -1;
	*free_b = (uint64_t)bavail * bsize;
	return 0;
}

static uint64_t free_bytes(const char *path)
{
	static uint64_t sf[64];
	uint64_t f = 0;
	int fd;

	memset(sf, 0, sizeof(sf));
	if (orbis_syscall3(396, (long)path, (long)sf, 0) >= 0 &&
	    fs_parse((unsigned char *)sf, &f) == 0)
		return f;

	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		memset(sf, 0, sizeof(sf));
		long r = orbis_syscall3(397, (long)fd, (long)sf, 0);
		close(fd);
		if (r >= 0 && fs_parse((unsigned char *)sf, &f) == 0)
			return f;
	}
	return 0;
}
#else
static uint64_t free_bytes(const char *path)
{
	struct statvfs vfs;

	if (statvfs(path, &vfs) != 0)
		return 0;
	return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}
#endif

/* ---------------------------------------------------------------- reading */
static void join(char *dst, size_t cap, const char *root, const char *path)
{
	snprintf(dst, cap, "%s%s", root, path);
}

void status_read(const char *root, ps4_status *s)
{
	char buf[1024];
	char tmp[512];
	char value[64];
	int i;

	memset(s, 0, sizeof(*s));
	s->keep_bytes = STATUS_KEEP_GB << 30;

	/* ---- Linux: the manifest the installer writes, exactly as main.c reads it */
	join(tmp, sizeof(tmp), root, STATE_DIR "/manifest.txt");
	if (read_small(tmp, buf, sizeof(buf)) == 0) {
		if (field(buf, "target", value, sizeof(value)))
			s->linux_on_usb = !strcmp(value, "external");
		if (field(buf, "state_gib", value, sizeof(value)))
			s->linux_size_gb = atoi(value);
	}

	join(tmp, sizeof(tmp), root, BOOT_DIR "/bzImage");
	s->linux_installed = exists(tmp);
	if (s->linux_installed) {
		join(tmp, sizeof(tmp), root, STATE_DIR "/manifest.txt");
		if (!exists(tmp)) {
			/* The payload's own target file, written beside the kernel. */
			join(tmp, sizeof(tmp), root, BOOT_DIR "/ps4linux-target");
			if (read_small(tmp, buf, sizeof(buf)) == 0 &&
			    field(buf, "target", value, sizeof(value)))
				s->linux_on_usb = !strcmp(value, "external");
		}
	}

	/* ---- free space, counted the way the installer offers it: what is free
	 *      plus the reserve already made, minus the space always kept back. */
	join(tmp, sizeof(tmp), root, BOOT_DIR);
	s->free_bytes = free_bytes(tmp);
	join(tmp, sizeof(tmp), root, ALLOC);
	s->free_bytes += file_size(tmp);
	if (s->free_bytes > s->keep_bytes)
		s->free_bytes -= s->keep_bytes;
	else
		s->free_bytes = 0;

	/* ---- ChromeOS: the image, plus the boot files cros_scan() counts */
	join(tmp, sizeof(tmp), root, IMAGE);
	{
		uint64_t image = file_size(tmp);

		s->chromeos_image = image > 0;
		s->chromeos_image_bytes = image;
		s->chromeos_bytes = image;
	}

	for (i = 0; i < (int)(sizeof(CROS_FILES) / sizeof(CROS_FILES[0])); i++) {
		uint64_t sz;

		join(tmp, sizeof(tmp), root, CROS_FILES[i]);
		sz = file_size(tmp);
		if (!sz)
			continue;
		s->chromeos_boot_files++;
		s->chromeos_bytes += sz;
	}

	join(tmp, sizeof(tmp), root, BOOT_DIR "/chromeos-external.guid");
	s->chromeos_on_usb = exists(tmp);
	s->chromeos_found = s->chromeos_image || s->chromeos_boot_files > 0;
}
