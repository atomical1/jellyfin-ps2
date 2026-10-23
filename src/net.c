/*
 * Networking: starting the PS2's network (static, DHCP or auto), plain
 * HTTP/1.0 requests into one shared buffer, long streams read piece by
 * piece, fire-and-forget reports, and the timed sleep (net_snooze) every
 * thread uses, counted in vertical blanks.
 */

#include "net.h"
#include "config.h"
#include "settings.h"
#include "ax88179_status.h"

#include <netman.h>
#include <ps2ip.h>
#include <sys/socket.h>
#include <unistd.h>
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* 64-byte aligned: the network path DMAs into this, and an unaligned
 * destination is a classic source of dropped bytes on the EE. */
static char g_raw[HTTP_RAW_MAX] __attribute__((aligned(64)));

/*
 * Who this PS2 says it is, in every request's Authorization header. Each
 * signed-in user has a device id of their own (saved in settings.txt), so
 * that signing one in never signs another out; before anyone signs in it is
 * a neutral placeholder.
 */
static char g_device[48] = "ps2-unsigned";

/*
 * Set the DeviceId every request carries: the signed-in user's own, or a
 * placeholder before anyone signs in.
 */
void net_set_device_id(const char *id)
{
	snprintf(g_device, sizeof(g_device), "%s", (id && id[0]) ? id : "ps2-unsigned");
}

/*
 * The MediaBrowser Authorization header. Jellyfin wants it even with no
 * token - signing in sends it bare - so it goes on every request.
 */
static int auth_header(char *out, int max, const char *token)
{
	if (token && token[0])
		return snprintf(out, max,
		    "Authorization: MediaBrowser Client=\"JellyfinPS2\", "
		    "Device=\"PlayStation 2\", DeviceId=\"%s\", "
		    "Version=\"0.1\", Token=\"%s\"\r\n", g_device, token);

	return snprintf(out, max,
	    "Authorization: MediaBrowser Client=\"JellyfinPS2\", "
	    "Device=\"PlayStation 2\", DeviceId=\"%s\", "
	    "Version=\"0.1\"\r\n", g_device);
}
static char g_local_ip[20] = "0.0.0.0";

#if !SKIP_NETWORK
static int g_dhcp_state = -1, g_dhcp_elapsed;
static char g_dhcp_trace[256];
static const char *dhcp_phase(int state)
{
	switch (state) {
	case DHCP_STATE_SELECTING: return "waiting for offer";
	case DHCP_STATE_REQUESTING: return "requesting offered IP";
	case DHCP_STATE_CHECKING: return "checking offered IP";
	case DHCP_STATE_BOUND: return "address ready";
	case DHCP_STATE_BACKING_OFF: return "address conflict/backoff";
	case DHCP_STATE_OFF: return "client off";
	case -1: return "interface missing";
	default: return "negotiating";
	}
}
#endif

#if JFPS2_USB_AX88179 && !SKIP_NETWORK
static void usb_detection_log(const Ax88179Status *usb)
{
	char path[320];
	const char *base = settings_path();
	const char *separator = strrchr(base, '/');
	if (!separator) separator = strrchr(base, ':');
	snprintf(path, sizeof(path), "%.*sjfusbethernet.txt",
	         separator ? (int)(separator - base + 1) : 0, base);
	FILE *file = fopen(path, "w");
	if (!file) return;
	fprintf(file, "Jellyfin PS2 USB network diagnostic 6\n"
	        "build=%s\nstate=%d stage=%d usb_result=%d\n"
	        "ready_devices=%u probe_calls=%u\n"
	        "port1=%08X port2=%08X\n",
	        JFPS2_BUILD, usb->state, usb->stage, usb->usb_error,
	        usb->usb_devices_seen, usb->probe_count,
	        usb->port_status[0], usb->port_status[1]);
	for (int i = 0; i < 4; ++i)
		fprintf(file, "device%d=%04X:%04X\n", i + 1,
		        usb->usb_ids[i] >> 16, usb->usb_ids[i] & 0xffff);
	unsigned int enumeration[16];
	if (NetManIoctl(AX88179_IOCTL_ENUMERATION, NULL, 0, enumeration, sizeof(enumeration)) == 0) {
		for (int i = 0; i < 2; ++i) {
			unsigned int *e = enumeration + i * 8;
			fprintf(file, "pending%d: id=%u state=%u port=%u result=%u request=%06X length=%u cursor=%u identity=%04X:%04X\n",
			        i + 1, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7] >> 16, e[7] & 0xffff);
		}
	}
	fprintf(file, "phy=%04X rx=%u tx=%u rx_dropped=%u tx_dropped=%u\n",
	        usb->phy_status, usb->rx_frames, usb->tx_frames,
	        usb->rx_dropped, usb->tx_dropped);
	Ax88179Traffic traffic;
	if (NetManIoctl(AX88179_IOCTL_TRAFFIC, NULL, 0, &traffic, sizeof(traffic)) == 0) {
		fprintf(file, "rx_completions=%u rx_bytes=%u last_rx_size=%d last_rx_result=%d "
		        "last_rx_footer=%08X rx_bad_batches=%u\n"
		        "tx_submitted=%u last_tx_size=%d last_tx_result=%d last_tx_count=%d\n",
		        traffic.rx_completions, traffic.rx_bytes, traffic.last_rx_size,
		        traffic.last_rx_result, traffic.last_rx_footer, traffic.rx_bad_batches,
		        traffic.tx_submitted, traffic.last_tx_size, traffic.last_tx_result, traffic.last_tx_count);
		for (int direction = 0; direction < 2; ++direction) {
			const unsigned char *h = direction ? traffic.tx_header : traffic.rx_header;
			fprintf(file, "%s_macs=", direction ? "tx" : "rx");
			for (int i = 0; i < 12; ++i) fprintf(file, "%02X%s", h[i], i == 11 ? "\n" : ":");
		}
	}
	Ax88179Registers registers;
	if (NetManIoctl(AX88179_IOCTL_REGISTERS, NULL, 0, &registers, sizeof(registers)) == 0) {
		fprintf(file, "registers_valid=%u rx_restarts=%u fifo=%08X rx_control=%04X "
		        "medium=%04X usb_speed=%02X clock=%02X\n"
		        "bulk=%02X:%02X:%02X:%02X:%02X endpoint_in=%02X endpoint_out=%02X revision=%04X config=%u\n"
		        "node=%02X:%02X:%02X:%02X:%02X:%02X\n",
		        registers.valid, registers.restarts, registers.fifo_status, registers.rx_control,
		        registers.medium, registers.usb_status, registers.clock,
		        registers.bulk[0], registers.bulk[1], registers.bulk[2], registers.bulk[3], registers.bulk[4],
		        registers.in_endpoint, registers.out_endpoint, registers.revision, registers.configuration,
		        registers.node[0], registers.node[1], registers.node[2], registers.node[3], registers.node[4], registers.node[5]);
	}
	t_ip_info info;
	if (ps2ip_getconfig("sm0", &info) == 1) {
		unsigned char ip[4];
		memcpy(ip, &info.ipaddr, sizeof(ip));
		fprintf(file, "dhcp_enabled=%d dhcp_state=%d ip=%u.%u.%u.%u\n"
		        "mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
		        info.dhcp_enabled, info.dhcp_status, ip[0], ip[1], ip[2], ip[3],
		        info.hw_addr[0], info.hw_addr[1], info.hw_addr[2],
		        info.hw_addr[3], info.hw_addr[4], info.hw_addr[5]);
	} else {
		fprintf(file, "tcpip_interface=unavailable\n");
	}
	fprintf(file, "dhcp_wait_seconds=%d dhcp_last_state=%d\ndhcp_transitions=%s\n",
	        g_dhcp_elapsed, g_dhcp_state, g_dhcp_trace);
	fflush(file);
	fclose(file);
}
#endif

