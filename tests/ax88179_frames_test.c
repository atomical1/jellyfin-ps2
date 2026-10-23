/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../iop/ax88179/frames.h"

static unsigned int seen, lengths[8];
static unsigned char first[8];
static void sink(const unsigned char *frame, unsigned int length, void *arg)
{
    (void)arg;
    assert(seen < 8);
    lengths[seen] = length;
    first[seen++] = frame[0];
}
static void put32(unsigned char *p, unsigned int n)
{
    for (unsigned int i=0; i<4; ++i) p[i] = n >> (i*8);
}
static unsigned int random_state = 0x88179;
static unsigned int next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
int main(void)
{
    unsigned char input[AX_RX_BUFFER_SIZE], output[AX_FRAME_MAX + 16];
    memset(input, 0, sizeof(input));
    input[0]=0x12; input[64]=0x34;
    put32(input+128, 60u << 16);
    put32(input+132, 0x80000000); /* firmware dummy */
    put32(input+136, 64u << 16);
    put32(input+140, (128u << 16) | 3);
    assert(ax_parse_rx(input,144,sink,NULL)==2);
    assert(seen==2 && lengths[0]==60 && lengths[1]==64);
    assert(first[0]==0x12 && first[1]==0x34);
    /* CRC error is dropped, remaining frame still delivered. */
    seen=0; put32(input+128,(60u << 16)|0x20000000u);
    assert(ax_parse_rx(input,144,sink,NULL)==1 && seen==1);
    /* Validate all metadata before delivering even an earlier valid frame. */
    seen=0; put32(input+128,60u << 16); put32(input+136,1514u << 16);
    assert(ax_parse_rx(input,144,sink,NULL)==-1 && seen==0);
    put32(input+140,(129u << 16)|3);
    assert(ax_parse_rx(input,144,sink,NULL)==-1);
    assert(ax_parse_rx(input,3,sink,NULL)==-1);
    assert(ax_parse_rx(NULL,20,sink,NULL)==-1);
    assert(ax_parse_rx(input,AX_RX_BUFFER_SIZE+1,sink,NULL)==-1);

    memset(input,0,sizeof(input));
    input[0]=0x56;
    put32(input+1520,1514u << 16);
    put32(input+1524,(1520u << 16)|1);
    seen=0;
    assert(ax_parse_rx(input,1528,sink,NULL)==1);
    assert(seen==1 && lengths[0]==1514 && first[0]==0x56);

    /* Every supported frame length, including every 64-byte USB boundary. */
    for(unsigned int length=14;length<=AX_FRAME_MAX;++length) {
        for(unsigned int i=0;i<length;++i) input[i]=(unsigned char)i;
        int n=ax_build_tx(output,sizeof(output),input,length,64);
        unsigned int pad=(length+8)%64==0;
        assert(n==(int)(length+8+pad));
        assert(output[0]==(unsigned char)length && output[1]==(length>>8));
        assert(output[3]==0x80);
        assert(!memcmp(output+8,input,length));
        assert(output[4]==0 && output[5]==(pad?0x80:0));
        assert(output[7]==(pad?0x80:0));
        if(pad) assert(output[n-1]==0);
    }
    assert(ax_build_tx(output,10,input,60,64)==-1);
    assert(ax_build_tx(output,sizeof(output),input,13,64)==-1);
    assert(ax_build_tx(output,sizeof(output),input,1515,64)==-1);
    assert(ax_build_tx(output,sizeof(output),input,60,0)==-1);

    /* Fuzz arbitrary USB batches under ASan/UBSan: no unchecked reads. */
    for(unsigned int trial=0;trial<100000;++trial) {
        unsigned int size=next_random()%(sizeof(input)+1);
        for(unsigned int i=0;i<size;++i) input[i]=next_random();
        (void)ax_parse_rx(input,size,NULL,NULL);
    }
    puts("AX88179 packet tests passed (100000 fuzz batches, all TX lengths).");
    return 0;
}
