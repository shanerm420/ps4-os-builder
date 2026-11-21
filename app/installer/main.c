// PS4 Linux - the app that installs Linux and then starts it, every time
// (OpenOrbis homebrew, needs GoldHEN + BinLoader). It is not called an installer
// because installing is only the first thing it does: after that it is the way in.
//
// Flow, in the order the user sees it:
//
//   1. find the package's files
//   2. look for an existing install (ours, or ChromeOS for PS4)
//   3. deal with an autoload entry: the boot side's note, then anything armed
//   4. the menu, one entry per category, each with its own submenu:
//        installed:     Start Linux | Graphics memory | Autoload |
//                       Remove ChromeOS | Recovery
//        not installed: Install Linux | Autoload | Remove ChromeOS | Recovery
//   5. installing only: "Before you begin" briefing (Cancel on every page), where
//      (PS4 drive or USB), how much space, copy files, write vram.txt at 1 GB,
//      reserve space from OrbisOS, wait for the drive to settle, confirm, start
//
// The loader reads the graphics memory from vram.txt next to the bzImage and
// ignores the payload's own size when the file is there, so that setting is a
// menu choice here and never a file edit on the console.
//
// Hard rules encoded here:
//   * this app deletes nothing that existed before it ran, with one exception the
//     user asks for by name: a payload autoload entry (autoload_offer_remove),
//     which it lists first and only removes from the known paths. Its unlink()
//     calls otherwise only remove temporary files it created itself (.part, its
//     probe, its own reserve). Removing a ChromeOS install is PS4LNX002's job;
//   * shared boot files (bzImage, initramfs.cpio.gz) are backed up before we
//     write ours, because the loader's search path is fixed (see docs/LAYOUT.md);
//   * this app never writes, registers or modifies any payload autoload config;
//   * Cancel at any point before step 7 changes nothing.
//
// Builds with the OpenOrbis toolchain (OO_PS4_TOOLCHAIN); see the repository
// README for the command and for what remains unverified. Nothing here has run
// on a PS4 yet.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <orbis/Net.h>

#include "appui.h"
#include "status.h"
#include "ui.h"

#define APP_NAME "PS4 XFCE Linux"

// The screens, when they can start. ui_ready == 0 keeps every dialog path in
// this file exactly as it was, so a console where SDL, the packaged fonts or a
// pad are missing still gets a working app.
static int ui_ready;
static ui_state uist;
static int ui_progress;

// ------------------------------------------------------------------ paths
// The loader's search order is fixed by upstream (USB first, then
// /data/linux/boot, then /user/system/boot). We cannot namespace our way out of
// sharing /data/linux/boot, so we own it explicitly: back up anything we find
// there, record it in a manifest, and offer to restore on uninstall.
static const char *src_candidates[] = {
		"/app0/assets/ps4linux",
		"/mnt/sandbox/pfsmnt/PS4LNX001-app0/assets/ps4linux",
		"/mnt/sandbox/PS4LNX001_000/app0/assets/ps4linux",
};
static char SRC[128];
#define BOOT_DIR "/data/linux/boot"
#define STATE_DIR "/data/ps4linux"
#define MANIFEST STATE_DIR "/manifest.txt"
#define IMAGE "/user/app/ps4linux.img"
#define ALLOC BOOT_DIR "/ps4linux.img.alloc"
#define USB "/mnt/usb0"
#define USB_MARKER USB "/PS4LINUX-TARGET.txt"
#define GB (1024ULL * 1024ULL * 1024ULL)
#define RESERVE_FREE (10ULL * GB)

static const char *boot_files[] = {
	"bzImage", "initramfs.cpio.gz", "ps4linux-loader.bin", "rootfs.tar.zst",
};
#define N_BOOT_FILES (sizeof(boot_files) / sizeof(boot_files[0]))

// Graphics memory. The PS4 has one 8 GB pool shared by the CPU and the GPU: the
// payload hands N MB to the graphics chip as VRAM and the kernel gets the rest.
// The loader takes the number from vram.txt (plain text, in MB) sitting next to
// the bzImage, and a vram.txt beats whichever payload was launched - which is
// what makes this a setting instead of a choice of four different payload files.
#define VRAM_FILE BOOT_DIR "/vram.txt"
#define VRAM_FIRST 1024u	// install and first start need the RAM, not the GPU
// The sizes live with the screen that offers them (ui/ui.c), so the dialog path
// and the screen cannot drift apart: 1, 2, 3, 4, 5 and 6 GB.
#define vram_sizes ui_vram_sizes
#define N_VRAM ui_vram_count

// ------------------------------------------------------------------ logging
static void klog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
	buf[n] = '\n';
	buf[n + 1] = 0;
	sceKernelDebugOutText(0, "[ps4linux] ");
	sceKernelDebugOutText(0, buf);
}

// ------------------------------------------------------------------ dialogs
static void dialog_param_init(OrbisMsgDialogParam *p)
{
	memset(p, 0, sizeof(*p));
	p->baseParam.size = sizeof(OrbisCommonDialogBaseParam);
	p->baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&p->baseParam);
	p->size = sizeof(*p);
}

// 1 = first button, 2 = second button, 0 = cancelled/closed.
static int dialog(OrbisMsgDialogButtonType type, const char *b1, const char *b2,
                  const char *fmt, ...)
{
	static char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	OrbisMsgDialogParam p;
	OrbisMsgDialogUserMessageParam um;
	OrbisMsgDialogButtonsParam bp;
	OrbisMsgDialogResult r;
	dialog_param_init(&p);
	memset(&um, 0, sizeof(um));
	memset(&bp, 0, sizeof(bp));
	p.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
	um.buttonType = type;
	um.msg = msg;
	if (type == ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS) {
		bp.msg1 = b1;
		bp.msg2 = b2;
		um.buttonsParam = &bp;
	}
	p.userMsgParam = &um;
	sceMsgDialogInitialize();
	if (sceMsgDialogOpen(&p) < 0) {
		sceMsgDialogTerminate();
		return 0;
	}
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
		sceKernelUsleep(10000);
	memset(&r, 0, sizeof(r));
	sceMsgDialogGetResult(&r);
	sceMsgDialogTerminate();
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON1) return 1;
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON2) return 2;
	return 0;
}

static int progress_open(const char *msg)
{
	if (ui_ready) {
		ui_progress = 1;
		uist.percent = 0;
		uist.step = 0;
		uist.written_gb = 0;
		uist.minutes_left = 0;
		appui_progress(&uist);
		return 0;
	}
	OrbisMsgDialogParam p;
	OrbisMsgDialogProgressBarParam pb;
	dialog_param_init(&p);
	memset(&pb, 0, sizeof(pb));
	p.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;
	pb.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
	pb.msg = msg;
	p.progBarParam = &pb;
	sceMsgDialogInitialize();
	return sceMsgDialogOpen(&p);
}

static void progress_set(uint32_t pct, const char *msg)
{
	if (ui_progress) {
		uist.percent = (int)pct;
		uist.step = pct < 15 ? 0 : pct < 35 ? 1 : pct < 90 ? 2 : 3;
		appui_progress(&uist);
		return;
	}
	if (pct > 100) pct = 100;	// an adopted loader can push past the estimate
	sceMsgDialogProgressBarSetValue(0, pct);
	if (msg) sceMsgDialogProgressBarSetMsg(0, msg);
	sceMsgDialogUpdateStatus();
}