const char *net_link_help(void)
{
#if JFPS2_USB_AX88179 && !SKIP_NETWORK
	Ax88179Status usb;
	static char message[192];
	if (NetManIoctl(AX88179_IOCTL_STATUS, NULL, 0, &usb, sizeof(usb)) < 0)
		return "The USB Ethernet driver did not start. Relaunch the USB "
		       "AX88179 build and check the module error on the connect screen.";
	usb_detection_log(&usb);
	if (usb.state == AX_WAITING) {
		snprintf(message, sizeof(message),
		         "USB diag6: ready=%u probes=%u ports=%08X/%08X. "
		         "IDs=%04X:%04X %04X:%04X %04X:%04X %04X:%04X. "
		         "Log: jfusbethernet.txt",
		         usb.usb_devices_seen, usb.probe_count,
		         usb.port_status[0], usb.port_status[1],
		         usb.usb_ids[0] >> 16, usb.usb_ids[0] & 0xffff,
		         usb.usb_ids[1] >> 16, usb.usb_ids[1] & 0xffff,
		         usb.usb_ids[2] >> 16, usb.usb_ids[2] & 0xffff,
		         usb.usb_ids[3] >> 16, usb.usb_ids[3] & 0xffff);
		return message;
	}
	if (usb.state == AX_DISCONNECTED)
		return "The USB Ethernet adapter was unplugged. Reconnect it "
		       "and relaunch Jellyfin.";
	if (usb.state == AX_FAILED || usb.state == AX_CONFIGURING) {
		snprintf(message, sizeof(message),
		         "USB Ethernet setup stopped at step %d (USB result %d). "
		         "Relaunch Jellyfin and report these numbers.",
		         usb.stage, usb.usb_error);
		return message;
	}
	return "The USB adapter was detected, but Ethernet has no link. "
	       "Check its cable and the router port, then try again.";
#else
	return "Ethernet never came up. Check the cable, and that the network "
	       "adaptor is seated if this is a fat console.";
#endif
}

const char *net_address_help(void)
{
#if JFPS2_USB_AX88179 && !SKIP_NETWORK
	Ax88179Status usb;
	static char message[256];
	if (NetManIoctl(AX88179_IOCTL_STATUS, NULL, 0, &usb, sizeof(usb)) == 0) {
		usb_detection_log(&usb);
		snprintf(message, sizeof(message),
		         "DHCP: %s after %ds. USB diag6: state=%d step=%d error=%d "
		         "TX=%u RX=%u dropped=%u/%u. Log: jfusbethernet.txt. "
		         "Reconnect the stick to the PC after this screen.",
		         dhcp_phase(g_dhcp_state), g_dhcp_elapsed,
		         usb.state, usb.stage, usb.usb_error, usb.tx_frames, usb.rx_frames,
		         usb.tx_dropped, usb.rx_dropped);
		return message;
	}
#endif
	return "No IP address was obtained. Check the router's DHCP server. "
	       "For a direct cable, set network=static and ip= in settings.txt.";
}
static unsigned g_last_port_net;      /* htons result, to prove it swapped */

static long g_last_status;
static int  g_last_len;               /* body bytes actually read */
static int  g_last_expect;            /* Content-Length, -1 if absent */

