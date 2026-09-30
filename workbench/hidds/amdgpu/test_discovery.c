/*
 * Host-side unit test for the amdgpu discovery-table parser.
 *
 * Builds a synthetic-but-format-accurate IP discovery binary (one die,
 * a handful of IP blocks with known base addresses), runs the exact
 * parse logic from amdgpu_discovery.c against it, and asserts the
 * resolved bases. No GPU, no AROS — pure format validation of the
 * wire layout we vendored from discovery.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* HW_IDs (from soc15_hw_ip.h, mirrored in discovery_excerpt.h) */
#define GC_HWID 11
#define NBIF_HWID 108
#define MMHUB_HWID 34
#define OSSSYS_HWID 40
#define MP0_HWID 255
#define SDMA0_HWID 42
#define DMU_HWID 271

#define BINARY_SIGNATURE 0x28211407
#define TABLE_SIGNATURE  0x53445049

/* ---- byte writers ---- */
static uint8_t blob[10240];
static void w16(int off, uint16_t v){ blob[off]=v&0xff; blob[off+1]=v>>8; }
static void w32(int off, uint32_t v){ blob[off]=v&0xff; blob[off+1]=(v>>8)&0xff; blob[off+2]=(v>>16)&0xff; blob[off+3]=v>>24; }

/* ---- parser copied from amdgpu_discovery.c (le readers + walk) ---- */
static uint16_t le16(const uint8_t*b){return b[0]|(b[1]<<8);}
static uint32_t le32(const uint8_t*b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}

struct base { uint32_t base; uint8_t maj,min,rev; int valid; };
static struct base gc, nbio, mmhub, osssys, psp, sdma0, dmu;

static void record(uint16_t hw_id, uint8_t maj, uint8_t min, uint8_t rev, uint32_t b){
    struct base *s=NULL;
    switch(hw_id){
    case GC_HWID: s=&gc; break; case NBIF_HWID: s=&nbio; break;
    case MMHUB_HWID: s=&mmhub; break; case OSSSYS_HWID: s=&osssys; break;
    case MP0_HWID: s=&psp; break; case SDMA0_HWID: s=&sdma0; break;
    case DMU_HWID: s=&dmu; break; default: break; }
    if(s && !s->valid){ s->base=b; s->maj=maj; s->min=min; s->rev=rev; s->valid=1; }
}

static int parse(const uint8_t*bin, uint32_t total, int is64){
    if(le32(bin)!=BINARY_SIGNATURE) return 0;
    uint32_t tl=12+0*8;
    uint32_t tbl_off=le16(bin+tl);
    if(le32(bin+tbl_off)!=TABLE_SIGNATURE) return 0;
    uint16_t num_dies=le16(bin+tbl_off+12);
    if(num_dies>16) num_dies=16;
    for(uint16_t d=0; d<num_dies; d++){
        uint32_t di=tbl_off+12+d*4;
        uint16_t die_off=le16(bin+di+2);
        uint32_t p=tbl_off+die_off;
        uint16_t num_ips=le16(bin+p+2);
        p+=4;
        for(uint16_t i=0;i<num_ips;i++){
            uint32_t head=p;
            if(head+8>total) return 1;
            uint16_t hw_id=le16(bin+head);
            uint8_t num_base=bin[head+3];
            uint8_t maj=bin[head+4],min=bin[head+5],rev=bin[head+6];
            uint32_t base0=0;
            p=head+8;
            if(num_base){ uint32_t stride=is64?8:4; base0=le32(bin+p); p+=num_base*stride; }
            record(hw_id,maj,min,rev,base0);
        }
    }
    return nbio.valid || gc.valid;
}

/* ---- build a table: header, 1 die at 0x100, IPs each with 1 base ---- */
struct ipdef { uint16_t hw; uint8_t maj,min,rev; uint32_t base; };

int main(void){
    memset(blob,0,sizeof blob);
    int TBL=0x40;        /* ip_discovery_header offset */
    int DIE=0x100;       /* die_header offset (relative to blob start) */

    /* binary_header */
    w32(0, BINARY_SIGNATURE);
    w16(12, TBL);        /* table_list[0].offset */

    /* ip_discovery_header at TBL */
    w32(TBL+0, TABLE_SIGNATURE);
    w16(TBL+12, 1);      /* num_dies */
    /* die_info[0]: die_id, die_offset (relative to TBL) */
    w16(TBL+12+2+0, 0);          /* die_id */
    w16(TBL+12+2+2, DIE-TBL);    /* die_offset */
    /* v4 64-bit flag at TBL+12+16*4 = TBL+76: leave 0 (32-bit) */

    struct ipdef ips[] = {
        { GC_HWID,    11, 0, 3, 0x00001260 },  /* GFX10.3 */
        { NBIF_HWID,   2, 3, 0, 0x00000D20 },
        { MMHUB_HWID,  2, 0, 0, 0x00068000 },
        { OSSSYS_HWID, 5, 0, 0, 0x00001000 },
        { MP0_HWID,   11, 0, 0, 0x00016000 },
        { SDMA0_HWID,  5, 0, 0, 0x00001260 },
        { DMU_HWID,    3, 0, 0, 0x00072000 },
    };
    int n = sizeof(ips)/sizeof(ips[0]);

    /* die_header at DIE */
    w16(DIE+0, 0);       /* die_id */
    w16(DIE+2, n);       /* num_ips */
    int p = DIE+4;
    for(int i=0;i<n;i++){
        w16(p+0, ips[i].hw);
        blob[p+2]=0;             /* instance */
        blob[p+3]=1;             /* num_base_address */
        blob[p+4]=ips[i].maj;
        blob[p+5]=ips[i].min;
        blob[p+6]=ips[i].rev;
        blob[p+7]=0;
        w32(p+8, ips[i].base);   /* base_address[0] */
        p += 8 + 4;              /* head + 1 32-bit base */
    }

    int ok = parse(blob, sizeof blob, 0);
    int fail = 0;
#define CHECK(name, cond) do{ if(!(cond)){ printf("FAIL %s\n", name); fail=1;} else printf("PASS %s\n", name);}while(0)
    CHECK("parse validated", ok==1);
    CHECK("GC base",    gc.valid && gc.base==0x1260);
    CHECK("GC version", gc.maj==11 && gc.min==0 && gc.rev==3);
    CHECK("NBIO base",  nbio.valid && nbio.base==0xD20);
    CHECK("MMHUB base", mmhub.valid && mmhub.base==0x68000);
    CHECK("OSS base",   osssys.valid && osssys.base==0x1000);
    CHECK("PSP base",   psp.valid && psp.base==0x16000);
    CHECK("SDMA0 base", sdma0.valid && sdma0.base==0x1260);
    CHECK("DMU base",   dmu.valid && dmu.base==0x72000);

    /* truncation safety: a table claiming more IPs than fit must not
       read past the buffer (parser returns what it has, no crash). */
    memset(&gc,0,sizeof gc); memset(&nbio,0,sizeof nbio);
    w16(DIE+2, 9999);
    int ok2 = parse(blob, sizeof blob, 0);
    CHECK("truncation-safe", ok2==1);   /* found real IPs, stopped cleanly */

    printf(fail ? "\nFAILED\n" : "\nALL PASS\n");
    return fail;
}