static void progress_close(void)
{
	if (ui_progress) {
		ui_progress = 0;
		return;
	}
	sceMsgDialogClose();
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
		sceKernelUsleep(10000);
	sceMsgDialogTerminate();
}

static void quit(void)
{
	sceSystemServiceLoadExec("exit", NULL);
	for (;;) sceKernelUsleep(1000000);
}

// ------------------------------------------------------------------ files
static uint64_t fd_size(int fd)
{
	off_t cur = lseek(fd, 0, SEEK_CUR);
	off_t end = lseek(fd, 0, SEEK_END);
	if (cur >= 0) lseek(fd, cur, SEEK_SET);
	return end > 0 ? (uint64_t)end : 0;
}

static uint64_t file_size(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	uint64_t n = fd_size(fd);
	close(fd);
	return n;
}

static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void mkdirs(const char *path)
{
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

static int write_small(const char *path, const char *text)
{
	char part[288];
	snprintf(part, sizeof(part), "%s.part", path);
	int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return -1;
	size_t n = strlen(text);
	ssize_t w = write(fd, text, n);
	fsync(fd);
	close(fd);
	if (w != (ssize_t)n || rename(part, path) != 0) {
		unlink(part);
		return -1;
	}
	chmod(path, 0666);
	return 0;
}

static int read_small(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	return 0;
}

// The graphics memory currently in effect, in MB. Anything unreadable or out of
// the loader's documented range falls back to the first-start value.
static uint32_t vram_current(void)
{
	char b[32];
	if (read_small(VRAM_FILE, b, sizeof(b)) != 0) return VRAM_FIRST;
	unsigned long v = strtoul(b, NULL, 10);
	if (v < 32 || v > 4096) return VRAM_FIRST;
	return (uint32_t)v;
}

static int write_vram(uint32_t mb)
{
	char b[16];
	snprintf(b, sizeof(b), "%u\n", mb);
	return write_small(VRAM_FILE, b);
}

// Re-reads what is on the console before a screen draws it, so a pill never
// shows a state from before the last action.
static void ui_refresh(void)
{
	ps4_status s;

	status_read("", &s);
	ui_state_from_status(&uist, &s);
	uist.vram_mb = (int)vram_current();
}

// ------------------------------------------------------------------ free space
// FreeBSD statfs(2) = 396, fstatfs(2) = 397, getfsstat(2) = 395. Called
// directly: libc's statvfs wrapper does not translate this kernel's struct
// layout, and inside the app sandbox statfs() returns EPERM (see the reference
// project's notes; same constraint applies here).
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
	if (bavail < 0) bavail = 0;
	if (bsize == 0 || blocks == 0) return -1;
	*free_b = (uint64_t)bavail * bsize;
	return 0;
}

static uint64_t free_bytes(const char *path)
{
	static uint64_t sf[64];
	static unsigned char all[64 * STATFS_SIZE];
	uint64_t f = 0;

	memset(sf, 0, sizeof(sf));
	if (orbis_syscall3(396, (long)path, (long)sf, 0) >= 0 &&
	    fs_parse((unsigned char *)sf, &f) == 0)
		return f;

	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		memset(sf, 0, sizeof(sf));
		long r = orbis_syscall3(397, (long)fd, (long)sf, 0);
		close(fd);
		if (r >= 0 && fs_parse((unsigned char *)sf, &f) == 0)
			return f;
	}

	memset(all, 0, sizeof(all));
	long r = orbis_syscall3(395, (long)all, (long)sizeof(all), 2);
	if (r < 0) return 0;
	for (long i = 0; i < r && i < 64; i++) {
		const char *on = (const char *)all + i * STATFS_SIZE + 384;
		if (!strcmp(on, "/user") || !strcmp(on, "/data"))
			if (fs_parse(all + i * STATFS_SIZE, &f) == 0)
				return f;
	}
	return 0;
}

// GoldHEN SDK syscall 500: leave the sandbox only to read free space, then
// return before booting (a boot straight out of a jailbroken process had no
// HDMI and no LED change on a tested console).
#define GOLDHEN_JAILBREAK 2
#define GOLDHEN_UNJAILBREAK 3
struct jailbreak_backup {
	uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
	uint64_t cr_paid;
	uint64_t cr_caps[2];
	void *cr_prison, *fd_cdir, *fd_jdir, *fd_rdir;
	uint8_t spare[128];
};
static struct jailbreak_backup jb;
static int jailbroken;

static int goldhen_jailbreak(void)
{
	if (jailbroken) return 0;
	memset(&jb, 0, sizeof(jb));
	if (orbis_syscall3(500, GOLDHEN_JAILBREAK, (long)&jb, 0) != 0) return -1;
	jailbroken = 1;
	return 0;
}

static void goldhen_unjailbreak(void)
{
	if (!jailbroken) return;
	if (orbis_syscall3(500, GOLDHEN_UNJAILBREAK, (long)&jb, 0) == 0)
		jailbroken = 0;
	sync();
	sceKernelUsleep(5 * 1000000);
}

// ------------------------------------------------------------------ detection
// Our own install: manifest + build-id. ChromeOS for PS4: image + named files.
// Deliberately identical to tools/chromeos-scan.c, which is the tested
// reference for these rules.
struct cros_found {
	int image;
	uint64_t image_size;
	int n_boot;
	uint64_t boot_bytes;
	char listing[1024];
};

static const char *k_cros_files[] = {
	BOOT_DIR "/chromeos.img.xz",
	BOOT_DIR "/chromeos.img.size",
	BOOT_DIR "/chromeos.img.alloc",
	BOOT_DIR "/chromeos.img.alloc.part",
	BOOT_DIR "/chromeos-install-choice",
	BOOT_DIR "/chromeos-target",
	BOOT_DIR "/chromeos-error.txt",
	BOOT_DIR "/chromeos-external.guid",
	BOOT_DIR "/ps4-layer.sqfs",
	BOOT_DIR "/ps4-chromeos-loader.bin",
	"/data/payloads/ps4-chromeos-loader.bin",
	NULL,
};

static void cros_scan(struct cros_found *c)
{
	memset(c, 0, sizeof(*c));
	c->image_size = file_size("/user/app/chromeos.img");
	c->image = c->image_size > 0;
	for (int i = 0; k_cros_files[i]; i++) {
		uint64_t sz = file_size(k_cros_files[i]);
		if (!sz) continue;
		c->n_boot++;
		c->boot_bytes += sz;
		size_t used = strlen(c->listing);
		snprintf(c->listing + used, sizeof(c->listing) - used, "  %s (%llu MB)\n",
		         k_cros_files[i], (unsigned long long)(sz >> 20));
	}
}

// Deleting a ChromeOS install is NOT done here. It lives in the remover package
// (installer/remove.c, TITLE_ID PS4LNX002), which is the only file in the project
// that removes a user's files. Keeping it out of the installer means "install Linux" can
// never erase an existing install, whatever the dialogs say or do.
//
// cros_scan() stays: the installer still needs to know whether one is there, so
// it can tell the user to run the remover first if they want the space back.

// Autoload check. We never write these; we only warn if something else did.
static const char *k_autoload[] = {
	"/data/GoldHEN/autoload",
	"/data/GoldHEN/autoload.txt",
	"/data/bdj_autoload.cfg",
	"/data/payloads/autoload.txt",
	NULL,
};