/* Body bytes that arrived in the same read as the headers, for streaming. */
/*
 * Whatever arrived alongside the HTTP headers, waiting to be handed back.
 *
 * Tagged with the socket it belongs to. It used to be untagged, and
 * net_stream_read returned it whoever asked - so a stream opening while
 * another had bytes outstanding would be served the other one's opening
 * data. Several kilobytes of the wrong film, at the exact moment a decoder
 * is reading its sequence header, which is why one film played under
 * another's title.
 */
static char g_leftover[2048];
static int  g_pending, g_pending_len;
static int  g_leftover_sock = -1;
static int  g_raw_used;               /* whole response including headers */

/* Every recv return value, so the dump can show where a hole appeared. */
#define RECV_LOG_MAX 64
static int g_recv_log[RECV_LOG_MAX];
static int g_recv_count;

static NetProgress g_progress;

/* Where start-up progress lines go (the boot log). */
void net_set_progress(NetProgress fn) { g_progress = fn; }

/* The PS2's own address as text, '0.0.0.0' before the network starts. */
const char *net_local_ip(void) { return g_local_ip; }

/*
 * A one-line summary of the stack, appended to connect failures. Knowing
 * the console's own address is what tells you whether a failed connect is
 * "wrong server" or "wrong subnet".
 */
const char *net_last_info(void)
{
	static char buf[80];

	if (g_last_expect >= 0)
		snprintf(buf, sizeof(buf), "HTTP %ld, %d of %d bytes",
		         g_last_status, g_last_len, g_last_expect);
	else
		snprintf(buf, sizeof(buf), "HTTP %ld, %d bytes, no length given",
		         g_last_status, g_last_len);

	return buf;
}

/* The last raw HTTP response, headers included, for diagnostics. */
const char *net_last_raw(void)     { return g_raw; }

/* How many recv calls the last response took, for diagnostics. */
int net_recv_count(void)     { return g_recv_count; }

/* Bytes returned by the i-th recv of the last response, for diagnostics. */
int net_recv_at(int i)
{
	return (i >= 0 && i < g_recv_count && i < RECV_LOG_MAX)
	       ? g_recv_log[i] : 0;
}

/* Length of the last raw response. */
int         net_last_raw_len(void) { return g_raw_used; }

/* Length of the last response body. */
int net_last_body_len(void) { return g_last_len; }


/*
 * One line about the stack for error screens: the PS2's address and the
 * last port sent.
 */
const char *net_status(void)
{
	static char buf[64];

	if (g_last_port_net)
		snprintf(buf, sizeof(buf), "PS2 is %s, port sent 0x%04x",
		         g_local_ip, g_last_port_net);
	else
		snprintf(buf, sizeof(buf), "PS2 is %s", g_local_ip);

	return buf;
}

/* Report a start-up step through the progress callback, if any. */
static void say(const char *msg)
{
	if (g_progress)
		g_progress(msg);
}

/* The same, for other modules. */
void net_say(const char *msg) { say(msg); }

/* inet_aton is not exposed by this SDK, so parse dotted quads ourselves. */
static void str_to_ip(const char *s, struct ip4_addr *out)
{
	unsigned int a = 0, b = 0, c = 0, d = 0;

	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		a = b = c = d = 0;

	IP4_ADDR(out, a, b, c, d);
}

/*
 * SKIP_NETWORK (config.h) builds the app with no network at all, for
 * working on screens in an emulator: these stubs fail every request, so
 * the app shows its error screens instead of hanging on a missing stack.
 */
#if SKIP_NETWORK

int net_start(void) { return -100; }
void net_stop(void) { }

int http_get(const char *host, int port, const char *path,
             const char *token, int *status, char **body)
{
	(void)host; (void)port; (void)path; (void)token;
	*status = 0; *body = g_raw; g_raw[0] = '\0';
	return -99;
}

int net_http_send(const char *host, int port, const char *method,
                  const char *path, const char *body)
{
	(void)host; (void)port; (void)method; (void)path; (void)body;
	return -99;
}

int http_post(const char *host, int port, const char *path,
              const char *token, const char *json, int *status, char **body)
{
	(void)host; (void)port; (void)path; (void)token; (void)json;
	*status = 0; *body = g_raw; g_raw[0] = '\0';
	return -99;
}

#else

/* ------------------------------------------------------------------ */
/* Waiting, the way the SDK sample does it                            */
/*                                                                    */
/* Sleeping the thread on an alarm rather than spinning. A busy loop   */
/* starves the RPC to the IOP and NetManIoctl deadlocks on the second  */
/* call.                                                              */
/* ------------------------------------------------------------------ */


static void wake_cb(s32 alarm_id, u16 time, void *common)
{
	(void)alarm_id; (void)time;
	iWakeupThread(*(int *)common);
}

/*
 * Poll ready() once a second for up to 'seconds', reporting progress. 0
 * when ready, -1 on timeout.
 */
