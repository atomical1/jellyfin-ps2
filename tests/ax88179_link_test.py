"""Exercise the driver's actual link-up sequence with a mock controller."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'iop/ax88179/driver.c').read_text()

def function(name):
    start = source.index('static int ' + name + '(')
    end = source.index('\n}\n', start) + 3
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct { int stage, usb_error; } status;
static struct { unsigned int restarts, fifo_status, rx_control, medium; } registers;
static unsigned char mac[6] = {2,3,4,5,6,7};
static int calls, fail_at, busy, stops, starts, queues, mac_writes, sleeps;
static int medium_writes, snapshots, snapshot_fail, readback_stopped, order;
static unsigned short requested_medium;
static int write16(int reg, unsigned short value)
{
    if (++calls == fail_at) return -1;
    if (reg == 0x0b && value == 0) { assert(order == 0); order = 1; stops++; }
    else if (reg == 0x0b && value == 0x01aa) { assert(order == 3); order = 4; starts++; }
    else { assert(reg == 0x22 && order == 0); medium_writes++; requested_medium = value; }
    return 0;
}
static int control(unsigned char type, unsigned char command, unsigned short value,
                   unsigned short index, void *data, unsigned int length)
{
    if (++calls == fail_at) return -1;
    if (type == 0x40 && command == 1 && value == 0x2e) {
        const unsigned char expected[5] = {7,0,0,0,8};
        assert(order == 1 && index == 5 && length == 5);
        assert(!memcmp(data, expected, 5)); order = 2; queues++;
    } else if (type == 0x40 && command == 1 && value == 0x10) {
        assert(order == 2 && index == 6 && length == 6);
        assert(!memcmp(data, mac, 6)); order = 3; mac_writes++;
    } else {
        assert(type == 0xc0 && command == 0x81 && value == 0x8c);
        assert(order == 4 && index == 0 && length == 4);
        memset(data, 0, 4);
        if (busy > 0) { ((unsigned char *)data)[3] = 0x40; busy--; }
        order = 0;
    }
    return 0;
}
static void DelayThread(int us) { assert(us == 1000); sleeps++; }
static int snapshot_registers(void)
{
    snapshots++;
    if (snapshot_fail) return -1;
    registers.rx_control = readback_stopped ? 0 : 0x01aa;
    registers.medium = requested_medium;
    return 0;
}
static void reset(void)
{
    memset(&status,0,sizeof(status)); memset(&registers,0,sizeof(registers));
    calls=fail_at=busy=stops=starts=queues=mac_writes=sleeps=0;
    medium_writes=snapshots=snapshot_fail=readback_stopped=order=0;
    requested_medium=0;
}
'''
suffix = r'''
int main(void)
{
    reset();
    assert(restart_receiver(0x013b) == 0);
    assert(stops==1 && starts==1 && queues==1 && mac_writes==1);
    assert(medium_writes==1 && requested_medium==0x013b && snapshots==1);
    assert(status.stage==7 && registers.restarts==1);
    for (int fail=1; fail<=6; fail++) {
        reset(); fail_at=fail;
        assert(restart_receiver(0x013b) == -1 && snapshots==0);
    }
    reset(); busy=3;
    assert(restart_receiver(0x0332)==0 && sleeps==3 && stops==4);
    assert(requested_medium==0x0332 && !registers.fifo_status);
    reset(); busy=1000;
    assert(restart_receiver(0x013b)==-1 && status.usb_error==-5);
    assert(stops==100 && sleeps==100 && !medium_writes && !snapshots);
    reset(); snapshot_fail=1;
    assert(restart_receiver(0x013b)==-1 && snapshots==1);
    reset(); readback_stopped=1;
    assert(restart_receiver(0x013b)==-1 && status.usb_error==-6);
    puts("AX88179 link tests passed: receive restart, FIFO drain, bounded retries, control failures and readback.");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ax88179-link-') as folder:
    test = Path(folder) / 'link.c'
    binary = Path(folder) / 'link'
    test.write_text(prefix + function('configure_receive_queue') + function('restart_receiver') + suffix)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