// Removing an autoload entry is the ONE thing this app deletes, and only after
// the user has read the paths and asked for it. It is here because of the loop:
// an armed entry fires every time GoldHEN loads, so the console never reaches a
// state where anything else can run. When it is NOT firing - the payload it names
// is gone, or the entry points at a file that no longer exists - this app does
// run, and this is where the mess gets cleaned up without FTP and without a
// second enabler.
static int remove_path(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) return 0;	// already gone
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *e;
			char p[320];
			while ((e = readdir(d)) != NULL) {
				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
				snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
				unlink(p);
			}
			closedir(d);
		}
		return rmdir(path) == 0 ? 1 : -1;
	}
	return unlink(path) == 0 ? 1 : -1;
}

// Everything that is armed, one per line: the four list files, and whatever sits
// inside GoldHEN's autoload folder. Returns how many were found.
static int autoload_listing(char *buf, size_t cap)
{
	size_t used = 0;
	int n = 0;
	buf[0] = 0;
	for (int i = 0; k_autoload[i]; i++) {
		if (!exists(k_autoload[i])) continue;
		int add = snprintf(buf + used, cap - used, "  %s\n", k_autoload[i]);
		if (add > 0 && (size_t)add < cap - used) used += (size_t)add;
		n++;
	}
	DIR *d = opendir("/data/GoldHEN/autoload");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			int add = snprintf(buf + used, cap - used,
			                   "  /data/GoldHEN/autoload/%s\n", e->d_name);
			if (add > 0 && (size_t)add < cap - used) used += (size_t)add;
			n++;
		}
		closedir(d);
	}
	return n;
}

// Deletes exactly what autoload_listing() shows. Returns 1 if anything went.
static int autoload_remove_all(void)
{
	int gone = 0, failed = 0;
	for (int i = 0; k_autoload[i]; i++) {
		if (!exists(k_autoload[i])) continue;
		if (remove_path(k_autoload[i]) == 1) {
			gone++;
			klog("removed autoload entry %s", k_autoload[i]);
		} else {
			failed++;
			klog("could not remove %s (errno %d)", k_autoload[i], errno);
		}
	}
	if (failed)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Removed %d autoload file(s); %d could not be removed.\n\n"
		       "If the console still goes into Linux by itself, the entry is "
		       "somewhere else: check GoldHEN's own settings screen, and "
		       "docs/RECOVERY.md.", gone, failed);
	else if (gone)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Removed %d autoload file(s).\n\nGoldHEN loads normally from now on, "
		       "and Linux starts only from this app.", gone);
	return gone;
}

// Lists what is armed and, if the user says so, removes it. The only deletion
// this app performs, and only after the paths have been shown.
static int autoload_offer_remove(void)
{
	char listing[768];
	int n = autoload_listing(listing, sizeof(listing));
	if (!n) return 0;

	int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Remove it", "Keep it",
	               "A payload autoload file was found:\n%s\n"
	               "This app never creates one and never writes one. An autoloaded "
	               "Linux payload makes the console restart in a loop every time "
	               "GoldHEN loads.\n\n"
	               "Remove it now? Only the file(s) listed above are deleted. "
	               "Nothing else on the console is touched.", listing);
	if (r != 1) return 0;
	return autoload_remove_all();
}

// What a Linux boot should do about an autoload entry. The initramfs has no UI to
// ask with, so the choice is made here and stored next to the kernel, where the
// boot function reads it:
//   ask    - leave it, leave a note, and let this app ask next launch (default)
//   remove - delete it during boot: a console that reaches Linux cannot loop again
//   keep   - never touch it
#define AUTOLOAD_POLICY BOOT_DIR "/autoload-policy"
#define AUTOLOAD_NOTE STATE_DIR "/autoload-seen"

static const char *autoload_policy(void)
{
	static char b[16];
	if (read_small(AUTOLOAD_POLICY, b, sizeof(b)) != 0) return "ask";
	if (strstr(b, "remove")) return "remove";
	if (strstr(b, "keep")) return "keep";
	return "ask";
}

static void set_autoload_policy(const char *v)
{
	char b[16];
	snprintf(b, sizeof(b), "%s\n", v);
	if (write_small(AUTOLOAD_POLICY, b) != 0)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Could not write %s (error %d).", AUTOLOAD_POLICY, errno);
	else
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Boot behaviour is now: %s", v);
}

// The note the boot side leaves when the policy is "ask": Linux was started by an
// autoload entry, which is the situation that turns into a loop.
static void autoload_note_prompt(void)
{
	if (!exists(AUTOLOAD_NOTE)) return;
	unlink(AUTOLOAD_NOTE);
	autoload_offer_remove();
}

// ------------------------------------------------------------------ menu
// The dialog API has two buttons and no list control, so a menu is one entry per
// screen: button 1 opens the entry shown, button 2 shows the next one.
static int menu(const char *title, const char *const *items, int n, int start)
{
	int i = start % n;
	for (;;) {
		int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Select", "Next",
		               "%s\n\n        > %s\n\n\"Select\" opens this. \"Next\" shows "
		               "the other entries.", title, items[i]);
		if (r == 0) return -1;
		if (r == 1) return i;
		i = (i + 1) % n;
	}
}

// ------------------------------------------------------------------ briefing
// Returns 1 to continue, 0 if the user cancelled. Cancel on any page stops.
static int briefing(void)
{
	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           APP_NAME " installs a complete Linux desktop (Debian with XFCE) on this "
	           "console.\n\n"
	           "Linux runs INSTEAD of the PS4 system until you restart. Your games, "
	           "saves and the PS4 system itself are not modified.\n\n"
	           "This is not an official Sony feature. Nothing here is supported by "
	           "Sony, and a jailbroken console can always be left in a state that "
	           "needs recovery.\n\n"
	           "Page 1 of 6") != 1) return 0;

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           "WHERE LINUX LIVES\n\n"
	           "PS4 drive: Linux is stored in one large file on the PS4 drive. Games "
	           "and saves stay. The space you choose becomes invisible to the PS4, "
	           "and %llu GB always stay free.\n\n"
	           "USB drive: the drive is ERASED completely and Linux lives on it. "
	           "Faster on an SSD, and your PS4 drive is untouched.\n\n"
	           "Page 2 of 6", (unsigned long long)(RESERVE_FREE / GB)) != 1) return 0;

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           "WHAT WILL HAPPEN\n\n"
	           "1. Files are copied to the PS4 drive (about 1 minute).\n"
	           "2. Space is reserved at once, at full drive speed.\n"
	           "3. Linux is installed into that space. The LED turns orange.\n"
	           "4. The console restarts into Linux.\n\n"
	           "The first install takes roughly 15 to 40 minutes depending on the "
	           "drive. Later starts take seconds.\n\n"
	           "Page 3 of 6") != 1) return 0;

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           "DO NOT POWER OFF\n\n"
	           "Do not switch off, unplug or reset the console while:\n"
	           "  - space is being reserved,\n"
	           "  - it says \"Finishing writing to the PS4 drive\",\n"
	           "  - the LED is orange (installing).\n\n"
	           "Interrupting those steps does not kill the console, but it corrupts "
	           "the Linux install and you have to start it again.\n\n"
	           "LED: white = PS4 system, orange = Linux working, off = off.\n\n"
	           "Page 4 of 6") != 1) return 0;

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           "STARTING AND RETURNING\n\n"
	           "To start Linux later: launch this app again and confirm.\n"
	           "To return to the PS4: use Restart inside Linux.\n\n"
	           "Linux NEVER starts by itself. This app does not change any GoldHEN "
	           "startup setting and never adds a payload to an autoload folder.\n\n"
	           "If you ever enabled \"autoload payloads\" in GoldHEN yourself, turn it "
	           "off: a Linux payload that autoloads makes the console loop, because "
	           "every jailbreak immediately restarts it again.\n\n"
	           "Page 5 of 6") != 1) return 0;

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	           "IF SOMETHING GOES WRONG\n\n"
	           "Nothing to worry about: hold the power button until the console turns "
	           "off, then start it normally. The PS4 system is not modified by this "
	           "installer.\n\n"
	           "A USB stick with a file named ps4linux-recovery in its root forces a "
	           "Linux rescue shell on the next start.\n\n"
	           "Full recovery instructions, including the boot-loop case, are in "
	           "docs/RECOVERY.md in the release.\n\n"
	           "Page 6 of 6") != 1) return 0;

	return 1;
}