static int wait_for(int (*ready)(void), int seconds, const char *label)
{
	char msg[64];

	for (int i = 0; i < seconds; i++) {
		if (ready())
			return 0;

		snprintf(msg, sizeof(msg), "net: %s %d/%d", label, i + 1, seconds);
		say(msg);

		net_snooze(1000);
	}

	return ready() ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Sleeping on the vertical blank                                     */
/* ------------------------------------------------------------------ */

/*
 * Timed sleeps are counted in vertical blanks, not kernel alarms.
 *
 * On hardware, threads sleeping on a kernel alarm were found asleep for
 * good: the music worker after a video (step 0, loop count frozen), and the
 * artwork loader on its pause after the last poster (W9, DMA fine, alarms
 * still firing for other threads). The alarm's wake-up simply never came
 * for that one sleep.
 *
 * The vertical blank interrupt is the most dependable clock on the console:
 * it is what the picture is drawn on, and it cannot stop without the screen
 * stopping too. Each sleeper takes a slot with its own semaphore; the blank
 * handler counts the slot down and signals it at zero. A semaphore keeps
 * its count, so a signal that lands before the wait is not lost, and only
 * the slot's own handler ever signals it, so nothing else can wake the
 * sleeper early either.
 */
#define SNOOZE_SLOTS 16

static volatile int s_left[SNOOZE_SLOTS];   /* blanks still to wait, 0 idle */
static volatile int s_used[SNOOZE_SLOTS];
static int          s_sema[SNOOZE_SLOTS];
static int          s_ready;

/* Run on every vertical blank, in the interrupt: see net_set_vblank_hook. */
static void (*volatile g_vblank_hook)(void);
static void (*volatile g_vblank_hook2)(void);

/*
 * Install the first vertical-blank hook (the end-of-stream watchdog in
 * video.c).
 */
void net_set_vblank_hook(void (*fn)(void)) { g_vblank_hook = fn; }

/* Install the second vertical-blank hook (the freeze safety net). */
void net_add_vblank_hook(void (*fn)(void)) { g_vblank_hook2 = fn; }

/*
 * The vertical-blank interrupt handler, 60 times a second: run the two
 * hooks, then count down every sleeping slot and wake those that are due
 * (see net_snooze).
 */
static s32 vblank_cb(s32 cause)
{
	(void)cause;

	if (g_vblank_hook)
		g_vblank_hook();
	if (g_vblank_hook2)
		g_vblank_hook2();

	for (int i = 0; i < SNOOZE_SLOTS; i++) {
		if (s_left[i] > 0 && --s_left[i] == 0)
			iSignalSema(s_sema[i]);
	}

	ExitHandler();
	return 0;
}

/*
 * Create the sleep slots' semaphores and install the vertical-blank
 * handler. Once, at start-up, before any thread sleeps.
 */
void net_snooze_init(void)
{
	if (s_ready)
		return;

	for (int i = 0; i < SNOOZE_SLOTS; i++) {
		ee_sema_t sem;

		memset(&sem, 0, sizeof(sem));
		sem.init_count = 0;
		sem.max_count  = 1;
		s_sema[i] = CreateSema(&sem);

		if (s_sema[i] < 0)
			return;                    /* keep using alarms */
	}

	if (AddIntcHandler(INTC_VBLANK_S, vblank_cb, 0) < 0)
		return;

	EnableIntc(INTC_VBLANK_S);
	s_ready = 1;
}

/* The old way, kept for before net_snooze_init and if no slot is free. */
static void snooze_alarm(int ms)
{
	int tid = GetThreadId();

	if (ms > 4000) ms = 4000;      /* the tick count is a u16 */

	/* Never sleep on an alarm that was not registered: SleepThread only
	 * returns when something wakes it. */
	if (SetAlarm((u16)(ms * 16), &wake_cb, &tid) < 0)
		return;

	SleepThread();
}

/*
 * Sleep without spinning. A nopdelay loop keeps the EE busy, and anything
 * the stack needs to process, ARP replies included, never gets a chance to
 * run. That was why a connect retry blocked: the delay between attempts
 * prevented the resolution it was waiting for.
 */
void net_snooze(int ms)
{
	if (ms < 16)
		ms = 16;

	if (!s_ready) {
		snooze_alarm(ms);
		return;
	}

	int slot = -1;
	int oi   = DIntr();

	for (int i = 0; i < SNOOZE_SLOTS; i++) {
		if (!s_used[i]) {
			s_used[i] = 1;
			slot = i;
			break;
		}
	}

	if (oi)
		EIntr();

	if (slot < 0) {
		snooze_alarm(ms);
		return;
	}

	/* Whole blanks, rounded up: 60 a second. */
	int blanks = (ms * 60 + 999) / 1000;

	if (blanks < 1)
		blanks = 1;

	PollSema(s_sema[slot]);            /* nothing left over from before */
	s_left[slot] = blanks;

	WaitSema(s_sema[slot]);

	s_used[slot] = 0;
}

/* Whether the Ethernet cable has a link. */
static int link_is_up(void)
{
	return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0)
	       == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

/* DHCP must have a bound, nonzero address. OFF is not a successful lease. */
static int dhcp_is_bound(void)
{
	t_ip_info info;

	if (ps2ip_getconfig("sm0", &info) != 1) {
		g_dhcp_state = -1;
		return 0;
	}
	g_dhcp_state = info.dhcp_status;

	return info.dhcp_status == DHCP_STATE_BOUND && info.ipaddr.s_addr != 0;
}

/* Allow a lease received near the deadline to finish its ACD probes.
 * A CHECKING state is evidence of a server reply, not a completed lease. */
static int wait_for_dhcp(int seconds)
{
	int limit = seconds, previous = -2;
	char msg[96];
	g_dhcp_trace[0] = '\0';
	for (g_dhcp_elapsed = 0; g_dhcp_elapsed <= limit; ++g_dhcp_elapsed) {
		int bound = dhcp_is_bound();
		if (g_dhcp_state != previous) {
			size_t used = strlen(g_dhcp_trace);
			snprintf(g_dhcp_trace + used, sizeof(g_dhcp_trace) - used,
			         "%s%d:%d", used ? "," : "", g_dhcp_elapsed, g_dhcp_state);
			previous = g_dhcp_state;
		}
		if (bound) return 0;
		if (g_dhcp_elapsed == seconds && g_dhcp_state == DHCP_STATE_CHECKING)
			limit += DHCP_CHECK_TIMEOUT;
		if (g_dhcp_elapsed == limit) return -1;
		snprintf(msg, sizeof(msg), "net: DHCP %s %d/%d",
		         dhcp_phase(g_dhcp_state), g_dhcp_elapsed + 1, limit);
		say(msg);
		net_snooze(1000);
	}
	return -1;
}

/* How the address was had, for the connect screen. */
static const char *g_net_how = "";

/*
 * How the address was obtained ('static', 'from router'...), for the
 * connect screen.
 */
const char *net_how(void) { return g_net_how; }

/* settings.txt's static address, onto an interface already running. */
static int set_static(void)
{
	struct ip4_addr ip, nm, gw;
	t_ip_info info;

	if (ps2ip_getconfig("sm0", &info) != 1)
		return -4;

	str_to_ip(settings.ip,   &ip);
	str_to_ip(settings.mask[0] ? settings.mask : "255.255.255.0", &nm);
	str_to_ip(settings.gateway[0] ? settings.gateway : settings.ip, &gw);

	info.dhcp_enabled = 0;
	memcpy(&info.ipaddr,  &ip, sizeof(ip));
	memcpy(&info.netmask, &nm, sizeof(nm));
	memcpy(&info.gw,      &gw, sizeof(gw));
	ps2ip_setconfig(&info);

	dns_setserver(0, &gw);
	return 0;
}

/* Copy the interface's address into net_local_ip's text. */
static void record_local_ip(void)
{
	t_ip_info info;

	if (ps2ip_getconfig("sm0", &info) != 1)
		return;

	/*
	 * Copy the address out as bytes rather than casting the struct, which
	 * is what the ip4_addr1..4 macros do and what gcc warns about. The
	 * value is in network byte order, so byte 0 is the first octet.
	 */
	unsigned char o[4];
	memcpy(o, &info.ipaddr, 4);

	snprintf(g_local_ip, sizeof(g_local_ip), "%d.%d.%d.%d",
	         o[0], o[1], o[2], o[3]);
}

/*
 * Order follows $PS2SDK/samples/network/tcpip-dhcp:
 *   NetManInit -> link mode -> wait for link -> ps2ipInit -> lease.
 *
 * Returns 0 on success. -1 init failed, -2 no link, -4 no address.
 */
int net_start(void)
{
	struct ip4_addr ip, nm, gw;
	char msg[64];
	int rc;

	say("net: NetManInit");
	rc = NetManInit();
	if (rc != 0) {
		snprintf(msg, sizeof(msg), "net: NetManInit failed (%d)", rc);
		say(msg);
		return -1;
	}

#if !JFPS2_USB_AX88179
	say("net: link mode auto");
	rc = NetManSetLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);
	if (rc != 0) {
		/* Not fatal. Some PHYs reject the change and work anyway. */
		snprintf(msg, sizeof(msg), "net: link mode rejected (%d)", rc);
		say(msg);
	}
#endif

#if REQUIRE_LINK
	/* Catch an unplugged cable here, where the message can say so. */
	if (wait_for(link_is_up, LINK_TIMEOUT, "link") != 0) {
		say("net: no link");
		return -2;
	}
	say("net: link up");
#endif

	/*
	 * settings.txt decides: static is set at once; dhcp asks the router;
	 * auto asks too, but only briefly, then falls back to the static
	 * lines - so one stick works on a router and on a direct cable.
	 */
	if (settings.network == NET_STATIC) {
		if (!settings.ip[0])
			return -5;

		str_to_ip(settings.ip, &ip);
		str_to_ip(settings.mask[0] ? settings.mask : "255.255.255.0", &nm);
		str_to_ip(settings.gateway[0] ? settings.gateway : settings.ip, &gw);

		say("net: ps2ipInit (static)");
		ps2ipInit(&ip, &nm, &gw);
		dns_setserver(0, &gw);
		g_net_how = "static";
	} else {
		IP4_ADDR(&ip, 0, 0, 0, 0);
		IP4_ADDR(&nm, 0, 0, 0, 0);
		IP4_ADDR(&gw, 0, 0, 0, 0);

		say("net: ps2ipInit");
		ps2ipInit(&ip, &nm, &gw);

		t_ip_info info;

		if (ps2ip_getconfig("sm0", &info) != 1) {
			say("net: sm0 not registered");
			return -4;
		}

		info.dhcp_enabled = 1;
		ps2ip_setconfig(&info);

		/*
		 * auto waits briefly only when it has a static address to fall
		 * back to. Without one it is plain dhcp, with dhcp's patience.
		 */
		int fallback = settings.network == NET_AUTO && settings.ip[0];
		int wait     = fallback ? AUTO_DHCP_TIMEOUT :
		               (JFPS2_USB_AX88179 ? USB_DHCP_TIMEOUT : DHCP_TIMEOUT);

		say("net: asking router for an address");

		if (wait_for_dhcp(wait) == 0) {
			g_net_how = "from router";
		} else if (fallback) {
			say("net: no router, using static address");

			if (set_static() < 0)
				return -4;

			g_net_how = "static, no router";
		} else {
			say("net: no address");
			return -4;
		}
	}

#if FORCE_NETIF_UP
	say("net: forcing netif up");
	{
		struct netif *nif = netif_list;
		while (nif) { netif_set_link_up(nif); netif_set_up(nif); nif = nif->next; }
	}
#endif

	record_local_ip();

	snprintf(msg, sizeof(msg), "net: ready at %s", g_local_ip);
	say(msg);
	return 0;
}

