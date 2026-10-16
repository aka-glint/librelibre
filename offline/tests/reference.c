/* Independent C test oracle for pinned DiaBLE Libre2.swift:64-171.
 * MIT; Copyright (c) 2026 Guido Soranzio. See ../reference/LICENSE.md.
 * stdin: 8 wire UID bytes, 6 patch bytes, 344 encrypted bytes. stdout: 344 bytes.
 * Does not parse JSON, estimate glucose, or use production Python code.
 */
#include <stdint.h>
#include <stdio.h>
static uint16_t le(const unsigned char *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint16_t op(uint16_t w) {
    return (w >> 2) ^ ((w & 1) ? 0x6860 : 0) ^ ((w & 2) ? 0xa0c5 : 0);
}
int main(void) {
    unsigned char uid[8], patch[6], data[344];
    if(fread(uid,1,8,stdin)!=8 || fread(patch,1,6,stdin)!=6 || fread(data,1,344,stdin)!=344 || getchar()!=EOF) return 2;
    for(unsigned b=0;b<43;b++) {
        uint16_t s[4] = {(uint16_t)(le(uid+4)+b+(le(patch+4)^0x44)),le(uid+2),(uint16_t)(le(uid)+2*b),0x241a^0x14c6};
        uint16_t r[8];
        r[0]=op(s[0])^s[3];
        for(unsigned i=1;i<4;i++) r[i]=op(r[i-1])^s[3-i];
        r[4]=op(r[3]);
        for(unsigned i=5;i<8;i++) r[i]=op(r[i-1]^r[i-5]);
        for(unsigned w=0;w<4;w++) {
            uint16_t k=r[3-w]^r[7-w];
            data[8*b+2*w]^=k&255; data[8*b+2*w+1]^=k>>8;
        }
    }
    return fwrite(data,1,344,stdout)==344 ? 0 : 3;
}