// ------------------------------------------------------------------ storage
static uint64_t img_bytes(uint64_t start, uint64_t gib)
{
	return (start * 512 + gib * GB + 34 * 512 + 1048575) / 1048576 * 1048576;
}

// Returns the STATE size in GiB, or 0 for USB. Quits on cancel.
static uint64_t ask_install(uint64_t start, uint64_t avail)
{
	int r;
	if (ui_ready) {
		int d;

		ui_refresh();
		d = appui_drive_choice(&uist);
		r = d < 0 ? 0 : (d == 0 ? 1 : 2);
	} else {
		r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "PS4 drive", "USB drive",
		           "Where should Linux be installed?\n\n"
		           "PS4 drive: games and saves stay. The space you choose becomes "
		           "invisible to the PS4.\n\n"
		           "USB drive: that drive is ERASED. You pick it on the TV after the "
		           "restart (USB keyboard needed).");
	}
	if (r == 0) quit();
	if (r == 2) return 0;

	uint64_t max_gib = 0;
	while (img_bytes(start, max_gib + 1) <= avail) max_gib++;
	if (max_gib < 8) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Not enough free space: Linux needs at least 8 GB plus %llu GB that "
		       "always stay free.\n\nFree up space, or install to a USB drive.",
		       (unsigned long long)(RESERVE_FREE / GB));
		quit();
	}

	static const uint64_t steps[] = { 32, 64, 128, 256 };
	uint64_t sizes[8];
	int n = 0;
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
		if (steps[i] <= max_gib) sizes[n++] = steps[i];
	if (n == 0 || sizes[n - 1] != max_gib) sizes[n++] = max_gib;

	for (int i = 0;; i = (i + 1) % n) {
		uint64_t g = sizes[i];
		const char *tag = g == 32 ? "  (recommended)"
		                        : (g == max_gib ? "  (all the free space)" : "");
		r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Install",
		           n > 1 ? "Next size" : "Install",
		           "Space for Linux (system, applications, your files):\n\n"
		           "        %llu GB%s\n\n"
		           "%llu GB of the PS4 drive always stay free for the PS4 system.%s",
		           (unsigned long long)g, tag, (unsigned long long)(RESERVE_FREE / GB),
		           n > 1 ? "\n\n\"Next size\" shows the other sizes." : "");
		if (r == 0) quit();
		if (r == 1 || n == 1) return g;
	}
}

// Returns the size the user picked, in MB. Quits on cancel. Two buttons that
// cycle, the same way the install size works: the dialog API has no list control.
static uint32_t ask_vram(uint32_t current)
{
	for (int i = 0;; i = (i + 1) % N_VRAM) {
		uint32_t mb = vram_sizes[i];
		const char *tag = mb == VRAM_FIRST ? "  (recommended for the first start)"
		                : mb == 2048 ? "  (usual desktop choice)"
		                : mb == 4096 ? "  (leaves 4 GB for programs)"
		                : mb == 5120 ? "  (leaves 3 GB for programs)"
		                : mb == 6144 ? "  (leaves 2 GB for programs)" : "";
		const char *now = mb == current ? "  <- current" : "";
		int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Use this size", "Next size",
		               "Graphics memory (VRAM): %u MB%s%s\n\n"
		               "The console has one 8 GB pool for both the CPU and the GPU. "
		               "%u MB goes to the graphics chip and %u MB stays as system "
		               "memory.\n\n"
		               "Installing and the first start need that memory more than the "
		               "GPU does: keep 1024 MB until Linux is up, then 2048 MB for a "
		               "desktop.\n\n"
		               "\"Next size\" shows the other sizes.",
		               mb, tag, now, mb, 8192u - mb);
		if (r == 0) quit();
		if (r == 1) return mb;
	}
}

// ------------------------------------------------------------------ submenus
static void vram_menu(void)
{
	static const char *const items[] = { "Show current size", "Change it", "Back" };

	if (ui_ready) {
		int picked;

		ui_refresh();
		picked = appui_graphics_memory(&uist);
		if (picked > 0) {
			if (write_vram((uint32_t)picked) != 0)
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "Could not write %s (error %d).", VRAM_FILE, errno);
			uist.vram_mb = picked;
		}
		return;
	}
	for (;;) {
		int m = menu("Graphics memory", items, 3, 0);
		if (m < 0 || m == 2) return;
		uint32_t v = vram_current();
		if (m == 0) {
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Graphics memory (VRAM): %u MB\n\n"
			       "That much of the 8 GB pool goes to the graphics chip; %u MB "
			       "stays as system memory. It applies to the next start.",
			       v, 8192u - v);
			continue;
		}
		v = ask_vram(v);
		if (write_vram(v) != 0)
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Could not write %s (error %d).", VRAM_FILE, errno);
		else
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Graphics memory is now %u MB.", v);
	}
}

static void autoload_menu(void)
{
	static const char *const items[] = {
		"Check what is armed", "Remove the entry", "Boot behaviour", "Back",
	};
	for (;;) {
		int m = menu("Autoload", items, 4, 0);
		if (m < 0 || m == 3) return;
		if (m == 0) {
			char listing[768];
			int n = autoload_listing(listing, sizeof(listing));
			if (!n)
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "Nothing is armed.\n\nNo GoldHEN autoload entry was found, so "
				       "the console cannot start Linux by itself.");
			else
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "Found %d autoload file(s):\n%s\nThese are what make GoldHEN "
				       "start Linux by itself, and an entry that fires on every "
				       "jailbreak is how consoles end up looping.", n, listing);
			continue;
		}
		if (m == 1) {
			if (!autoload_offer_remove())
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "Nothing was removed.");
			continue;
		}
		// Boot behaviour: ask / remove / keep, cycling.
		const char *cur = autoload_policy();
		const char *next = !strcmp(cur, "ask") ? "remove"
		                 : !strcmp(cur, "remove") ? "keep" : "ask";
		int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Change it", "Back",
		               "What a Linux start should do about an autoload entry:\n\n"
		               "  ask    - leave it, then ask me here next time\n"
		               "  remove - delete it during the boot, so the console cannot "
		               "loop again\n"
		               "  keep   - never touch it\n\n"
		               "Current: %s\n\n\"Change it\" sets: %s", cur, next);
		if (r == 1) set_autoload_policy(next);
	}
}

