// PS4 Linux: Remove ChromeOS - uninstaller (OpenOrbis homebrew, needs GoldHEN).
//
// A separate package from the installer on purpose. This app is the only place
// in the project that deletes anything, and it deletes one thing: a
// ChromeOS-for-PS4 installation. It never reserves space, never boots anything,
// never unjailbreaks, and never touches games, saves or the PS4 system.
//
// Flow:
//   1. scan for a ChromeOS-for-PS4 installation
//   2. nothing found -> say so and exit (no writes at all)
//   3. list every file with its size, on the TV, before anything happens
//   4. confirmation 1: "Delete these" / "Cancel"
//   5. confirmation 2: "Delete now" / "Cancel"   (last chance)
//   6. delete, one file at a time, progress shown
//   7. re-scan and report what actually went, and what was left alone
//
// What it deliberately does NOT delete: bzImage, initramfs.cpio.gz, bootargs.txt
// and vram.txt. Those names are shared with every other PS4 Linux install
// (docs/LAYOUT.md), so deleting them would break a coexisting install.
//
// Builds with the OpenOrbis toolchain (OO_PS4_TOOLCHAIN); see the repository
// README for the command and for what remains unverified. Nothing here has run
// on a PS4 yet.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#include "appui.h"
#include "status.h"
#include "ui.h"

#define APP_NAME "PS4 Linux: Remove ChromeOS"

// Same rule as the installer: the screens are used when they can start, and
// every dialog below stays as the fallback.
static int ui_ready;
static ui_state uist;

// ------------------------------------------------------------------ paths
#define BOOT_DIR "/data/linux/boot"
#define IMAGE "/user/app/chromeos.img"

// Files whose names belong to ChromeOS-for-PS4 alone. Anything else in
// /data/linux/boot is left where it is.
static const char *k_cros_files[] = {
	BOOT_DIR "/chromeos.img.xz",
	BOOT_DIR "/chromeos.img.size",
	BOOT_DIR "/chromeos.img.alloc",
	BOOT_DIR "/chromeos.img.alloc.part",
	BOOT_DIR "/chromeos-install-choice",
	BOOT_DIR "/chromeos-target",
	BOOT_DIR "/chromeos-error.txt",
	BOOT_DIR "/chromeos-external.guid",
	BOOT_DIR "/chromeos-dryrun",
	BOOT_DIR "/ps4-layer.sqfs",
	BOOT_DIR "/ps4-chromeos-loader.bin",
	"/data/payloads/ps4-chromeos-loader.bin",
	NULL,
};

#define N_CROS_FILES (sizeof(k_cros_files) / sizeof(k_cros_files[0]) - 1)

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
	sceKernelDebugOutText(0, "[ps4linux-remove] ");
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
	sceMsgDialogProgressBarSetValue(0, pct);
	if (msg) sceMsgDialogProgressBarSetMsg(0, msg);
	sceMsgDialogUpdateStatus();
}

