"""Exercise the real read-only pipe snapshot with synthetic OHCI state."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'third_party/ps2sdk-usbd/src/usbd_api.c').read_text()
start = source.index('int sceUsbdGetPipeInfo(')
end = source.index('\n}\n', start) + 3
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uintptr_t uiptr;
#define USB_RC_OK 0
#define USB_RC_BADOPTION 1
#define USB_RC_BADCONTEXT 2
#define USB_RC_BADPIPE 3
#define TYPE_BULK 64
#define TYPE_CONTROL 63
#define TYPE_ISOCHRON 65
typedef struct TD { u32 m_hcArea; void *m_curBufPtr; struct TD *m_next; void *m_bufferEnd; } UsbdHcTD_t;
typedef struct { union { u32 asu32; } m_hcArea; UsbdHcTD_t *m_tdTail, *m_tdHead; } UsbdHcED_t;
typedef struct EP UsbdEndpoint_t;
typedef struct { UsbdEndpoint_t *m_correspEndpoint; unsigned int m_length; void *m_destPtr; } UsbdIoRequest_t;
struct EP { unsigned int m_id,m_endpointType,m_inTdQueue; UsbdHcED_t *m_hcEd; UsbdIoRequest_t *m_ioReqListStart; };
static struct { u32 HcControl; } controller;
static UsbdHcTD_t tds[2] __attribute__((aligned(16)));
static UsbdIoRequest_t requests[1];
static UsbdIoRequest_t *lookup[2];
static struct Pool {
    UsbdHcTD_t *m_hcTdBuf,*m_hcTdBufEnd;
    UsbdIoRequest_t **m_hcTdToIoReqLUT;
    UsbdIoRequest_t *m_ioReqBufPtr;
    __typeof__(controller) *m_ohciRegs;
} pool, *memPool=&pool;
static struct { int m_maxIoReqs; } usbConfig;
static UsbdHcED_t ed;
static UsbdEndpoint_t ep;
static unsigned char buffer[2048];
static int lock_failure, locked, bad_pipe;
static int usbdLock(void) { if(lock_failure) return -1; locked=1; return 0; }
static void usbdUnlock(void) { assert(locked); locked=0; }
static UsbdEndpoint_t *fetchEndpointById(int id)
{ return id==7 && !bad_pipe ? &ep : NULL; }
static void reset(void)
{
    memset(tds,0,sizeof(tds)); memset(requests,0,sizeof(requests)); memset(&ed,0,sizeof(ed));
    pool.m_hcTdBuf=tds; pool.m_hcTdBufEnd=tds+2;
    pool.m_hcTdToIoReqLUT=lookup; pool.m_ioReqBufPtr=requests; pool.m_ohciRegs=&controller;
    usbConfig.m_maxIoReqs=1; controller.HcControl=0xbf;
    ed.m_hcArea.asu32=0x00401102; ed.m_tdHead=tds; ed.m_tdTail=tds+1;
    ep=(UsbdEndpoint_t){.m_id=7,.m_endpointType=TYPE_BULK,.m_hcEd=&ed};
    requests[0].m_correspEndpoint=&ep; requests[0].m_length=2048; requests[0].m_destPtr=buffer;
    lookup[0]=requests; lookup[1]=NULL;
    tds[0].m_hcArea=0xf01c0000; tds[0].m_curBufPtr=buffer;
    lock_failure=locked=bad_pipe=0;
}
'''
suffix = r'''
int main(void)
{
    u32 output[8]; reset();
    assert(sceUsbdGetPipeInfo(7,output)==0 && !locked);
    assert(output[0]==7 && output[1]==0x00401102 && output[2]==0x100);
    assert(output[3]==0xf01c0000 && output[4]==2048 && output[5]==0 && output[6]==0xbf);
    tds[0].m_curBufPtr=buffer+128;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[5]==128);
    tds[0].m_curBufPtr=NULL;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[5]==2048);
    ed.m_tdHead=(UsbdHcTD_t *)((uiptr)tds|1);
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[2]==0x101);
    ed.m_tdHead=ed.m_tdTail;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[2]==0 && !output[3] && !output[4]);
    ed.m_tdHead=(UsbdHcTD_t *)16;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[2]==0x80000000u);
    reset(); lookup[0]=(UsbdIoRequest_t *)16;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[4]==0); /* bounds-checked LUT */
    reset(); ep.m_ioReqListStart=requests; ep.m_inTdQueue=1;
    assert(sceUsbdGetPipeInfo(7,output)==0 && output[7]==0x101);
    ep.m_endpointType=TYPE_ISOCHRON;
    assert(sceUsbdGetPipeInfo(7,output)==USB_RC_BADPIPE && !locked);
    reset(); bad_pipe=1;
    assert(sceUsbdGetPipeInfo(7,output)==USB_RC_BADPIPE && !locked);
    reset(); lock_failure=1;
    assert(sceUsbdGetPipeInfo(7,output)==USB_RC_BADCONTEXT);
    assert(sceUsbdGetPipeInfo(7,NULL)==USB_RC_BADOPTION);
    puts("USBD pipe tests passed: idle, queued, active, partial, complete, halted, invalid pointers and errors.");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='usbd-pipe-') as folder:
    test = Path(folder) / 'pipe.c'
    binary = Path(folder) / 'pipe'
    test.write_text(prefix + source[start:end] + suffix)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