static void cros_menu(const struct cros_found *c)
{
	static const char *const items[] = { "What was found", "Back" };
	for (;;) {
		int m = menu("Remove ChromeOS", items, 2, 0);
		if (m < 0 || m == 1) return;
		if (!c->image && !c->n_boot)
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "No ChromeOS for PS4 installation was found.");
		else
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Image: %llu MB\nOther files: %d (%llu MB)\n\n"
			       "Removing it is the job of the separate app \"PS4 Linux: Remove "
			       "ChromeOS\": it lists every file with its size and asks twice. "
			       "This app never deletes it, so installing Linux can never erase "
			       "an existing install.",
			       (unsigned long long)(c->image_size >> 20), c->n_boot,
			       (unsigned long long)(c->boot_bytes >> 20));
	}
}

static void recovery_menu(void)
{
	static const char *const items[] = {
		"Rescue flag on a USB stick", "After a boot loop", "Back",
	};
	for (;;) {
		int m = menu("Recovery", items, 3, 0);
		if (m < 0 || m == 2) return;
		if (m == 0) {
			static const char *const usb[] = { "/mnt/usb0", "/mnt/usb1", "/mnt/usb2" };
			int ok = 0;
			for (size_t i = 0; i < sizeof(usb) / sizeof(usb[0]); i++) {
				char p[64];
				snprintf(p, sizeof(p), "%s/ps4linux-recovery", usb[i]);
				if (!exists(usb[i])) continue;
				if (write_small(p, "ps4linux recovery\n") == 0) {
					ok++;
					klog("rescue flag written to %s", p);
				}
			}
			if (ok)
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "The flag is on the stick.\n\nStart Linux with it plugged in and "
				       "the boot stops at a rescue shell before touching the image.");
			else
				dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
				       "No USB stick was found to write the flag to.\n\nCreate an "
				       "empty file named ps4linux-recovery in the root of a FAT32 "
				       "stick on a PC instead.");
			continue;
		}
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "A boot loop means GoldHEN is autoloading a Linux payload.\n\n"
		       "1. Power off by holding the button.\n"
		       "2. Start the exploit with a loader that does not autoload payloads "
		       "(NextHEN, PS4HEN Lite, Mira).\n"
		       "3. Delete the payload and the entry that names it - or, if the "
		       "console boots normally again, let this app do it from the Autoload "
		       "menu.\n"
		       "4. Put the payload file back on a stick or in /data/payloads and "
		       "launch this app: it restores the copy and starts Linux.\n\n"
		       "docs/RECOVERY.md has the long version.");
	}
}

// ------------------------------------------------------------------ reserve
#define PROBE_FILE BOOT_DIR "/.ps4linux-probe"
#define PROBE_FAST_US 150000ULL
#define PROBE_NEED 3
#define SETTLE_MIN_S 30
#define SETTLE_MAX_S 600

static uint64_t probe_us(void)
{
	static char block[4096];
	uint64_t t0 = sceKernelGetProcessTime();
	int fd = open(PROBE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return ~0ULL;
	block[0]++;
	ssize_t w = write(fd, block, sizeof(block));
	int f = fsync(fd);
	close(fd);
	if (w != (ssize_t)sizeof(block) || f != 0) return ~0ULL;
	return sceKernelGetProcessTime() - t0;
}

// Starting Linux while OrbisOS is still flushing the reserve crashed a tested
// console (no picture at 32 GB, panic at 64 GB). Measured, not guessed.
static void settle_drive(void)
{
	char msg[160];
	sync();
	if (progress_open("Finishing writing to the PS4 drive ...") < 0) {
		sceKernelUsleep(60 * 1000000);
		return;
	}
	uint64_t t0 = sceKernelGetProcessTime();
	int fast = 0, secs = 0;
	for (;;) {
		uint64_t us = probe_us();
		secs = (int)((sceKernelGetProcessTime() - t0) / 1000000);
		fast = (us != ~0ULL && us < PROBE_FAST_US) ? fast + 1 : 0;
		if (fast >= PROBE_NEED && secs >= SETTLE_MIN_S) break;
		if (secs >= SETTLE_MAX_S) break;
		snprintf(msg, sizeof(msg), "Finishing writing to the PS4 drive ... %d s", secs);
		progress_set((uint32_t)(secs * 100 / SETTLE_MAX_S), msg);
		sync();
		sceKernelUsleep(2 * 1000000);
	}
	unlink(PROBE_FILE);
	sync();
	progress_close();
	klog("drive settled after %d s", secs);
}

// Reserves `want` bytes as a fully-allocated file (never sparse: the PS4's UFS
// driver is safest that way). Resumes a .part left by an earlier run.
static int reserve(uint64_t want, uint64_t start, uint64_t gib)
{
	static unsigned char zero[4 * 1024 * 1024];
	char part[288], msg[200];
	snprintf(part, sizeof(part), "%s.part", ALLOC);

	if (exists(ALLOC) && file_size(ALLOC) == want) {
		klog("already reserved: %llu MB", (unsigned long long)(want >> 20));
		return 0;
	}
	if (exists(ALLOC) && file_size(ALLOC) != want) {

		unlink(ALLOC);
		klog("removed a reserve of a different size");
	}

	int out = open(part, O_WRONLY | O_CREAT, 0666);
	if (out < 0) return -1;
	uint64_t have = fd_size(out);
	if (have > want) {
		close(out);
		unlink(part);
		out = open(part, O_WRONLY | O_CREAT, 0666);
		if (out < 0) return -1;
		have = 0;
	}
	have -= have % sizeof(zero);
	if (lseek(out, (off_t)have, SEEK_SET) != (off_t)have) {
		close(out);
		unlink(part);
		return -1;
	}
	klog("reserving %llu MB (resume at %llu MB)", (unsigned long long)(want >> 20),
	     (unsigned long long)(have >> 20));

	snprintf(msg, sizeof(msg), "Reserving %llu GB for Linux (one time only) ...",
	         (unsigned long long)gib);
	if (progress_open(msg) < 0) {
		close(out);
		return -1;
	}
	uint64_t t0 = sceKernelGetProcessTime(), done = have, last = 0;
	int rc = 0, werr = 0;
	while (done < want) {
		size_t n = (want - done) < sizeof(zero) ? (size_t)(want - done) : sizeof(zero);
		ssize_t w = write(out, zero, n);
		if (w <= 0) { werr = errno; rc = -1; break; }
		done += (uint64_t)w;
		uint64_t now = sceKernelGetProcessTime();
		if (now - last >= 2000000ULL) {
			last = now;
			uint64_t el = (now - t0) / 1000000, mb = (done - have) >> 20;
			if (el >= 5 && mb) {
				uint64_t left = ((want - done) >> 20) * el / mb;
				snprintf(msg, sizeof(msg),
				         "Reserving %llu GB for Linux (one time only) ...\n"
				         "%llu MB/s, about %llu min %02llu s left",
				         (unsigned long long)gib, (unsigned long long)(mb / el),
				         (unsigned long long)(left / 60), (unsigned long long)(left % 60));
				progress_set((uint32_t)(done * 100 / want), msg);
			} else {
				progress_set((uint32_t)(done * 100 / want), NULL);
			}
		}
	}
	fsync(out);
	close(out);
	progress_close();
	if (rc != 0) {
		if (werr == 28) {
			unlink(part);
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "The PS4 drive is full.\n\nThe reservation was removed. Free up "
			       "space, or launch again and choose a smaller size.");
		} else {
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Writing to the PS4 drive failed (error %d).\n\nWhat was reserved "
			       "so far is kept: launch the app again to continue.", werr);
		}
		return -1;
	}
	if (file_size(part) != want || rename(part, ALLOC) != 0) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "The space was written but could not be finished. Launch the app "
		       "again.");
		return -1;
	}
	return 0;
}

