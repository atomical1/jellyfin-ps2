"""Run the actual DHCP wait code against timed offers and ACD completion."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/net.c').read_text()
config = (root / 'src/config.h').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    return source[start:end]

constants = '\n'.join(re.findall(r'^#define (?:USB_DHCP_TIMEOUT|DHCP_CHECK_TIMEOUT)\s+\d+.*$', config, re.M)) + '\n'
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DHCP_STATE_OFF 0
#define DHCP_STATE_REQUESTING 1
#define DHCP_STATE_SELECTING 6
#define DHCP_STATE_CHECKING 8
#define DHCP_STATE_BOUND 10
#define DHCP_STATE_BACKING_OFF 12
typedef struct { struct { unsigned int s_addr; } ipaddr; int dhcp_status; } t_ip_info;
static int g_dhcp_state, g_dhcp_elapsed;
static char g_dhcp_trace[256];
static int clock_seconds, offer_at, bind_at, missing, forced_state, valid_ip;
static int ps2ip_getconfig(const char *name, t_ip_info *info)
{
    assert(!strcmp(name,"sm0"));
    memset(info,0,sizeof(*info));
    if (missing) return 0; /* This SDK returns zero, not -1, on failure. */
    info->dhcp_status = forced_state >= 0 ? forced_state :
        clock_seconds >= bind_at ? DHCP_STATE_BOUND :
        clock_seconds >= offer_at ? DHCP_STATE_CHECKING : DHCP_STATE_SELECTING;
    if (info->dhcp_status == DHCP_STATE_BOUND && valid_ip) info->ipaddr.s_addr=0x2a01a8c0;
    return 1;
}
static void net_snooze(int ms) { assert(ms==1000); clock_seconds++; }
static void say(const char *msg) { assert(strstr(msg,"net: DHCP ")==msg); }
static void reset(void)
{
    g_dhcp_state=-1; g_dhcp_elapsed=clock_seconds=missing=0;
    offer_at=bind_at=1000; forced_state=-1; valid_ip=1;
}
'''
suffix = r'''
int main(void)
{
    reset(); offer_at=4; bind_at=14;
    assert(wait_for_dhcp(USB_DHCP_TIMEOUT)==0 && clock_seconds==14);
    assert(strstr(g_dhcp_trace,"4:8,14:10"));
    reset(); offer_at=USB_DHCP_TIMEOUT-1; bind_at=USB_DHCP_TIMEOUT+8;
    assert(wait_for_dhcp(USB_DHCP_TIMEOUT)==0 && clock_seconds==bind_at);
    reset();
    assert(wait_for_dhcp(USB_DHCP_TIMEOUT)==-1 && clock_seconds==USB_DHCP_TIMEOUT);
    reset(); offer_at=1;
    assert(wait_for_dhcp(USB_DHCP_TIMEOUT)==-1);
    assert(clock_seconds==USB_DHCP_TIMEOUT+DHCP_CHECK_TIMEOUT);
    reset(); offer_at=4; bind_at=9;
    assert(wait_for_dhcp(5)==0 && clock_seconds==9); /* auto-mode gets ACD grace too */
    reset(); missing=1;
    assert(wait_for_dhcp(3)==-1 && g_dhcp_state==-1);
    reset(); forced_state=DHCP_STATE_OFF;
    assert(wait_for_dhcp(3)==-1);
    reset(); forced_state=DHCP_STATE_BOUND; valid_ip=0;
    assert(wait_for_dhcp(3)==-1);
    reset(); forced_state=DHCP_STATE_BOUND;
    assert(wait_for_dhcp(USB_DHCP_TIMEOUT)==0 && clock_seconds==0);
    puts("DHCP wait tests passed: delayed lease, ACD grace, bounded failure, missing interface and zero-IP rejection.");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='jfps2-dhcp-') as folder:
    test = Path(folder) / 'wait.c'
    binary = Path(folder) / 'wait'
    test.write_text(constants + prefix + function('static const char *dhcp_phase(') +
                    function('static int dhcp_is_bound(') + function('static int wait_for_dhcp(') + suffix)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