/* ------------------------------------------------------------------ */

/* Shared by http_get and http_get_range. extra may be NULL. */
static int request(const char *host, int port, const char *path,
                   const char *token, const char *extra, const char *json,
                   int *status, char **body);

/* Shut the network stack down before quitting to the browser. */
void net_stop(void)
{
	/* Mirrors the teardown in $PS2SDK/samples/network/tcpip-basic, so the
	 * stack is not left running when we hand control back. */
	ps2ipDeinit();
	NetManDeinit();
}

/*
 * A plain GET into the shared response buffer. Returns 0 or a negative
 * error; *status gets the HTTP code and *body the start of the body.
 */
int http_get(const char *host, int port, const char *path,
             const char *token, int *status, char **body)
{
	return request(host, port, path, token, NULL, NULL, status, body);
}

/* The same as a POST with a JSON body (signing in). */
int http_post(const char *host, int port, const char *path,
              const char *token, const char *json, int *status, char **body)
{
	return request(host, port, path, token, NULL, json ? json : "{}",
	               status, body);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                          */
/* ------------------------------------------------------------------ */

static int connect_to(const char *host, int port)
{
	struct ip4_addr hip;
	str_to_ip(host, &hip);
	if (hip.addr == 0)
		return -2;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family      = AF_INET;
	sa.sin_port        = htons(port);
	sa.sin_addr.s_addr = hip.addr;

	for (int attempt = 0; attempt < 3; attempt++) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
			return -1;

		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0)
			return s;

		close(s);
		net_snooze(250);
	}

	return -3;
}