// ------------------------------------------------------------------ boot files
// /data/linux/boot is shared with any other Linux install. Before writing ours,
// move anything already there out of the way and record it, so uninstall can put
// it back. This is the answer to "namespaced paths": the loader's search path is
// fixed upstream, so we cannot namespace - we take ownership explicitly.
static int claim_boot_files(void)
{
	char from[288], to[320];
	const char *shared[] = { "bzImage", "initramfs.cpio.gz", "bootargs.txt", "vram.txt", NULL };
	mkdirs(STATE_DIR);
	for (int i = 0; shared[i]; i++) {
		snprintf(from, sizeof(from), BOOT_DIR "/%s", shared[i]);
		if (!exists(from)) continue;
		snprintf(to, sizeof(to), BOOT_DIR "/%s.ps4linux-bak", shared[i]);
		if (exists(to)) {
			klog("%s already backed up; leaving both", shared[i]);
			continue;
		}
		if (rename(from, to) == 0) {
			char line[400];
			snprintf(line, sizeof(line), "backup %s %s\n", shared[i], to);
			int fd = open(STATE_DIR "/backups.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (fd >= 0) {
				write(fd, line, strlen(line));
				close(fd);
			}
			klog("backed up existing %s", shared[i]);
		} else {
			klog("could not back up %s (errno %d) - aborting", shared[i], errno);
			return -1;
		}
	}
	return 0;
}

static int copy_file(int in, const char *dst, uint64_t *done, uint64_t total, const char *label)
{
	static unsigned char buf[4 * 1024 * 1024];
	char part[288], msg[200];
	snprintf(part, sizeof(part), "%s.part", dst);
	snprintf(msg, sizeof(msg), "Copying %s ...", label);
	progress_set(total ? (uint32_t)(*done * 100 / total) : 0, msg);

	if (in < 0 || lseek(in, 0, SEEK_SET) != 0) return -1;
	int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0) return -1;
	ssize_t n;
	int rc = 0;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w <= 0) { rc = -1; break; }
			off += w;
		}
		if (rc) break;
		*done += (uint64_t)n;
		progress_set(total ? (uint32_t)(*done * 100 / total) : 0, NULL);
	}
	if (n < 0) rc = -1;
	fsync(out);
	close(out);
	if (rc == 0 && file_size(part) != fd_size(in)) rc = -1;
	if (rc == 0) {
		unlink(dst);
		if (rename(part, dst) != 0) rc = -1;
		else chmod(dst, 0666);
	}
	if (rc != 0) unlink(part);
	return rc;
}

// ------------------------------------------------------------------ loader
// The loader payload is the one thing this project does not ship: it is rebuilt
// per firmware generation and it is somebody else's binary. It is also the only
// reason to go through GoldHEN's payload menu at all - which is what this app is
// here to remove. Drop a current payload (ps4-linux-1gb.bin, or the .elf) in the
// root of a USB stick or in /data/payloads and the app adopts it: copies it next
// to the kernel as ps4linux-loader.bin, and starts Linux over BinLoader from then
// on, from this app.
//
// Nothing here ever writes an autoload entry. That is what would make a console
// restart into Linux in a loop on every jailbreak, and it is the reason this app
// starts the loader itself instead of leaving a file for GoldHEN to run.
#define LOADER BOOT_DIR "/ps4linux-loader.bin"
#define LOADER_MIN 4096u	// the package's own placeholder is 89 bytes
static char LOADER_FOUND[288];

// A payload is a raw binary or an ELF; either is far bigger than the placeholder.
// Cheap checks only: the real validation is the loader itself refusing to run.
static int looks_like_loader(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
	if ((uint64_t)st.st_size < LOADER_MIN) return 0;
	char head[16];
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, head, sizeof(head));
	close(fd);
	if (n < 8) return 0;
	if (n >= 11 && !memcmp(head, "PLACEHOLDER", 11)) return 0;
	return 1;
}

// Fills LOADER_FOUND and returns 1 when a payload was found. Order matters: the
// adopted copy next to the kernel first (the one we already know works), then a
// USB stick (where a fresh download lands), then /data/payloads.
static int loader_scan(void)
{
	static const char *dirs[] = { BOOT_DIR, "/mnt/usb0", "/data/payloads" };
	char p[288];
	LOADER_FOUND[0] = 0;
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		DIR *d = opendir(dirs[i]);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (!strstr(e->d_name, "linux") && !strstr(e->d_name, "Linux") &&
			    !strstr(e->d_name, "LINUX"))
				continue;
			const char *dot = strrchr(e->d_name, '.');
			if (!dot || (strcmp(dot, ".bin") && strcmp(dot, ".elf"))) continue;
			snprintf(p, sizeof(p), "%s/%s", dirs[i], e->d_name);
			if (looks_like_loader(p)) {
				snprintf(LOADER_FOUND, sizeof(LOADER_FOUND), "%s", p);
				closedir(d);
				return 1;
			}
		}
		closedir(d);
	}
	return 0;
}

// ------------------------------------------------------------------ payload
// Streams the payload from the package file straight into GoldHEN's BinLoader
// socket. There is deliberately no size cap: the upstream AIO loader payloads
// embed a kexec'd kernel and are several MB, so buffering one whole payload in
// the app's memory was both wasteful and a limit that would have refused to send
// the file at all.
static int send_payload(int fd)
{
	static unsigned char buf[64 * 1024];
	uint64_t size = fd_size(fd);
	if (size == 0 || lseek(fd, 0, SEEK_SET) != 0) return -1;
	sceNetInit();
	int s = sceNetSocket("ps4linux-loader", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct { uint8_t len; uint8_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; } sa;
	memset(&sa, 0, sizeof(sa));
	sa.len = sizeof(sa);
	sa.family = ORBIS_NET_AF_INET;
	sa.port = sceNetHtons(9090);
	sa.addr = 0x0100007F;
	if (sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa)) < 0) {
		sceNetSocketClose(s);
		return -1;
	}
	size_t off = 0;
	uint64_t sent = 0;
	int rc = 0;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		off = 0;
		while (off < (size_t)n) {
			int w = sceNetSend(s, buf + off, (size_t)n - off, 0);
			if (w <= 0) {
				rc = -1;
				break;
			}
			off += (size_t)w;
		}
		if (rc) break;
		sent += (uint64_t)n;
	}
	if (n < 0 || sent != size) rc = -1;
	sceNetSocketClose(s);
	if (rc == 0) klog("payload sent (%llu bytes)", (unsigned long long)sent);
	return rc;
}

