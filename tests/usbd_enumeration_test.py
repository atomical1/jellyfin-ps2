"""Compile and exercise the actual USBD config-fetch function on the host."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path)
args = parser.parse_args()
source = args.source or Path(__file__).resolve().parents[1] / 'third_party/ps2sdk-usbd/src/device.c'
text = source.read_text()
start = text.index('static void fetchConfigDescriptors(UsbdIoRequest_t *req)\n{')
end = text.index('\nstatic void requestDeviceDescrptorCB', start)
function = text[start:end]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef struct {
    u8 bLength, bDescriptorType; uint16_t bcdUSB;
    u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    u8 iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} UsbDeviceDescriptor;
typedef struct {
    u8 bLength, bDescriptorType; uint16_t wTotalLength;
    u8 bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, maxPower;
} UsbConfigDescriptor;
typedef struct Device UsbdDevice_t;
typedef struct { UsbdDevice_t *m_correspDevice; } Endpoint;
typedef struct Request {
    Endpoint *m_correspEndpoint;
    int m_resultCode;
} UsbdIoRequest_t;
struct Device {
    void *m_staticDeviceDescPtr, *m_staticDeviceDescEndPtr;
    unsigned int m_fetchDescriptorCounter;
    void *m_reportDescriptorStart, *m_reportDescriptorCurForFetch;
    UsbdIoRequest_t m_ioRequest;
};
static struct { unsigned int m_maxStaticDescSize; int m_curDescNum; } usbConfig;
#define USB_RC_OK 0
#define USB_DIR_IN 128
#define USB_RECIP_DEVICE 0
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_DT_CONFIG 2
#define READ_UINT16(p) (((u8 *)(p))[0] | (((u8 *)(p))[1] << 8))
#define dbg_printf(...) ((void)0)
static int ready, killed, pending, requested_length, requested_config;
static void *requested_destination;
static void (*continuation)(UsbdIoRequest_t *);
static void killDevice(UsbdDevice_t *dev, Endpoint *ep) { (void)dev; (void)ep; killed++; }
static void connectNewDevice(UsbdDevice_t *dev) { (void)dev; ready++; }
static void fetchNextReportDescriptor(UsbdIoRequest_t *req) { (void)req; assert(0); }
static void handleStaticDeviceDescriptor(UsbdDevice_t *dev, UsbDeviceDescriptor *a, UsbDeviceDescriptor *b)
{ (void)dev; (void)a; (void)b; }
static int doControlTransfer(Endpoint *ep, UsbdIoRequest_t *req, int type, int command,
                            int value, int index, int length, void *destination,
                            void (*callback)(UsbdIoRequest_t *))
{
    (void)ep; (void)req; (void)index;
    assert(type==USB_DIR_IN && command==USB_REQ_GET_DESCRIPTOR && !pending);
    assert((value>>8)==USB_DT_CONFIG);
    requested_config=value & 255; requested_length=length;
    requested_destination=destination; continuation=callback; pending=1;
    return 0;
}
'''
suffix = r'''
static int check_configurations(int number)
{
    unsigned char buffer[2048] __attribute__((aligned(8)));
    unsigned char configs[3][128];
    const int lengths[3]={39,75,85}; /* Odd lengths are common USB layouts. */
    memset(buffer,0,sizeof(buffer)); memset(configs,0,sizeof(configs));
    UsbDeviceDescriptor *desc=(UsbDeviceDescriptor *)buffer;
    desc->bLength=18; desc->bNumConfigurations=number;
    UsbdDevice_t dev={.m_staticDeviceDescPtr=buffer,.m_staticDeviceDescEndPtr=buffer+18};
    Endpoint ep={&dev}; UsbdIoRequest_t req={&ep,0};
    for(int i=0;i<number;i++) {
        for(int j=0;j<lengths[i];j++) configs[i][j]=(unsigned char)(i*31+j);
        configs[i][0]=9; configs[i][1]=2;
        configs[i][2]=lengths[i]; configs[i][3]=0;
    }
    ready=killed=pending=0;
    usbConfig.m_maxStaticDescSize=sizeof(buffer); usbConfig.m_curDescNum=0;
    fetchConfigDescriptors(&req);
    int transfers=0;
    while(pending) {
        int config=transfers/2, expected_offset=18;
        for(int i=0;i<config;i++) expected_offset+=lengths[i];
        if(requested_destination!=buffer+expected_offset) {
            fprintf(stderr,"FAIL: configuration %d destination is offset %td, expected %d\n",
                    config,(unsigned char *)requested_destination-buffer,expected_offset);
            return 1;
        }
        assert(config<number && requested_config==config);
        assert(requested_length==(transfers%2 ? lengths[config] : 4));
        memcpy(requested_destination,configs[config],requested_length);
        pending=0; ++transfers; continuation(&req);
    }
    assert(transfers==number*2 && ready==1 && killed==0);
    int offset=18;
    for(int i=0;i<number;i++) {
        assert(!memcmp(buffer+offset,configs[i],lengths[i])); offset+=lengths[i];
    }
    assert(dev.m_staticDeviceDescEndPtr==buffer+offset);
    return 0;
}
int main(void)
{
    for(int configs=1;configs<=3;configs++) if(check_configurations(configs)) return 1;
    puts("USBD enumeration tests passed: 1, 2 and 3 configurations, odd-length descriptors.");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='jfps2-usbd-test-') as folder:
    test = Path(folder) / 'test.c'
    binary = Path(folder) / 'test'
    test.write_text(prefix + function + suffix)
    subprocess.run(['cc', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