/*
 * Open a long response for streaming (video, music, subtitles): send the
 * GET, read the headers, check for 200. Returns the socket, or negative.
 */
int net_stream_open(const char *host, int port, const char *path,
                    const char *token)
{
	return net_stream_open_wait(host, port, path, token, 1000);
}

/* net_stream_open, waiting up to wait_ms for the reply to start. */
int net_stream_open_wait(const char *host, int port, const char *path,
                         const char *token, int wait_ms)
{
	int s = connect_to(host, port);
	if (s < 0)
		return s;

	static char req[2048];
	int n;

	{
		char auth[320];

		auth_header(auth, sizeof(auth), token);
		n = snprintf(req, sizeof(req),
		    "GET %s HTTP/1.0\r\nHost: %s:%d\r\n%sAccept: */*\r\n\r\n",
		    path, host, port, auth);
	}

	if (send(s, req, n, 0) < 0) {
		close(s);
		return -4;
	}

	/*
	 * A receive timeout, so a read cannot block forever.
	 *
	 * Closing a socket does not wake a thread already blocked in recv on
	 * this stack, so a decoder thread waiting for video could not be got
	 * out of it at all - it stayed inside MPEG_Picture, and nothing could
	 * then reset the IPU or destroy the session safely.
	 *
	 * A second is long enough that it never fires during playback and
	 * short enough that stopping is prompt. SO_RCVBUF is rejected by this
	 * stack; SO_RCVTIMEO is not.
	 */
	{
		struct timeval tv;

		tv.tv_sec  = wait_ms / 1000;
		tv.tv_usec = (wait_ms % 1000) * 1000;

		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	/*
	 * Read just far enough to find the end of the headers, then leave the
	 * rest in the socket. Anything already read past the blank line is
	 * body, so it is stashed and returned by the first net_stream_read.
	 */
	static char hdr[2048];
	int used = 0;
	char *end = NULL;

	while (used < (int)sizeof(hdr) - 1) {
		int r = recv(s, hdr + used, sizeof(hdr) - 1 - used, 0);
		if (r <= 0)
			break;

		used += r;
		hdr[used] = '\0';

		end = strstr(hdr, "\r\n\r\n");
		if (end)
			break;
	}

	if (!end) {
		close(s);
		return -5;
	}

	if (strncmp(hdr, "HTTP/", 5) == 0 && atoi(hdr + 9) != 200) {
		close(s);
		return -6;
	}

	g_pending     = (end + 4) - hdr;
	g_pending_len = used - g_pending;

	if (g_pending_len > 0)
		memcpy(g_leftover, hdr + g_pending, g_pending_len);
	else
		g_pending_len = 0;

	g_pending       = 0;
	g_leftover_sock = s;

	return s;
}

/*
 * Read the next bytes of a stream: first any body bytes that arrived with
 * the headers, then from the socket. 0 when the server closed it, negative
 * on a timeout.
 */
int net_stream_read(int sock, void *buf, int len)
{
	/* Hand back whatever came in alongside the headers first - but only
	 * to the stream it actually belongs to. */
	if (g_pending_len > 0 && sock == g_leftover_sock) {
		int n = (g_pending_len < len) ? g_pending_len : len;

		memcpy(buf, g_leftover + g_pending, n);
		g_pending     += n;
		g_pending_len -= n;

		if (g_pending_len == 0)
			g_pending = 0;

		return n;
	}

	return recv(sock, buf, len, 0);
}

/*
 * Shorten a stream's receive timeout once it is open.
 *
 * The open keeps the long one, since Jellyfin can take a moment to send the
 * headers of a fresh transcode. Afterwards a short one means a thread
 * blocked in recv sees a stop request within that time rather than after a
 * full second: closing the socket does not wake it on this stack.
 */
void net_stream_set_timeout(int sock, int ms)
{
	struct timeval tv;

	if (sock < 0)
		return;

	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Close a stream and drop any leftover header bytes that belonged to it. */
void net_stream_close(int sock)
{
	/* Do not leave this stream's opening bytes for the next one. */
	if (sock == g_leftover_sock) {
		g_pending       = 0;
		g_pending_len   = 0;
		g_leftover_sock = -1;
	}

	if (sock >= 0)
		close(sock);
}

/*
 * A request whose reply body is not needed (reports): send, read the
 * status line, close. Returns the HTTP status or negative.
 */
int net_http_send(const char *host, int port, const char *method,
                  const char *path, const char *body)
{
	int s = connect_to(host, port);

	if (s < 0)
		return s;

	static char req[1024];
	int blen = body ? (int)strlen(body) : 0;

	int n = snprintf(req, sizeof(req),
	    "%s %s HTTP/1.0\r\n"
	    "Host: %s:%d\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: %d\r\n"
	    "Accept: */*\r\n\r\n%s",
	    method, path, host, port, blen, body ? body : "");

	if (n <= 0 || n >= (int)sizeof(req) || send(s, req, n, 0) < 0) {
		close(s);
		return -4;
	}

	/* Two seconds for the status line; nothing else is needed. */
	{
		struct timeval tv;

		tv.tv_sec  = 2;
		tv.tv_usec = 0;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	char line[64];
	int  got = recv(s, line, sizeof(line) - 1, 0);

	close(s);

	if (got < 12 || strncmp(line, "HTTP/", 5) != 0)
		return -5;

	line[got] = '\0';
	return atoi(line + 9);
}

/*
 * A GET of one byte range (Range header), for fetching large replies in
 * slices that the network stack delivers reliably.
 */
int http_get_range(const char *host, int port, const char *path,
                   const char *token, long start, int len,
                   char *out, int *status, long *total)
{
	char extra[64];
	char *body;

	snprintf(extra, sizeof(extra), "Range: bytes=%ld-%ld\r\n",
	         start, start + len - 1);

	int rc = request(host, port, path, token, extra, NULL, status, &body);
	if (rc < 0)
		return rc;

	/* 206 with a range, or 200 if the server ignored the header. */
	if (*status != 206 && *status != 200)
		return -7;

	*total = -1;
	{
		const char *cr = strstr(g_raw, "Content-Range:");
		if (!cr) cr = strstr(g_raw, "content-range:");

		if (cr) {
			const char *slash = strchr(cr, '/');
			if (slash) *total = atol(slash + 1);
		}
	}

	int n = g_last_len;
	if (n > len) n = len;
	if (n > 0) memcpy(out, body, n);

	return n;
}

/*
 * The server's date, from the Date: header every Jellyfin reply carries
 * ("Date: Thu, 15 Oct 2026 01:59:53 GMT"). The PS2's own clock is often
 * wrong - a flat battery, or never set - and the server's is right by
 * definition for the requests that need a date (Next Up's cutoff).
 */
static int g_srv_y, g_srv_m, g_srv_d;     /* 0 until a reply had one */

static void note_server_date(const char *head, int len)
{
	static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";

	for (int i = 0; i + 30 < len; i++) {
		/* The header name, case-insensitively, at a line start. */
		if (!(i == 0 || head[i - 1] == '\n'))
			continue;
		if (strncasecmp(head + i, "Date:", 5) != 0)
			continue;

		/* "Date: Thu, 15 Oct 2026 ..." - day, month name, year. */
		const char *p = strchr(head + i, ',');

		if (!p || p - head > len)
			return;

		int d = 0, y = 0;
		char m[4] = "";

		if (sscanf(p + 1, " %d %3s %d", &d, m, &y) != 3)
			return;

		const char *f = strstr(mon, m);

		if (!f || m[0] == '\0' || y < 2000 || d < 1 || d > 31)
			return;

		g_srv_y = y;
		g_srv_m = (int)(f - mon) / 3 + 1;
		g_srv_d = d;
		return;
	}
}

int net_server_date(int *y, int *m, int *d)
{
	if (!g_srv_y)
		return 0;

	*y = g_srv_y;
	*m = g_srv_m;
	*d = g_srv_d;
	return 1;
}

/*
 * The one HTTP/1.0 request routine behind http_get, http_post and
 * http_get_range: connect, send the request with the Authorization header,
 * read the whole reply into the shared buffer, split headers from body.
 * Not re-entrant: callers are on the screen thread.
 */
static int request(const char *host, int port, const char *path,
                   const char *token, const char *extra, const char *json,
                   int *status, char **body)
{
	*status  = 0;
	*body    = g_raw;
	g_raw[0] = '\0';

	struct ip4_addr hip;
	str_to_ip(host, &hip);          /* no DNS, so this must be a dotted quad */
	if (hip.addr == 0)
		return -2;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family      = AF_INET;
	sa.sin_port        = htons(port);
	sa.sin_addr.s_addr = hip.addr;

	/* 8096 is 0x1fa0, so a working htons gives 0xa01f. Confirmed correct on
	 * hardware; kept because a byte-order bug here looks like nothing else. */
	g_last_port_net = (unsigned)sa.sin_port & 0xFFFF;

	/*
	 * Connect, with retries.
	 *
	 * The first attempt after the stack comes up reliably fails: lwIP has
	 * no ARP entry for the peer, so the SYN is dropped while the address
	 * resolves and a blocking connect gives up rather than waiting. The
	 * next attempt finds the entry cached.
	 *
	 * Each attempt gets a fresh socket. A failed connect leaves the old one
	 * in SYN_SENT, and those accumulate until lwIP runs out of PCBs, which
	 * is what made the third browse session hang.
	 */
	/*
	 * Nudge ARP, once per host.
	 *
	 * A UDP send queues a packet and returns immediately, which starts
	 * resolution without blocking, so the TCP connect that follows finds the
	 * entry cached instead of losing its first SYN to it.
	 *
	 * Only the first time, though. A capture showed this firing before every
	 * request and drawing an ICMP port unreachable each time, which is a
	 * pointless round trip once the entry exists.
	 */
	{
		static u32 nudged_addr;

		if (nudged_addr != hip.addr) {
			int u = socket(AF_INET, SOCK_DGRAM, 0);

			if (u >= 0) {
				sendto(u, "", 1, 0, (struct sockaddr *)&sa, sizeof(sa));
				close(u);
				net_snooze(250);   /* yields, so the reply can arrive */
			}
			nudged_addr = hip.addr;
		}
	}

	int s = -1;
	char dbg[64];

	for (int attempt = 0; attempt < 4; attempt++) {
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
			return -1;

		/* Bound the blocking calls. Not every lwIP build honours these. */
		{
			struct timeval tv;
			tv.tv_sec  = 5;
			tv.tv_usec = 0;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

			/* A larger receive buffer, if this build supports it.
			 * Probably a no-op: the 2047-byte cap this was meant to
			 * lift turned out not to exist. Harmless to ask. */
			int rb = HTTP_RCVBUF;
			setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
		}

		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0)
			break;

		close(s);
		s = -1;

		snprintf(dbg, sizeof(dbg), "net: connect retry %d/4", attempt + 2);
		say(dbg);

		/* Yielding, so ARP can actually complete while we wait. */
		net_snooze(500);
	}

	if (s < 0)
		return -3;

	static char req[2048];
	int n;

	/*
	 * HTTP/1.0 on purpose: no chunked transfer encoding, so the body is
	 * always plain and close-delimited. Verified against Kestrel.
	 */
	const char *ex = extra ? extra : "";

	char auth[320];

	auth_header(auth, sizeof(auth), token);

	if (json) {
		/* A POST with a JSON body: signing in, signing out. */
		n = snprintf(req, sizeof(req),
		    "POST %s HTTP/1.0\r\n"
		    "Host: %s:%d\r\n"
		    "%s%s"
		    "Content-Type: application/json\r\n"
		    "Content-Length: %d\r\n"
		    "Accept: */*\r\n\r\n%s",
		    path, host, port, auth, ex, (int)strlen(json), json);
	} else {
		n = snprintf(req, sizeof(req),
		    "GET %s HTTP/1.0\r\n"
		    "Host: %s:%d\r\n"
		    "%s%s"
		    "Accept: */*\r\n\r\n",
		    path, host, port, auth, ex);
	}

	if (n >= (int)sizeof(req)) { close(s); return -4; }

	if (send(s, req, n, 0) < 0) { close(s); return -4; }

	int used = 0;
	g_recv_count = 0;

	for (;;) {
		/*
		 * Take as much as possible per call. The receive mailbox is the
		 * bottleneck: when it fills, lwIP drops already-acknowledged
		 * data. Draining in large gulps keeps it emptier.
		 */
		int want = HTTP_RAW_MAX - 1 - used;
		if (want > HTTP_CHUNK) want = HTTP_CHUNK;
		if (want <= 0) break;

		int r = recv(s, g_raw + used, want, 0);

		if (g_recv_count < RECV_LOG_MAX)
			g_recv_log[g_recv_count++] = r;

		if (r <= 0) break;
		used += r;
	}
	g_raw[used] = '\0';
	g_raw_used  = used;
	close(s);

	if (used < 12 || strncmp(g_raw, "HTTP/", 5) != 0) {
		g_last_status = 0;
		g_last_len    = used;
		g_last_expect = -1;
		return -5;
	}

	*status       = atoi(g_raw + 9);      /* "HTTP/1.0 200 OK" */
	g_last_status = *status;

	/*
	 * Compare what arrived against what was promised. Without this a
	 * truncated body looks like a valid 200 with unparseable JSON, which
	 * is a much more confusing thing to debug.
	 */
	g_last_expect = -1;
	{
		const char *cl = strstr(g_raw, "Content-Length:");
		if (!cl) cl = strstr(g_raw, "content-length:");
		if (cl)  g_last_expect = atoi(cl + 15);
	}

	char *end = strstr(g_raw, "\r\n\r\n");

	if (end)
		note_server_date(g_raw, (int)(end - g_raw));
	*body = end ? end + 4 : g_raw + used;

	g_last_len = end ? (int)(used - (end + 4 - g_raw)) : 0;

	/* Promised more than it delivered. */
	if (g_last_expect > 0 && g_last_len < g_last_expect)
		return -6;

	/*
	 * Kestrel sends no Content-Length for HTTP/1.0, so length cannot prove
	 * completeness. For JSON it can be proven directly: a complete document
	 * ends with a closing brace or bracket.
	 *
	 * Only for JSON, though. A JPEG ends with 0xD9, and applying this to
	 * binary rejected every image ever fetched. So look at the first byte
	 * and only judge documents that claim to be JSON.
	 */
	if (g_last_len > 0 && !extra) {      /* a range reply is partial by design */
		const char *b = *body;

		int f = 0;
		while (f < g_last_len && (b[f] == '\n' || b[f] == '\r' ||
		                          b[f] == ' '  || b[f] == '\t'))
			f++;

		int looks_like_json = (f < g_last_len &&
		                       (b[f] == '{' || b[f] == '['));

		if (looks_like_json) {
			int i = g_last_len - 1;

			while (i > 0 && (b[i] == '\n' || b[i] == '\r' ||
			                 b[i] == ' '  || b[i] == '\t'))
				i--;

			if (b[i] != '}' && b[i] != ']')
				return -6;              /* truncated mid-document */
		}
	}

	return 0;
}

#endif /* SKIP_NETWORK */