// ------------------------------------------------------------------ screens
// The home screen offers the same five entries the dialog menu offers; this
// translates its answer back into the index those loops expect. -2 means
// "graphics memory", which the fresh-install menu has no slot for.
static int app_ui_pick(int installed)
{
	int a = appui_home(&uist);

	if (a == APPUI_CANCEL)
		return -1;

	if (installed) {
		switch (a) {
		case APPUI_START: return 0;
		case APPUI_VRAM: return 1;
		case APPUI_AUTOLOAD: return 2;
		case APPUI_CHROME: return 3;
		case APPUI_RECOVERY: return 4;
		default: return -1;
		}
	}

	switch (a) {
	case APPUI_INSTALL: return 0;
	case APPUI_VRAM: return -2;
	case APPUI_AUTOLOAD: return 1;
	case APPUI_CHROME: return 2;
	case APPUI_RECOVERY: return 3;
	default: return -1;
	}
}

// ------------------------------------------------------------------ main
int main(void)
{
	char path[288], buf[64];
	uint64_t start = 0;

	sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
	sceCommonDialogInitialize();
	klog("start (build " __DATE__ " " __TIME__ ")");

	// The graphical screens, if this console can show them. Everything below
	// falls back to the system dialogs when this returns 0.
	ui_ready = appui_init();
	klog("screens: %s", ui_ready ? "on" : "system dialogs");
	if (ui_ready)
		ui_refresh();

	// 1. package files
	SRC[0] = 0;
	for (size_t i = 0; i < sizeof(src_candidates) / sizeof(src_candidates[0]); i++) {
		snprintf(path, sizeof(path), "%s/bzImage", src_candidates[i]);
		if (exists(path)) {
			snprintf(SRC, sizeof(SRC), "%s", src_candidates[i]);
			break;
		}
	}
	if (!SRC[0]) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Cannot find the installer's files.\n\nReinstall the package and try "
		       "again.");
		quit();
	}

	int src_fd[N_BOOT_FILES];
	uint64_t total = 0;
	int no_rootfs = 0;
	for (size_t i = 0; i < N_BOOT_FILES; i++) {
		snprintf(path, sizeof(path), "%s/%s", SRC, boot_files[i]);
		src_fd[i] = open(path, O_RDONLY);
		if (src_fd[i] < 0) {
			//In a package built with WITH_ROOTFS=1 this never happens; with
			// WITH_ROOTFS=0 the image is built on a host instead (docs/BUILD-PKG.md).
			if (!strcmp(boot_files[i], "rootfs.tar.zst")) {
				no_rootfs = 1;
				continue;
			}
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "The package is incomplete: %s is missing.", boot_files[i]);
			quit();
		}
		total += fd_size(src_fd[i]);
	}
	// Look for a real loader payload before anything else: the package carries
	// only a placeholder, and without one there is nothing to start.
	int have_loader = loader_scan();
	klog("loader payload: %s", have_loader ? LOADER_FOUND : "none found");

	snprintf(path, sizeof(path), "%s/chromeos.img.state-start", SRC);
	if (read_small(path, buf, sizeof(buf)) == 0)
		start = strtoull(buf, NULL, 10);

	// 2. what is already here?
	int installed = exists(MANIFEST) && exists(BOOT_DIR "/bzImage");
	struct cros_found cros;
	cros_scan(&cros);

	// A note from the boot side ("Linux was started by an autoload entry") and an
	// armed entry are the same problem, so both are dealt with first - and only if
	// the user says so. If the entry is currently looping, the app never gets here
	// at all; docs/RECOVERY.md covers that case.
	autoload_note_prompt();
	autoload_offer_remove();

	// 3. already installed -> start, or change the graphics memory first. The
	//    app stays installed after the install (it is the only way to start Linux
	//    again), so this is where the setting lives for the rest of the console's
	//    life - no file editing, no FTP.
	if (installed) {
		// One entry per category, each with its own submenu. This app is the only
		// way to start Linux, so it is also where everything about the install
		// lives - and Start Linux stays the first entry, one press away.
		static const char *const items[] = {
			"Start Linux", "Graphics memory (VRAM)", "Autoload",
			"Remove ChromeOS", "Recovery",
		};
		for (;;) {
			int m = ui_ready ? app_ui_pick(1) : menu(APP_NAME, items, 5, 0);
			if (m < 0) quit();
			if (m == 1) { vram_menu(); continue; }
			if (m == 2) { autoload_menu(); continue; }
			if (m == 3) { cros_menu(&cros); continue; }
			if (m == 4) { recovery_menu(); continue; }
			break;	// Start Linux
		}
		if (!have_loader) {
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "There is no loader payload on this console yet, so Linux cannot "
			       "be started from here.\n\n"
			       "Copy a current PS4 Linux payload (ps4-linux-1gb.bin, or the .elf) "
			       "into the root of a USB stick, or into /data/payloads, then launch "
			       "this app again: it picks it up by itself. Any VRAM size will do - "
			       "the app sets that.\n\n"
			       "Nothing was changed.");
			quit();
		}
		// Put the copy next to the kernel back if it went missing. Breaking a boot
		// loop usually means deleting the payload, which leaves the install with no
		// loader - and that is why the recovery doc can say "just launch the app
		// again": this is the code that makes it true.
		if (strcmp(LOADER_FOUND, LOADER) != 0) {
			int src = open(LOADER_FOUND, O_RDONLY);
			if (src >= 0) {
				uint64_t d = 0, sz = fd_size(src);
				if (progress_open("Putting the loader back ...") >= 0) {
					if (copy_file(src, LOADER, &d, sz, "loader payload") == 0)
						klog("loader payload restored at %s", LOADER);
					else
						klog("could not restore %s", LOADER);
					progress_close();
				}
				close(src);
			}
		}
		int lf = open(LOADER_FOUND, O_RDONLY);
		goldhen_unjailbreak();
		if (lf < 0 || send_payload(lf) != 0) {
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "The loader could not be started.\n\nIs GoldHEN's BinLoader "
			       "server (port 9090) enabled?\n\nNothing else is affected: launch "
			       "this app again once BinLoader is on.");
			quit();
		}
		for (;;) sceKernelUsleep(1000000);
	}

	// Nothing installed yet: the menu still exists, so the autoload, ChromeOS and
	// recovery entries are reachable without starting an install.
	{
		static const char *const items[] = {
			"Install Linux", "Autoload", "Remove ChromeOS", "Recovery",
		};
		for (;;) {
			int m = ui_ready ? app_ui_pick(0) : menu(APP_NAME, items, 4, 0);
			if (m == -2) { vram_menu(); continue; }  // Graphics memory, no install
			if (m < 0) quit();
			if (m == 1) { autoload_menu(); continue; }
			if (m == 2) { cros_menu(&cros); continue; }
			if (m == 3) { recovery_menu(); continue; }
			break;	// Install Linux
		}
	}

	// 4. briefing
	if (!briefing()) quit();

	// The last screen before anything is written: what is about to happen,
	// in the same layout as the rest of the app.
	if (ui_ready) {
		ui_refresh();
		if (!appui_install_confirm(&uist))
			quit();
	}

	// 5. An existing ChromeOS install is reported here, never touched: removing
	//    it is a separate package (PS4LNX002). This app removes no existing file.
	if (cros.image || cros.n_boot) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "A ChromeOS for PS4 installation was found. It will be left "
		       "untouched.\n\n"
		       "Image: %llu MB\nOther files: %d (%llu MB)\n\n"
		       "Linux is installed beside it and keeps working. To reclaim that "
		       "space, run \"PS4 Linux: Remove ChromeOS\" first, then start "
		       "this app again.",
		       (unsigned long long)(cros.image_size >> 20), cros.n_boot,
		       (unsigned long long)(cros.boot_bytes >> 20));
	}

	// 6. where + how much
	uint64_t free_user = free_bytes(BOOT_DIR);
	if (!free_user && goldhen_jailbreak() == 0)
		free_user = free_bytes(BOOT_DIR);
	uint64_t avail = free_user + file_size(ALLOC);
	avail = avail > RESERVE_FREE ? avail - RESERVE_FREE : 0;
	if (!avail) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "The free space of the PS4 drive could not be read.\n\nMake sure "
		       "GoldHEN 2.4 or newer is running, then launch the app again.\n\nIf it "
		       "keeps failing, install to a USB drive instead.");
		goldhen_unjailbreak();
		quit();
	}
	uint64_t gib = ask_install(start, avail);

	// 7. copy + reserve
	if (claim_boot_files() != 0) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Could not move an existing Linux boot file aside, so nothing was "
		       "changed.\n\nDetails are in the GoldHEN log.");
		quit();
	}
	mkdirs(BOOT_DIR);
	mkdirs("/data/payloads");
	if (progress_open("Copying PS4 XFCE Linux ...") < 0) quit();
	uint64_t done = 0;
	for (size_t i = 0; i < N_BOOT_FILES; i++) {
		if (src_fd[i] < 0) continue;	// optional asset that is not in this build
		snprintf(path, sizeof(path), BOOT_DIR "/%s", boot_files[i]);
		if (copy_file(src_fd[i], path, &done, total, boot_files[i]) != 0) {
			progress_close();
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Copying %s failed. Launch the app again to retry.", boot_files[i]);
			quit();
		}
	}

	// Adopt a real loader payload if there is one, replacing the placeholder the
	// package carries. This is the point of the exercise: the app starts Linux
	// itself, so the GoldHEN payload menu is never opened and no autoload entry is
	// ever written - which is what makes consoles restart into Linux in a loop.
	int loader_ready = 0;
	if (have_loader && strcmp(LOADER_FOUND, LOADER) != 0) {
		int lf = open(LOADER_FOUND, O_RDONLY);
		if (lf >= 0) {
			if (copy_file(lf, LOADER, &done, total, "loader payload") == 0)
				loader_ready = 1;
			close(lf);
		}
	} else if (have_loader) {
		loader_ready = 1;
	}

	// First start: 1 GB of VRAM. Every guide says the same thing - the install and
	// the first boot need the RAM more than the GPU does - and 1 GB is what the
	// loader itself defaults to when vram.txt is missing. Written explicitly so the
	// value is visible, and so a later start can change it without guesswork.
	if (write_vram(VRAM_FIRST) != 0)
		klog("could not write vram.txt (errno %d)", errno);

	// GoldHEN's payload menu lists the files in /data/payloads, and starting the
	// loader from that menu is the way in when BinLoader (port 9090) is switched
	// off. The reference project keeps a copy there for the same reason. Not
	// fatal if it fails: the BinLoader path above may still work.
	int payload_copy_failed = 1;
	if (loader_ready) {
		int lf = open(LOADER, O_RDONLY);
		if (lf >= 0) {
			if (copy_file(lf, "/data/payloads/ps4linux-loader.bin", &done, total,
			              "ps4linux-loader.bin for GoldHEN") == 0)
				payload_copy_failed = 0;
			close(lf);
		}
	}
	progress_close();
	if (!loader_ready)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "No loader payload was found, so Linux cannot be started from this "
		       "app yet.\n\n"
		       "Copy a current PS4 Linux payload (ps4-linux-1gb.bin, or the .elf) "
		       "into the root of a USB stick, or into /data/payloads, then launch "
		       "this app again: it adopts the file by itself. Any VRAM size will "
		       "do - the app sets that.\n\n"
		       "Everything else is already installed. The ps4linux-loader.bin in the "
		       "package is a placeholder and cannot boot anything.");
	else if (payload_copy_failed)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "The loader was not copied into /data/payloads, so GoldHEN's payload "
		       "menu will not list it.\n\nStarting Linux from this app still works "
		       "if BinLoader is enabled. Otherwise copy ps4linux-loader.bin from "
		       "/data/linux/boot into /data/payloads over FTP.");

	if (no_rootfs)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "This package carries no root filesystem, so the image it reserves "
		       "stays empty.\n\n"
		       "Before starting Linux, copy a prebuilt ps4linux.img (made with "
		       "scripts/build-rootimg.sh) to /data/linux/boot, or use a package "
		       "built with a rootfs.tar.zst.\n\n"
		       "Booting with an empty image stops at a rescue shell; nothing on "
		       "the console is modified.");

	if (gib) {
		uint64_t want = img_bytes(start, gib);
		if (reserve(want, start, gib) != 0) quit();
		snprintf(buf, sizeof(buf), "target=internal\nstate_gib=%llu\n",
		         (unsigned long long)gib);
	} else {
		snprintf(buf, sizeof(buf), "target=external\n");
	}
	write_small(STATE_DIR "/choice.txt", buf);

	// The same decision again, in the layout the patched PS4 Linux initramfs
	// reads (checked against the ChromeOS original it was adapted from):
	// key=value lines under /data/linux/boot, first match wins, values trimmed
	// of whitespace and CR, unknown keys ignored, and the choice file deleted
	// once it has been acted on. Written in addition to our own files, never
	// instead of them, so either initramfs works unchanged.
	write_small(BOOT_DIR "/ps4linux-install-choice", buf);
	write_small(BOOT_DIR "/ps4linux-target", buf);

	char manifest[512];
	snprintf(manifest, sizeof(manifest),
	         "target=%s\nstate_gib=%llu\nstart=%llu\ninstalled=1\n",
	         gib ? "internal" : "external", (unsigned long long)gib,
	         (unsigned long long)start);
	write_small(MANIFEST, manifest);

	if (gib) settle_drive();

	// 8. explicit confirmation before anything boots
	int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Start Linux", "Not now",
	               "Setup is finished.\n\nStart Linux now?\n\nThe console restarts into "
	               "Linux and finishes the install there (LED orange).\n\n"
	               "Graphics memory is set to 1024 MB, which is what the first start "
	               "should use. Launch this app again later to change it.\n\n"
	               "If you choose \"Not now\", launch this app again when you are ready.");
	if (r != 1) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Nothing else will happen. Launch this app again to start Linux.");
		quit();
	}

	if (!loader_ready) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Everything is installed, but there is no loader payload yet, so "
		       "Linux cannot be started.\n\n"
		       "Copy a current PS4 Linux payload (ps4-linux-1gb.bin, or the .elf) "
		       "into the root of a USB stick, or into /data/payloads, then launch "
		       "this app again and press Start Linux.");
		quit();
	}
	int lf = open(LOADER, O_RDONLY);
	goldhen_unjailbreak();
	if (lf < 0 || send_payload(lf) != 0) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Everything is in place, but the loader could not be started.\n\n"
		       "Is GoldHEN's BinLoader server (port 9090) enabled? Launch this app "
		       "again once it is.");
		quit();
	}
	for (;;) sceKernelUsleep(1000000);
	return 0;
}