static void progress_close(void)
{
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
static uint64_t file_size(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	off_t end = lseek(fd, 0, SEEK_END);
	close(fd);
	return end > 0 ? (uint64_t)end : 0;
}

static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

// ------------------------------------------------------------------ scan
struct cros_found {
	int image;
	uint64_t image_size;
	int n_files;
	uint64_t files_bytes;
	int present[N_CROS_FILES];
	char listing[1200];
};

static void cros_scan(struct cros_found *c)
{
	memset(c, 0, sizeof(*c));
	c->image_size = file_size(IMAGE);
	c->image = c->image_size > 0;
	if (c->image) {
		snprintf(c->listing, sizeof(c->listing), "  %s (%llu MB)\n", IMAGE,
		         (unsigned long long)(c->image_size >> 20));
	}
	for (size_t i = 0; i < N_CROS_FILES; i++) {
		uint64_t sz = file_size(k_cros_files[i]);
		if (!sz) continue;
		c->present[i] = 1;
		c->n_files++;
		c->files_bytes += sz;
		size_t used = strlen(c->listing);
		if (used < sizeof(c->listing) - 1)
			snprintf(c->listing + used, sizeof(c->listing) - used,
			         "  %s (%llu MB)\n", k_cros_files[i],
			         (unsigned long long)(sz >> 20));
	}
}

static uint64_t cros_total(const struct cros_found *c)
{
	return c->image_size + c->files_bytes;
}

// Nothing outside this list is unlinked, and only after two confirmations.
static int cros_remove(struct cros_found *c)
{
	int failed = 0, done = 0;
	int steps = (c->image ? 1 : 0) + c->n_files;
	if (!steps) return 0;

	if (progress_open("Removing ChromeOS for PS4 ...") < 0) {
		klog("could not open the progress dialog");
		return -1;
	}

	if (c->image) {
		progress_set(0, "Removing the ChromeOS disk image ...");
		if (unlink(IMAGE) != 0) {
			klog("unlink %s failed: %d", IMAGE, errno);
			failed++;
		} else {
			c->image = 0;
			c->image_size = 0;
		}
		done++;
	}

	for (size_t i = 0; i < N_CROS_FILES; i++) {
		if (!c->present[i]) continue;
		progress_set((uint32_t)((done * 100) / steps), k_cros_files[i]);
		if (unlink(k_cros_files[i]) != 0) {
			klog("unlink %s failed: %d", k_cros_files[i], errno);
			failed++;
		} else {
			c->present[i] = 0;
			c->files_bytes = 0;   // recomputed by the caller's re-scan
		}
		done++;
	}
	progress_close();
	klog("removal finished: %d failed", failed);
	return failed;
}

// A payload autoload entry is not ChromeOS and is not touched. It is worth one
// warning: an autoloaded Linux payload restarts the console in a loop.
static const char *k_autoload[] = {
	"/data/GoldHEN/autoload",
	"/data/GoldHEN/autoload.txt",
	"/data/bdj_autoload.cfg",
	"/data/payloads/autoload.txt",
	NULL,
};

static const char *autoload_present(void)
{
	for (int i = 0; k_autoload[i]; i++)
		if (exists(k_autoload[i])) return k_autoload[i];
	return NULL;
}

// ------------------------------------------------------------------ main
int main(void)
{
	sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
	sceCommonDialogInitialize();
	klog("start (build " __DATE__ " " __TIME__ ")");

	struct cros_found c;
	cros_scan(&c);

	ui_ready = appui_init();
	if (ui_ready) {
		ps4_status s;

		status_read("", &s);
		ui_state_from_status(&uist, &s);
	}
	klog("screens: %s", ui_ready ? "on" : "system dialogs");

	if (!c.image && !c.n_files) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "No ChromeOS for PS4 installation was found.\n\n"
		       "Looked for:\n  %s\n  and %u files in %s\n\n"
		       "Nothing was changed.",
		       IMAGE, (unsigned)N_CROS_FILES, BOOT_DIR);
		quit();
	}

	const char *al = autoload_present();
	if (al)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Note: a payload autoload file exists at:\n%s\n\n"
		       "It is not ChromeOS and will NOT be removed. If you added it "
		       "yourself, remove it: an autoloaded Linux payload makes the "
		       "console restart in a loop every time you jailbreak.\n\n"
		       "See docs/RECOVERY.md.", al);

	// Everything that will go, on screen, before anything goes.
	{
		int confirmed;

		if (ui_ready) {
			confirmed = appui_remove_confirm(&uist);
		} else {
			confirmed = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS,
			                   "Delete these", "Cancel",
			                   "ChromeOS for PS4 was found.\n\n"
			                   "This removes %d MB and cannot be undone.\n\n"
			                   "These exact items will be deleted:\n\n%s\n"
			                   "NOT deleted: games, saves, the PS4 system, and "
			                   "the shared boot files (bzImage, "
			                   "initramfs.cpio.gz).",
			                   (int)(cros_total(&c) >> 20), c.listing) == 1;
		}
		if (!confirmed) {
			dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
			       "Nothing was deleted.");
			quit();
		}
	}

	if (dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Delete now", "Cancel",
	           "Last confirmation.\n\n"
	           "Delete the items listed on the previous screen and free their "
	           "space?\n\n"
	           "There is no undo.") != 1) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Nothing was deleted.");
		quit();
	}

	uint64_t freed = cros_total(&c);
	int failed = cros_remove(&c);

	// Report what is actually gone, by looking again rather than by assuming.
	struct cros_found after;
	cros_scan(&after);
	uint64_t left = cros_total(&after);

	if (!after.image && !after.n_files && failed == 0) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "ChromeOS for PS4 was removed.\n\n"
		       "%d MB freed.\n\n"
		       "Games, saves and the PS4 system were not touched. The space "
		       "shows up again in Settings > Storage, possibly after a "
		       "restart.\n\n"
		       "The shared boot files were left in place.", (int)(freed >> 20));
	} else {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Removal finished with problems.\n\n"
		       "%d MB freed, %d MB still in place, %d file(s) could not be "
		       "deleted.\n\n"
		       "Details are in the GoldHEN log (port 3232). Run this app "
		       "again to retry, or delete the remaining files over FTP.",
		       (int)(freed >> 20), (int)(left >> 20), failed);
	}
	quit();
	return 0;
}
