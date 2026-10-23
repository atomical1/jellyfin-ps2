/*
 * IOP modules. Resets the IOP, patches it to accept modules from EE
 * memory, and loads the drivers embedded in the ELF: USB storage,
 * keyboard, network and sound.
 */

#include "mods.h"
#include "config.h"

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <libkbd.h>
#include <stdio.h>
#include <string.h>

/*
 * Module set and load order taken from
 * $PS2SDK/samples/network/tcpip-basic/ps2ip.c
 *
 * Three network modules only. There is no ps2ip.irx or ps2ips.irx: lwIP
 * runs on the EE side and is linked in with -lps2ip.
 */

#define IRX_DECL(n) extern unsigned char n[]; extern unsigned int size_##n;

IRX_DECL(usbd_irx)
IRX_DECL(ps2kbd_irx)
IRX_DECL(bdm_irx)
IRX_DECL(bdmfs_fatfs_irx)
IRX_DECL(usbmass_bd_irx)
IRX_DECL(netman_irx)
#if JFPS2_USB_AX88179
IRX_DECL(ax88179_irx)
#else
IRX_DECL(dev9_irx)
IRX_DECL(smap_irx)
#endif
IRX_DECL(libsd_irx)
IRX_DECL(audsrv_irx)

static int  g_kbd = 0;
static char g_failed[160] = "";

static ModsProgress g_progress;

/* Report a loading step through the progress callback, if any. */
static void say(const char *msg)
{
	if (g_progress)
		g_progress(msg);
}

/*
 * Add a module's name to the list of those that failed to load, shown on
 * the connect screen.
 */
static void note_failure(const char *name)
{
	if (strlen(g_failed) + strlen(name) + 2 >= sizeof(g_failed))
		return;
	if (g_failed[0])
		strcat(g_failed, " ");
	strcat(g_failed, name);
}

/*
 * Load one IOP module from a buffer embedded in the ELF (bin2c), noting a
 * failure. Returns the module id, or negative.
 */
static int load_args(const char *name, unsigned char *buf, unsigned int size,
                     int arg_len, const char *args)
{
	char msg[64];
	snprintf(msg, sizeof(msg), "loading %s", name);
	say(msg);

	int ret = 0;
	int id  = SifExecModuleBuffer(buf, size, arg_len, args, &ret);

	if (id < 0 || ret != 0) {
		note_failure(name);
		return -1;
	}

	return id;
}

static int load(const char *name, unsigned char *buf, unsigned int size)
{
	return load_args(name, buf, size, 0, NULL);
}

/*
 * Reset the IOP, patch it so modules can be loaded from EE memory, and
 * load every module the app needs: USB (bdm, FAT, mass storage), keyboard,
 * network (dev9, netman, smap), sound (libsd, audsrv).
 */
void mods_load(ModsProgress progress)
{
	g_progress = progress;

#if IOP_RESET
	/*
	 * A fresh IOP. Required so sbv_patch_enable_lmb can take effect, which
	 * in turn is what allows SifExecModuleBuffer to load modules out of EE
	 * memory. Without this, module loading silently wedges.
	 *
	 * Note: this also tears down ps2link's host: filesystem, so if you
	 * launch over ps2client, settings.txt must live on mass:.
	 */
	say("resetting IOP");
	sceSifInitRpc(0);
	while (!SifIopReset("", 0)) {}
	while (!SifIopSync()) {}
#endif

	say("sif services");
	sceSifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();
	sbv_patch_enable_lmb();

	/* Pad, from the BIOS. padInit and padPortOpen hang without these. */
	say("rom0:SIO2MAN");
	if (SifLoadModule("rom0:SIO2MAN", 0, NULL) < 0)
		note_failure("sio2man");

	say("rom0:PADMAN");
	if (SifLoadModule("rom0:PADMAN", 0, NULL) < 0)
		note_failure("padman");

	/* USB: the keyboard and mass storage both sit on top of usbd. */
#if JFPS2_USB_AX88179 && !SKIP_NETWORK
	/* USBD silently skips devices when all configuration descriptors do
	 * not fit its default 512-byte cache. Allow composite network devices. */
	static const char usb_args[] = "conf=2048";
	load_args("usbd", usbd_irx, size_usbd_irx, sizeof(usb_args), usb_args);
	/* Give the exact ASIX ID priority over general class drivers. */
	if (load("netman", netman_irx, size_netman_irx) >= 0)
		load("ax88179", ax88179_irx, size_ax88179_irx);
#else
	load("usbd",    usbd_irx,        size_usbd_irx);
#endif
	load("bdm",     bdm_irx,         size_bdm_irx);
	load("fatfs",   bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
	load("usbmass", usbmass_bd_irx,  size_usbmass_bd_irx);

	/* Sound. audsrv sits on top of libsd, so the order matters. */
	load("libsd",  libsd_irx,  size_libsd_irx);
	load("audsrv", audsrv_irx, size_audsrv_irx);

	if (load("ps2kbd", ps2kbd_irx, size_ps2kbd_irx) >= 0 &&
	    PS2KbdInit() >= 0) {
		PS2KbdSetReadmode(PS2KBD_READMODE_NORMAL);
		g_kbd = 1;
	}

#if SKIP_NETWORK
	say("network skipped (SKIP_NETWORK=1)");
	note_failure("network-skipped");
#elif !JFPS2_USB_AX88179
	/* Native Ethernet: exactly the SDK sample's module set and order. */
	load("dev9",   dev9_irx,   size_dev9_irx);
	load("netman", netman_irx, size_netman_irx);
	load("smap",   smap_irx,   size_smap_irx);
#endif

	say("settling");
	for (int i = 0; i < 120; i++)
		nopdelay();

	g_progress = NULL;
}

/* Whether a USB keyboard driver was loaded. */
int         mods_have_keyboard(void) { return g_kbd; }

/* Space-separated names of modules that failed to load, or ''. */
const char *mods_failures(void)      { return g_failed; }
