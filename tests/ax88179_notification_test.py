"""Exercise the actual firmware-notification service without hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'iop/ax88179/driver.c').read_text()
start = source.index('static int service_notifications(void)')
end = source.index('\n}\n', start) + 3
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define USB_RC_OK 0
#define USB_RC_DATAUNDER 9
#define AX_READY 2
static struct { int done, result, count; } notification_io;
static struct { int wMaxPacketSizeLB; } notification_endpoint;
static struct { unsigned int notifications, last_notification; } registers;
static struct { int usb_error, state; } status;
static int notification_pipe=7, notification_pending, link_up;
static unsigned char notification_buffer[64];
static int arms, updates, update_fail, transfer_result;
static void complete(int result, int count, void *arg)
{ (void)result; (void)count; (void)arg; }
static void set_link(int up) { link_up=up; }
static int update_link(int force)
{ assert(force==1); updates++; if(update_fail) return -1; link_up=1; return 0; }
static int sceUsbdTransferPipe(int pipe, void *data, int size, void *option,
                              void (*callback)(int,int,void *), void *arg)
{
    assert(pipe==7 && data==notification_buffer && size==notification_endpoint.wMaxPacketSizeLB);
    assert(!option && callback==complete && arg==&notification_io);
    arms++; return transfer_result;
}
static void reset(void)
{
    memset(&notification_io,0,sizeof(notification_io)); memset(&registers,0,sizeof(registers));
    memset(&status,0,sizeof(status)); memset(notification_buffer,0,sizeof(notification_buffer));
    notification_pending=link_up=arms=updates=update_fail=transfer_result=0;
    notification_endpoint.wMaxPacketSizeLB=16;
}
static void event(int up, int bytes)
{
    notification_pending=1; notification_io.done=1; notification_io.count=bytes;
    notification_buffer[2]=up ? 1 : 0;
}
'''
suffix = r'''
int main(void)
{
    reset();
    assert(service_notifications()==0 && arms==1 && !updates);
    assert(service_notifications()==0 && arms==1 && !updates); /* no PHY polling while pending */
    event(1,8);
    assert(service_notifications()==0 && updates==1 && link_up && arms==2);
    assert(registers.notifications==1 && registers.last_notification==0x10000);
    event(1,16);
    assert(service_notifications()==0 && updates==1); /* duplicate event does not reconfigure */
    event(0,8);
    assert(service_notifications()==0 && !link_up && status.state==AX_READY);
    event(1,8);
    assert(service_notifications()==0 && updates==2 && link_up);
    reset(); event(1,7);
    assert(service_notifications()==-1 && status.usb_error==-7 && !updates);
    reset(); event(1,17);
    assert(service_notifications()==-1 && status.usb_error==-7 && !updates);
    reset(); event(1,8); notification_io.result=9;
    assert(service_notifications()==0 && updates==1); /* valid short interrupt completion */
    reset(); event(1,8); notification_io.result=4;
    assert(service_notifications()==-1 && status.usb_error==4 && !updates);
    reset(); transfer_result=3;
    assert(service_notifications()==-1 && status.usb_error==3);
    reset(); event(1,8); update_fail=1;
    assert(service_notifications()==-1 && !arms);
    puts("AX88179 notification tests passed: pending idle, up/down, duplicate suppression, 8/16-byte events and errors.");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ax88179-notify-') as folder:
    test = Path(folder) / 'notify.c'
    binary = Path(folder) / 'notify'
    test.write_text(prefix + source[start:end] + suffix)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
