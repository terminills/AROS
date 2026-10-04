/*
 * amdgpu.hidd — DDC/EDID display-mode probe.
 *
 * DRAFT — ported from the radeonsi monolith for review before it goes on
 * hardware. The register offsets, DDC transaction sequence and EDID parser
 * are lifted faithfully from:
 *
 *   workbench/hidds/radeonsi/radeonsi_display.c     (DCN DC_I2C DDC block,
 *                                                    connector naming, EDID read)
 *   workbench/hidds/radeonsi/radeonsi_hiddclass.c   (EDID mode harvest + sync
 *                                                    tag builder)
 *   workbench/hidds/radeonsi/radeonsi_accel.c       (CVT timing calculator)
 *
 * What was ADAPTED for amdgpu:
 *   - radeonsi's RREG32/WREG32 take BYTE addresses and pre-multiply the DCN
 *     BASE_IDX-2 segment: RADEONSI_DCN42_REG(reg) = ((0x34C0 + reg) * 4).
 *     amdgpu's amdgpu_rreg32(ad, off)/amdgpu_wreg32(ad, off, v) take a DWORD
 *     register offset, and the DMU BASE_IDX-2 segment is already resolved in
 *     ad->dmu_seg[2] (== 0x34C0 on Navi 44, per amdgpu_dcn.c). So here every
 *     access is amdgpu_rreg32(ad, dcn + REG) with REG kept as the RAW dword
 *     register index from the radeonsi comments (e.g. regDC_I2C_CONTROL 0x1e98).
 *     This is the identical hardware register as radeonsi — cross-check: this
 *     file's AMDGPU_DDC_HPD0_INT_STATUS (0x175a) equals radeonsi's
 *     RADEONSI_HPD_INT_STATUS_BASE reg AND amdgpu_dcn.c's DCN_HPD0_INT_STATUS.
 *
 * >>> REGISTER-BASE / OFFSET UNCERTAINTIES FOR THE PARENT TO VERIFY <<<
 *   (1) dmu_seg[2] base. amdgpu_dcn.c already reads HUBP/OTG/HPD through
 *       dcn = ad->dmu_seg[2]; the HPD read here reuses that exact path, so HPD
 *       is on solid ground. The DC_I2C_* block is assumed to share BASE_IDX 2
 *       (radeonsi_display.c:18 says "DC_I2C_* registers have BASE_IDX 2"), but
 *       radeonsi ALSO warns the raw reg*4 path "lands in an all-zero hole" on
 *       RDNA4 — confirm DC_I2C_CONTROL (dcn+0x1e98) is not all-ones/all-zero
 *       before trusting a transaction (guard included below).
 *   (2) Per-channel DDC strides. radeonsi strides SETUP/SPEED by 0x08 BYTES
 *       (= 2 dwords) and HW_STATUS by 0x04 BYTES (= 1 dword) per channel
 *       (radeonsi_display.c:85-87). Reproduced here in dword units
 *       (AMDGPU_DDC_*_CH_STRIDE). Verify against Linux dcn_4_2_0_offset.h that
 *       regDC_I2C_DDC2_SETUP - regDC_I2C_DDC1_SETUP == 2 dwords, DDC2_HW_STATUS
 *       - DDC1_HW_STATUS == 1 dword.
 *   (3) radeonsi keeps the DCN I2C WRITE path DISABLED by default
 *       (RADEONSI_ENABLE_DCN_I2C_WRITES == 0) because ProcessI2cChannel
 *       Transaction was found MISSING in the RDNA4 VBIOS and the block read
 *       all-ones. The full transaction IS ported here (it is what the task
 *       asked for) but gated behind AMDGPU_ENABLE_DCN_I2C_WRITES, defaulting
 *       OFF, so a boot does not poke the block until you flip it on hardware.
 *   (4) Connector TYPE is a STUB. Real type + DDC-line routing need the ATOM
 *       displayobjectinfo table (radeonsi_display.c:1461 RadeonsiParseAtom
 *       DisplayInfo). amdgpu has no ATOM parse yet, so connector labels are a
 *       best-effort by index and the DDC channel is assumed == connector index.
 *       See amdgpu_display_connector_name() — flagged there too.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <oop/oop.h>
#include <hidd/gfx.h>
#include <hidd/pci.h>

#include "amdgpu_intern.h"

/* ------------------------------------------------------------------------ *
 * DCN DC_I2C DDC + HPD register map — RAW dword indices (BASE_IDX 2).
 * Accessed as amdgpu_rreg32(ad, dcn + REG) where dcn = ad->dmu_seg[2].
 * Values + comments transcribed from radeonsi_display.c:23-38.
 * ------------------------------------------------------------------------ */
#define AMDGPU_DDC_CONTROL            0x1e98   /* regDC_I2C_CONTROL          (rsi:23) */
#define AMDGPU_DDC_ARBITRATION       0x1e99   /* regDC_I2C_ARBITRATION      (rsi:24) */
#define AMDGPU_DDC_SW_STATUS         0x1e9b   /* regDC_I2C_SW_STATUS        (rsi:25) */
#define AMDGPU_DDC_HW_STATUS_BASE    0x1e9c   /* regDC_I2C_DDC1_HW_STATUS   (rsi:26) */
#define AMDGPU_DDC_SPEED_BASE        0x1ea2   /* regDC_I2C_DDC1_SPEED       (rsi:27) */
#define AMDGPU_DDC_SETUP_BASE        0x1ea3   /* regDC_I2C_DDC1_SETUP       (rsi:28) */
#define AMDGPU_DDC_TRANSACTION_0     0x1eae   /* regDC_I2C_TRANSACTION0     (rsi:29) */
#define AMDGPU_DDC_TRANSACTION_1     0x1eaf   /* regDC_I2C_TRANSACTION1     (rsi:30) */
#define AMDGPU_DDC_TRANSACTION_2     0x1eb0   /* regDC_I2C_TRANSACTION2     (rsi:31) */
#define AMDGPU_DDC_TRANSACTION_3     0x1eb1   /* regDC_I2C_TRANSACTION3     (rsi:32) */
#define AMDGPU_DDC_DATA              0x1eb2   /* regDC_I2C_DATA             (rsi:33) */

/* HPD per connector — regHPD<n>_DC_HPD_INT_STATUS, dword stride 8
 * (radeonsi_display.c:35,38 use 0x08*4 BYTES = 8 dwords; amdgpu_dcn.c:32-33
 * already uses 0x175a + n*0x0008 in dwords — matches). */
#define AMDGPU_DDC_HPD0_INT_STATUS   0x175a
#define AMDGPU_HPD_DW_STRIDE         0x0008

/* Per-DDC-channel strides in DWORDS (radeonsi byte strides / 4;
 * radeonsi_display.c:41,85-87). SEE UNCERTAINTY (2). */
#define AMDGPU_DDC_HWSTAT_CH_STRIDE  0x01     /* radeonsi 0x04 bytes */
#define AMDGPU_DDC_SPEED_CH_STRIDE   0x02     /* radeonsi 0x08 bytes */
#define AMDGPU_DDC_SETUP_CH_STRIDE   0x02     /* radeonsi 0x08 bytes */

#define AMDGPU_DDC_HW_STATUS(ch)  (AMDGPU_DDC_HW_STATUS_BASE + ((ch) * AMDGPU_DDC_HWSTAT_CH_STRIDE))
#define AMDGPU_DDC_SPEED(ch)      (AMDGPU_DDC_SPEED_BASE     + ((ch) * AMDGPU_DDC_SPEED_CH_STRIDE))
#define AMDGPU_DDC_SETUP(ch)      (AMDGPU_DDC_SETUP_BASE     + ((ch) * AMDGPU_DDC_SETUP_CH_STRIDE))
#define AMDGPU_HPD_INT_STATUS(i)  (AMDGPU_DDC_HPD0_INT_STATUS + ((i) * AMDGPU_HPD_DW_STRIDE))

/* DC_I2C_CONTROL / SW_STATUS / TRANSACTION / DATA bit fields
 * (radeonsi_display.c:89-126) */
#define DC_I2C_GO                       (1UL << 0)
#define DC_I2C_SOFT_RESET               (1UL << 1)
#define DC_I2C_SW_STATUS_RESET          (1UL << 3)
#define DC_I2C_DDC_SELECT(ch)           (((ch) & 0x7UL) << 8)
#define DC_I2C_TRANSACTION_COUNT(n)     (((n) & 0x3UL) << 20)

#define DC_I2C_SW_STATUS_MASK           0x00000003UL
#define DC_I2C_SW_DONE                  0x00000004UL
#define DC_I2C_SW_ABORTED               0x00000010UL
#define DC_I2C_SW_TIMEOUT               0x00000020UL
#define DC_I2C_SW_STOPPED_ON_NACK       0x00000100UL

#define DC_I2C_ARB_STATUS_MASK          0x0000000CUL
#define DC_I2C_ARB_STATUS_SW            0x00000004UL
#define DC_I2C_SW_USE_I2C_REG_REQ       0x00100000UL
#define DC_I2C_SW_DONE_USING_I2C_REG    0x00200000UL

#define DC_I2C_TX_RW                    (1UL << 0)
#define DC_I2C_TX_STOP_ON_NACK          (1UL << 8)
#define DC_I2C_TX_START                 (1UL << 12)
#define DC_I2C_TX_STOP                  (1UL << 13)
#define DC_I2C_TX_COUNT(v)              (((v) & 0x3FFUL) << 16)

#define DC_I2C_DATA_RW                  (1UL << 0)
#define DC_I2C_DATA_BYTE(v)             (((v) & 0xFFUL) << 8)
#define DC_I2C_DATA_INDEX(v)            (((v) & 0x3FFUL) << 16)
#define DC_I2C_DATA_INDEX_WRITE         0x80000000UL

/* HPD sense (radeonsi_display.c:124-125, amdgpu_dcn.c:35-36) */
#define DC_HPD_SENSE                    0x00000002UL   /* immediate sense       */
#define DC_HPD_SENSE_DELAYED            0x00000010UL   /* debounced = attached  */

#define AMDGPU_DDC_MAX_CHANNELS         5   /* DCN 4.x advertises 5 DDC (rsi:40) */
#define AMDGPU_EDID_ADDR7               0x50 /* 7-bit EDID/DDC I2C slave addr    */

/* Master switch for the DCN I2C WRITE path. OFF by default — see UNCERTAINTY (3).
 * When 0, amdgpu_display_read_edid() only does the read-only all-ones guard and
 * returns FALSE (no register writes), which is safe to run on any boot. */
#ifndef AMDGPU_ENABLE_DCN_I2C_WRITES
#define AMDGPU_ENABLE_DCN_I2C_WRITES    0
#endif

/* ------------------------------------------------------------------------ *
 * stdc-free helpers. amdgpu.hidd can cold-start at residentpri 49 BEFORE
 * stdc.library exists (see amdgpu_firmware.c:31-36), so no memset/snprintf.
 * ------------------------------------------------------------------------ */
static void dsp_memzero(void *p, ULONG n)
{
    UBYTE *b = (UBYTE *)p;
    while (n--) *b++ = 0;
}

/* Append NUL-terminated src to dst[pos], never exceeding cap-1; updates pos. */
static void dsp_str_append(char *dst, ULONG *pos, ULONG cap, const char *src)
{
    while (*src && *pos + 1 < cap)
        dst[(*pos)++] = *src++;
    dst[*pos] = '\0';
}

/* Append unsigned decimal value to dst[pos]. */
static void dsp_uint_append(char *dst, ULONG *pos, ULONG cap, ULONG val)
{
    char tmp[12];
    LONG t = 0;

    if (val == 0)
        tmp[t++] = '0';
    while (val && t < (LONG)sizeof(tmp))
    {
        tmp[t++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (t > 0 && *pos + 1 < cap)
        dst[(*pos)++] = tmp[--t];
    dst[*pos] = '\0';
}

/* ------------------------------------------------------------------------ *
 * 1. Connector enumeration.
 *    HPD sense is solid (same path as amdgpu_dcn.c). TYPE + real names need
 *    ATOM (UNCERTAINTY 4) — labels here are a best-effort by index.
 * ------------------------------------------------------------------------ */

/* ATOM object ID packs OBJECT_TYPE:4 | ENUM_ID:4 | OBJECT_ID:8, so the LOW BYTE
 * is the connector type (ObjectID.h CONNECTOR_OBJECT_ID_*). */
#define ATOM_OBJID_TYPE(o)   (((o) >> 12) & 0xF)
#define ATOM_OBJID_ENUM(o)   (((o) >>  8) & 0xF)
#define ATOM_OBJID_ID(o)     ((o) & 0xFF)

#define CONN_OBJ_SINGLE_LINK_DVI_D  0x03
#define CONN_OBJ_DUAL_LINK_DVI_D    0x04
#define CONN_OBJ_VGA                0x05
#define CONN_OBJ_HDMI_TYPE_A        0x0C
#define CONN_OBJ_HDMI_TYPE_B        0x0D
#define CONN_OBJ_LVDS               0x0E
#define CONN_OBJ_DISPLAYPORT        0x13
#define CONN_OBJ_EDP                0x14
#define CONN_OBJ_USBC               0x17

static const char *amdgpu_conn_type_name(UWORD objid)
{
    switch (ATOM_OBJID_ID(objid)) {
    case CONN_OBJ_HDMI_TYPE_A:       return "HDMI-A";
    case CONN_OBJ_HDMI_TYPE_B:       return "HDMI-B";
    case CONN_OBJ_DISPLAYPORT:       return "DP";
    case CONN_OBJ_EDP:               return "eDP";
    case CONN_OBJ_USBC:              return "USB-C";
    case CONN_OBJ_SINGLE_LINK_DVI_D: return "DVI-D";
    case CONN_OBJ_DUAL_LINK_DVI_D:   return "DVI-D";
    case CONN_OBJ_VGA:               return "VGA";
    case CONN_OBJ_LVDS:              return "LVDS";
    default:                         return NULL;
    }
}

/* Connector label from the ATOM display-object ID.
 *
 * This USED to hardcode "DP-<idx+1>" with the comment "without ATOM we cannot
 * know the real type". That comment went stale once the displayobjectinfo parse
 * landed — display_objid has been available in ad->atom_connectors[] since then,
 * and the type was sitting in its low byte the whole time. The placeholder was
 * actively misleading: on the DUT the RX 9060 XT reports
 *     connector 0 obj=0x3113  -> id 0x13 DISPLAYPORT, enum 1
 *     connector 1 obj=0x3213  -> id 0x13 DISPLAYPORT, enum 2
 *     connector 2 obj=0x330c  -> id 0x0C HDMI_TYPE_A, enum 3
 * so the ACTIVE display — the one carrying the EDID and the live scanout — is
 * HDMI, and we were labelling it "DP-3". That label was then reported upward and
 * used to reason about which physical output the card drives, incorrectly.
 *
 * Instance numbering is per TYPE (the first HDMI is HDMI-A-1, not HDMI-A-3),
 * which matches the semantics of the new gfx.hidd ConnectorID attribute
 * (upstream 344966d4f7: "1-based instance per type"). Falls back to the old
 * neutral form only when ATOM gave us nothing usable. */
void amdgpu_display_connector_name(struct AmdgpuDevice *ad, ULONG idx,
                                   char *label, ULONG label_size)
{
    ULONG pos = 0;
    const char *type = NULL;
    ULONG instance = idx + 1;

    if (!label || label_size == 0)
        return;

    if (ad && ad->atom_display_valid && idx < ad->atom_connector_count &&
        ad->atom_connectors[idx].valid)
    {
        UWORD objid = ad->atom_connectors[idx].display_objid;

        type = amdgpu_conn_type_name(objid);
        if (type) {
            ULONG i, n = 0;
            /* 1-based instance among connectors of the SAME type */
            for (i = 0; i <= idx && i < ad->atom_connector_count; i++) {
                if (ad->atom_connectors[i].valid &&
                    ATOM_OBJID_ID(ad->atom_connectors[i].display_objid) ==
                    ATOM_OBJID_ID(objid))
                    n++;
            }
            instance = n;
        }
    }

    dsp_str_append(label, &pos, label_size, type ? type : "DP");
    dsp_str_append(label, &pos, label_size, "-");
    dsp_uint_append(label, &pos, label_size, instance);
}

/* Read HPD sense for one connector. Mirrors amdgpu_dcn.c:90-98. */
BOOL amdgpu_display_connector_connected(struct AmdgpuDevice *ad, ULONG idx)
{
    ULONG dcn, hpd;

    if (!ad->rmmio || !ad->ip.dmu.valid)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn || idx >= AMDGPU_DCN_MAX_CONNECTORS)
        return FALSE;

    hpd = amdgpu_rreg32(ad, dcn + AMDGPU_HPD_INT_STATUS(idx));
    if (hpd == 0xFFFFFFFFUL)
        return FALSE;   /* unmapped / all-ones = ignore */

    return (hpd & (DC_HPD_SENSE_DELAYED | DC_HPD_SENSE)) != 0;
}

/* Fill label + connected state for connector idx. Returns FALSE if idx is out
 * of range. Port of RadeonsiGetConnectorInfo() (radeonsi_display.c:1726). */
BOOL amdgpu_display_get_connector_info(struct AmdgpuDevice *ad, ULONG idx,
                                       char *label, ULONG label_size,
                                       BOOL *connected)
{
    if (idx >= AMDGPU_DCN_MAX_CONNECTORS)
        return FALSE;

    amdgpu_display_connector_name(ad, idx, label, label_size);
    if (connected)
        *connected = amdgpu_display_connector_connected(ad, idx);

    D(bug("[amdgpu-display] connector %lu: %s -> %s\n",
          (unsigned long)idx, label,
          (connected && *connected) ? "CONNECTED" : "empty"));
    return TRUE;
}

/* ------------------------------------------------------------------------ *
 * 2. DDC/I2C EDID read over dmu_seg[2].
 *    Faithful port of the RADEONSI_ENABLE_DCN_I2C_WRITES path in
 *    RadeonsiReadEDID() (radeonsi_display.c:1996-2099) plus the SW arbitration
 *    acquire/release (radeonsi_display.c:348-378).
 *
 *    DDC TRANSACTION SEQUENCE (as ported):
 *      a. Guard: read CONTROL/ARB/SW_STATUS/DATA; if all-ones, abort (block
 *         not mapped — UNCERTAINTY 1).
 *      b. Acquire SW arbitration: set DC_I2C_SW_USE_I2C_REG_REQ in ARBITRATION,
 *         poll until (ARB & ARB_STATUS_MASK) == ARB_STATUS_SW.
 *      c. CONTROL = SW_STATUS_RESET | DDC_SELECT(ch).
 *      d. Program 3 DATA writes = the I2C address phase:
 *           DATA = INDEX_WRITE | INDEX(0) | BYTE(0x50)   (write addr, index 0)
 *           DATA = BYTE(0)                               (EDID sub-offset 0x00)
 *           DATA = BYTE(0x50) | DATA_RW                  (read addr)
 *      e. TRANSACTION0 = STOP_ON_NACK | START | COUNT(1)         (write phase)
 *         TRANSACTION1 = STOP_ON_NACK | START | RW | STOP | COUNT(read_len)
 *      f. CONTROL = DDC_SELECT(ch) | TRANSACTION_COUNT(1); then |= DC_I2C_GO.
 *      g. Poll SW_STATUS until DONE|NACK|TIMEOUT|ABORTED (or timeout counter).
 *      h. On DONE: set DATA = INDEX_WRITE | DATA_RW | INDEX(3) (read pointer),
 *         then read DATA read_len times; each byte is (DATA >> 8) & 0xFF.
 *      i. Release SW arbitration: set DC_I2C_SW_DONE_USING_I2C_REG.
 *
 *    NOTE: radeonsi ships this path DISABLED (UNCERTAINTY 3). Gated by
 *    AMDGPU_ENABLE_DCN_I2C_WRITES, default 0.
 * ------------------------------------------------------------------------ */

static BOOL amdgpu_ddc_block_all_ones(struct AmdgpuDevice *ad, ULONG dcn)
{
    ULONG ctrl = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_CONTROL);
    ULONG arb  = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_ARBITRATION);
    ULONG sw   = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_SW_STATUS);
    ULONG data = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_DATA);

    if (ctrl == 0xFFFFFFFFUL && arb == 0xFFFFFFFFUL &&
        sw == 0xFFFFFFFFUL && data == 0xFFFFFFFFUL)
    {
        D(bug("[amdgpu-display] DDC guard: DC_I2C block reads all-ones "
              "(CTRL/ARB/SW/DATA) — aborting (UNCERTAINTY 1)\n"));
        return TRUE;
    }
    D(bug("[amdgpu-display] DDC block: CTRL=0x%08lx ARB=0x%08lx SW=0x%08lx DATA=0x%08lx\n",
          (unsigned long)ctrl, (unsigned long)arb,
          (unsigned long)sw, (unsigned long)data));
    return FALSE;
}

#if AMDGPU_ENABLE_DCN_I2C_WRITES
/* Port of RadeonsiAcquireI2C() (radeonsi_display.c:348-369). */
static BOOL amdgpu_ddc_acquire(struct AmdgpuDevice *ad, ULONG dcn)
{
    ULONG arb = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_ARBITRATION);
    ULONG wait;

    if ((arb & DC_I2C_ARB_STATUS_MASK) == DC_I2C_ARB_STATUS_SW)
        return TRUE;

    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_ARBITRATION, arb | DC_I2C_SW_USE_I2C_REG_REQ);
    for (wait = 0; wait < 1000; wait++)
    {
        arb = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_ARBITRATION);
        if ((arb & DC_I2C_ARB_STATUS_MASK) == DC_I2C_ARB_STATUS_SW)
            break;
    }
    return (arb & DC_I2C_ARB_STATUS_MASK) == DC_I2C_ARB_STATUS_SW;
}

/* Port of RadeonsiReleaseI2C() (radeonsi_display.c:371-378). */
static void amdgpu_ddc_release(struct AmdgpuDevice *ad, ULONG dcn)
{
    ULONG arb = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_ARBITRATION);
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_ARBITRATION, arb | DC_I2C_SW_DONE_USING_I2C_REG);
}
#endif /* AMDGPU_ENABLE_DCN_I2C_WRITES */

/* ======================================================================== *
 * GPIO BIT-BANGED DDC EDID READ (PREFERRED PATH)
 *
 * The DC_I2C hardware engine reads all-zeros (dead) on RDNA4 — confirmed on
 * hardware — so EDID cannot come through it. radeonsi's working fallback is to
 * bit-bang I2C directly over the connector's DDC SCL/SDA GPIO lines, with the
 * GPIO register offsets + per-connector routing parsed from the VBIOS ATOM
 * GPIO_Pin_LUT / displayobjectinfo I2C records.
 *
 * FAITHFUL PORT of radeonsi_display.c. Source-line map:
 *   - VBIOS fetch:          radeonsi_minimal_stubs.c:333-402 (radeonsi_load_vbios_copy)
 *   - atom_get_u16/u32:     radeonsi_display.c:968-979
 *   - atom_get_rom_size:    radeonsi_display.c:981-988
 *   - atom_offset_valid:    radeonsi_display.c:990-993
 *   - ATOM connector parse: radeonsi_display.c:1414-1612 (records + displayobjectinfo)
 *   - GPIO_Pin_LUT walk:    radeonsi_display.c:394-462  (RadeonsiFindGpioDDCPin)
 *   - SCL/SDA masks:        radeonsi_display.c:81-83
 *   - bit-bang primitives:  radeonsi_display.c:386-654  (delay/put/get/open/close/
 *                           writebit/readbit/start/stop/writebyte/readbyte)
 *   - EDID read loop:       radeonsi_display.c:656-776  (RadeonsiGpioDDCReadEDID)
 *
 * >>> CRITICAL ADAPTATION — GPIO REGISTER OFFSET UNITS (verify on hardware) <<<
 *   radeonsi's RREG32(off) takes a BYTE address and reads mmio_base[off/4], so it
 *   multiplies the ATOM GPIO data_a register INDEX by 4:
 *       data_a_offset = data_a_index * 4;  mask = (index-1)*4; en = (index+1)*4;
 *       y = (index+2)*4   (radeonsi_display.c:454-457)
 *   amdgpu_rreg32(ad, off)/amdgpu_wreg32 take a DWORD offset into rmmio directly
 *   (amdgpu_mmio.c:48-60), so here we DO NOT multiply by 4 — we keep the raw ATOM
 *   dword register index:
 *       data_a_offset = data_a_index;      mask = index-1; en = index+1;
 *       y = index+2   (all DWORD offsets)
 *   These GPIO A/EN/Y/MASK registers are read ABSOLUTELY off rmmio (NOT added to
 *   dmu_seg[2]) — radeonsi reads them straight off mmio_base with no DCN segment,
 *   because the ATOM GPIO_Pin_LUT stores absolute register indices. UNCERTAINTY:
 *   confirm on the 9060 XT that these indices land on the live DC_GPIO_DDCx_A/EN/Y
 *   registers (non-zero, toggling) and not in an unmapped hole.
 * ======================================================================== */

/* ATOM BIOS table constants (radeonsi_display.c:132-170). */
#define AMDGPU_ATOM_ROM_HEADER_PTR             0x48
#define AMDGPU_ATOM_ROM_IMAGE_SIZE             0x02
#define AMDGPU_ATOM_ROM_MASTER_COMMAND_OFFSET  0x1e
#define AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET     0x20
#define AMDGPU_ATOM_MASTER_DISPLAY_OBJECT_INFO 0x30
#define AMDGPU_ATOM_COMMON_HEADER_SIZE         4

/* ATOM command-table indices (radeonsi_display.c:139-154) — the modeset commands
 * whose presence decides ATOM-assisted modeset vs the SMU/DCCG-direct route on
 * RDNA4 (same viability question DC_I2C answered: ProcessI2c was stripped here). */
#define AMDGPU_ATOM_CMD_SET_PIXEL_CLOCK        12
#define AMDGPU_ATOM_CMD_ENABLE_DISP_POWER_GATE 13
#define AMDGPU_ATOM_CMD_BLANK_CRTC             34
#define AMDGPU_ATOM_CMD_ENABLE_CRTC            35
#define AMDGPU_ATOM_CMD_SET_CRTC_TIMING        39
#define AMDGPU_ATOM_CMD_SELECT_CRTC_SOURCE     42
#define AMDGPU_ATOM_CMD_ENABLE_GRAPH_SURFACES  43
#define AMDGPU_ATOM_CMD_SET_DCE_CLOCK          46
#define AMDGPU_ATOM_CMD_DIG1_ENCODER_CONTROL   74
#define AMDGPU_ATOM_CMD_DIG1_TRANSMITTER_CTRL  76
#define AMDGPU_ATOM_DATA_GPIO_PIN_LUT          12   /* master-data-table index    */
#define AMDGPU_ATOM_GPIO_PIN_ASSIGNMENT_SIZE   8    /* bytes per GPIO_Pin entry    */
#define AMDGPU_ATOM_DATA_POWERPLAYINFO         15   /* master-data-table index
                                                     * (atom_master_list_of_data_
                                                     * tables_v2_1.powerplayinfo;
                                                     * displayobjectinfo=22 -> its
                                                     * byte offset 4+22*2=0x30 in
                                                     * our code confirms this map) */

#define AMDGPU_ATOM_OBJECT_RECORD_I2C          1
#define AMDGPU_ATOM_OBJECT_RECORD_HPD          2
#define AMDGPU_ATOM_OBJECT_RECORD_END          0xff
#define AMDGPU_ATOM_DISPLAY_PATH_V1_4_SIZE     20
#define AMDGPU_ATOM_DISPLAY_PATH_V1_5_SIZE     16
#define AMDGPU_ATOM_MAX_CONNECTORS             8
#define AMDGPU_ATOM_OBJECT_TYPE_MASK           0x7000
#define AMDGPU_ATOM_OBJECT_TYPE_CONNECTOR      0x3000
#define AMDGPU_ATOM_I2C_HW_LANE_MUX            0x0f
#define AMDGPU_ATOM_I2C_HW_ENGINE_ID_MASK      0x70

/* DDC SCL/SDA bit masks inside the GPIO A/EN/Y registers
 * (radeonsi_display.c:81-83). */
#define AMDGPU_GPIO_DDC_SCL_MASK   0x00000001UL
#define AMDGPU_GPIO_DDC_SDA_MASK   0x00000100UL
#define AMDGPU_GPIO_DDC_LINE_MASK  (AMDGPU_GPIO_DDC_SCL_MASK | AMDGPU_GPIO_DDC_SDA_MASK)

/* HPD sense bit used to gate the bit-bang so we do not toggle dead lines
 * (DC_HPD_SENSE already defined above). Require HPD-positive by default; set
 * to 0 to bit-bang regardless (parent may loosen for a bring-up boot). */
#ifndef AMDGPU_GPIO_DDC_REQUIRE_HPD
#define AMDGPU_GPIO_DDC_REQUIRE_HPD 1
#endif

/* Resolved DDC GPIO register offsets for one connector (radeonsi_display.c:191).
 * All *_offset fields are DWORD offsets into rmmio (see adaptation note). */
struct AmdgpuGpioDDC {
    ULONG gpio_id;
    ULONG data_a_offset;
    ULONG mask_offset;
    ULONG en_offset;
    ULONG y_offset;
    ULONG saved_a;
    ULONG saved_mask;
    ULONG saved_en;
};

/* ---- little-endian ATOM readers (radeonsi_display.c:968-993) ------------- */
static UWORD amdgpu_atom_u16(const UBYTE *rom, ULONG off)
{
    return (UWORD)(rom[off] | (rom[off + 1] << 8));
}

static ULONG amdgpu_atom_u32(const UBYTE *rom, ULONG off)
{
    return (ULONG)(rom[off] | (rom[off + 1] << 8) |
                   (rom[off + 2] << 16) | (rom[off + 3] << 24));
}

static ULONG amdgpu_atom_rom_size(const UBYTE *rom, ULONG fallback)
{
    ULONG image_size = rom[AMDGPU_ATOM_ROM_IMAGE_SIZE] * 512UL;
    if (image_size && image_size <= fallback)
        return image_size;
    return fallback;
}

static BOOL amdgpu_atom_offset_valid(ULONG off, ULONG need, ULONG rom_size)
{
    return (off < rom_size && need <= rom_size - off) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------------ *
 * VBIOS fetch — copy the PCI expansion ROM into ad->rom_base.
 * Port of radeonsi_load_vbios_copy() (radeonsi_minimal_stubs.c:333-402),
 * using amdgpu's PCI conventions (HIDD_PCIDriver_MapPCI like amdgpu_mmio.c,
 * OOP attrs via amdgpuBase.pciDeviceAB like amdgpu_pci.c).
 *
 * >>> VBIOS-ACCESS UNCERTAINTIES FOR THE PARENT <<<
 *   (V1) We enable ROM decode by writing PCI config 0x30 (EXPROM_BASE) with the
 *        low enable bit set, then RESTORE it afterwards — exactly as radeonsi
 *        does. This is a real config write; it is the only way to read the ROM
 *        BAR. Restored on every exit path.
 *   (V2) rom_addr is masked & 0xFFFFF800 (radeonsi_minimal_stubs.c:346) to drop
 *        the enable/reserved low bits before mapping.
 *   (V3) If the card advertises no expansion ROM (RomBase/RomSize == 0), we bail
 *        — no ISA 0xC0000 shadow fallback is attempted (that legacy path only
 *        applies to the POSTed primary VGA; a discrete RDNA4 card's ATOM image
 *        lives in its own ROM BAR). radeon/ati_init.c:159 shows the shadow path
 *        for reference if the parent wants it added.
 * ------------------------------------------------------------------------ */
BOOL amdgpu_display_fetch_vbios(struct AmdgpuDevice *ad)
{
    IPTR rom_addr = 0, rom_size = 0;
    APTR rom_virt, rom_copy;

    if (!ad || !ad->pciDevice || !ad->pciDriver)
        return FALSE;
    if (ad->rom_allocated && ad->rom_base && ad->rom_size)
        return TRUE;   /* already fetched */

    OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_RomBase, &rom_addr);
    OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_RomSize, &rom_size);
    rom_addr &= 0xFFFFF800UL;   /* (V2) */

    bug("[amdgpu-display] VBIOS: ROM BAR=0x%p size=%lu bytes\n",
        (APTR)rom_addr, (unsigned long)rom_size);

    if (!rom_addr || !rom_size)
    {
        D(bug("[amdgpu-display] VBIOS: no PCI expansion ROM advertised (V3)\n"));
        return FALSE;
    }
    if (rom_size > 2 * 1024 * 1024)
        rom_size = 2 * 1024 * 1024;

    /* (V1) enable ROM decode: config 0x30 = addr | 1 */
    HIDD_PCIDevice_WriteConfigLong(ad->pciDevice, 0x30, (ULONG)rom_addr | 1);

    rom_virt = HIDD_PCIDriver_MapPCI(ad->pciDriver, (APTR)rom_addr, (ULONG)rom_size);
    if (!rom_virt || rom_virt == (APTR)-1)
    {
        D(bug("[amdgpu-display] VBIOS: MapPCI failed for ROM BAR 0x%p\n", (APTR)rom_addr));
        HIDD_PCIDevice_WriteConfigLong(ad->pciDevice, 0x30, (ULONG)rom_addr);
        return FALSE;
    }

    rom_copy = AllocVec((ULONG)rom_size, MEMF_PUBLIC);
    if (!rom_copy)
    {
        D(bug("[amdgpu-display] VBIOS: AllocVec(%lu) failed\n", (unsigned long)rom_size));
        HIDD_PCIDriver_UnmapPCI(ad->pciDriver, rom_virt, (ULONG)rom_size);
        HIDD_PCIDevice_WriteConfigLong(ad->pciDevice, 0x30, (ULONG)rom_addr);
        return FALSE;
    }

    CopyMem(rom_virt, rom_copy, (ULONG)rom_size);
    HIDD_PCIDriver_UnmapPCI(ad->pciDriver, rom_virt, (ULONG)rom_size);
    HIDD_PCIDevice_WriteConfigLong(ad->pciDevice, 0x30, (ULONG)rom_addr);   /* restore */

    if (((UBYTE *)rom_copy)[0] != 0x55 || ((UBYTE *)rom_copy)[1] != 0xAA)
    {
        D(bug("[amdgpu-display] VBIOS: invalid ROM signature %02x %02x\n",
              ((UBYTE *)rom_copy)[0], ((UBYTE *)rom_copy)[1]));
        FreeVec(rom_copy);
        return FALSE;
    }

    if (ad->rom_allocated && ad->rom_base)
        FreeVec(ad->rom_base);
    ad->rom_base = (UBYTE *)rom_copy;
    ad->rom_size = (ULONG)rom_size;
    ad->rom_allocated = TRUE;

    bug("[amdgpu-display] VBIOS: copied %lu bytes from PCI expansion ROM (sig 55 AA OK)\n",
        (unsigned long)ad->rom_size);
    return TRUE;
}

/* ------------------------------------------------------------------------ *
 * ATOM displayobjectinfo parse — fill ad->atom_connectors[] with per-connector
 * i2c_id + hpd_pin. Port of RadeonsiParseAtomRecords (radeonsi_display.c:1414)
 * + RadeonsiParseAtomDisplayInfo (radeonsi_display.c:1461-1612).
 * ------------------------------------------------------------------------ */
static void amdgpu_atom_parse_records(struct AmdgpuDevice *ad, ULONG ci,
                                      ULONG records_offset, ULONG table_end,
                                      const UBYTE *rom, ULONG rom_size)
{
    ULONG off = records_offset, guard = 0;

    while (amdgpu_atom_offset_valid(off, 2, rom_size) && off < table_end && guard++ < 64)
    {
        UBYTE rtype = rom[off];
        UBYTE rsize = rom[off + 1];

        if (rtype == AMDGPU_ATOM_OBJECT_RECORD_END || rsize == 0)
            break;
        if (!amdgpu_atom_offset_valid(off, rsize, rom_size) || off + rsize > table_end)
            break;

        if (rtype == AMDGPU_ATOM_OBJECT_RECORD_I2C && rsize >= 4)
        {
            UBYTE i2c_id = rom[off + 2];
            ad->atom_connectors[ci].has_i2c = TRUE;
            ad->atom_connectors[ci].i2c_id = i2c_id;
            ad->atom_connectors[ci].i2c_slave_addr = rom[off + 3];
            ad->atom_connectors[ci].i2c_line = i2c_id & AMDGPU_ATOM_I2C_HW_LANE_MUX;
            ad->atom_connectors[ci].i2c_engine_id =
                (i2c_id & AMDGPU_ATOM_I2C_HW_ENGINE_ID_MASK) >> 4;
        }
        else if (rtype == AMDGPU_ATOM_OBJECT_RECORD_HPD && rsize >= 4)
        {
            ad->atom_connectors[ci].has_hpd = TRUE;
            ad->atom_connectors[ci].hpd_pin = rom[off + 2];
            ad->atom_connectors[ci].hpd_state = rom[off + 3];
        }

        off += rsize;
    }
}

BOOL amdgpu_display_parse_atom(struct AmdgpuDevice *ad)
{
    const UBYTE *rom;
    ULONG rom_size, rom_header, master_data, display_info;
    ULONG table_size, table_end, paths_offset, path_size, path;
    UBYTE format_rev, content_rev, number_of_path;
    ULONG cc = 0;

    if (!ad)
        return FALSE;
    if (ad->atom_display_valid)
        return TRUE;
    if (!amdgpu_display_fetch_vbios(ad))
        return FALSE;

    rom = ad->rom_base;
    rom_size = amdgpu_atom_rom_size(rom, ad->rom_size);

    if (!amdgpu_atom_offset_valid(0, 0x4a, rom_size) || rom[0] != 0x55 || rom[1] != 0xAA)
    {
        D(bug("[amdgpu-display] ATOM: invalid VBIOS signature/size\n"));
        return FALSE;
    }

    rom_header = amdgpu_atom_u16(rom, AMDGPU_ATOM_ROM_HEADER_PTR);
    if (!amdgpu_atom_offset_valid(rom_header, AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET + 2, rom_size))
        return FALSE;

    master_data = amdgpu_atom_u16(rom, rom_header + AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET);
    if (!amdgpu_atom_offset_valid(master_data, AMDGPU_ATOM_MASTER_DISPLAY_OBJECT_INFO + 2, rom_size))
        return FALSE;

    display_info = amdgpu_atom_u16(rom, master_data + AMDGPU_ATOM_MASTER_DISPLAY_OBJECT_INFO);
    if (!display_info || !amdgpu_atom_offset_valid(display_info, 8, rom_size))
    {
        D(bug("[amdgpu-display] ATOM: displayobjectinfo missing (0x%lx)\n",
              (unsigned long)display_info));
        return FALSE;
    }

    table_size = amdgpu_atom_u16(rom, display_info);
    format_rev = rom[display_info + 2];
    content_rev = rom[display_info + 3];
    if (!table_size || !amdgpu_atom_offset_valid(display_info, table_size, rom_size))
        return FALSE;

    table_end = display_info + table_size;
    number_of_path = rom[display_info + 6];
    path_size = (content_rev == 5) ? AMDGPU_ATOM_DISPLAY_PATH_V1_5_SIZE
                                   : AMDGPU_ATOM_DISPLAY_PATH_V1_4_SIZE;
    paths_offset = display_info + 8;

    bug("[amdgpu-display] ATOM: displayobjectinfo v%u.%u off=0x%lx size=%lu paths=%u\n",
        format_rev, content_rev, (unsigned long)display_info,
        (unsigned long)table_size, number_of_path);

    if (format_rev != 1 || (content_rev != 4 && content_rev != 5))
    {
        D(bug("[amdgpu-display] ATOM: unsupported displayobjectinfo rev v%u.%u\n",
              format_rev, content_rev));
        return FALSE;
    }

    dsp_memzero(ad->atom_connectors, sizeof(ad->atom_connectors));

    for (path = 0; path < number_of_path && cc < AMDGPU_ATOM_MAX_CONNECTORS; path++)
    {
        ULONG po = paths_offset + path * path_size;
        UWORD display_objid, disp_recordoffset, encoder_objid;

        if (!amdgpu_atom_offset_valid(po, path_size, rom_size) || po + path_size > table_end)
            break;

        display_objid     = amdgpu_atom_u16(rom, po);
        disp_recordoffset = amdgpu_atom_u16(rom, po + 2);
        encoder_objid     = amdgpu_atom_u16(rom, po + 4);

        if (encoder_objid == 0 || display_objid == 0 ||
            (display_objid & AMDGPU_ATOM_OBJECT_TYPE_MASK) != AMDGPU_ATOM_OBJECT_TYPE_CONNECTOR)
            continue;

        ad->atom_connectors[cc].valid = TRUE;
        ad->atom_connectors[cc].display_objid = display_objid;
        ad->atom_connectors[cc].encoder_objid = encoder_objid;

        if (disp_recordoffset && display_info + disp_recordoffset < table_end)
            amdgpu_atom_parse_records(ad, cc, display_info + disp_recordoffset,
                                      table_end, rom, rom_size);

        bug("[amdgpu-display] ATOM: connector %lu obj=0x%04x enc=0x%04x "
            "i2c=%s(id=0x%02x line=%u engine=%u) hpd=%s(pin=%u)\n",
            (unsigned long)cc, display_objid, encoder_objid,
            ad->atom_connectors[cc].has_i2c ? "yes" : "no",
            ad->atom_connectors[cc].i2c_id,
            ad->atom_connectors[cc].i2c_line,
            ad->atom_connectors[cc].i2c_engine_id,
            ad->atom_connectors[cc].has_hpd ? "yes" : "no",
            ad->atom_connectors[cc].hpd_pin);
        cc++;
    }

    ad->atom_connector_count = cc;
    ad->atom_display_valid = TRUE;
    bug("[amdgpu-display] ATOM: parsed %lu connector route(s)\n", (unsigned long)cc);
    return cc ? TRUE : FALSE;
}

/* ------------------------------------------------------------------------ *
 * GPIO_Pin_LUT walk: i2c_id -> DDC GPIO register offsets.
 * Port of RadeonsiFindGpioDDCPin (radeonsi_display.c:394-462). See the CRITICAL
 * ADAPTATION note: offsets are DWORD (no *4), read absolutely off rmmio.
 * ------------------------------------------------------------------------ */
static BOOL amdgpu_atom_find_gpio_ddc(struct AmdgpuDevice *ad, UBYTE i2c_id,
                                      struct AmdgpuGpioDDC *gpio)
{
    const UBYTE *rom;
    ULONG rom_size, rom_header, master_data, table_entry, table, table_size, count, entry;

    if (!ad || !gpio || !ad->rom_base || !ad->rom_size)
        return FALSE;

    rom = ad->rom_base;
    rom_size = amdgpu_atom_rom_size(rom, ad->rom_size);

    if (!amdgpu_atom_offset_valid(0, 0x4a, rom_size) || rom[0] != 0x55 || rom[1] != 0xAA)
        return FALSE;

    rom_header = amdgpu_atom_u16(rom, AMDGPU_ATOM_ROM_HEADER_PTR);
    if (!amdgpu_atom_offset_valid(rom_header, AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET + 2, rom_size))
        return FALSE;

    master_data = amdgpu_atom_u16(rom, rom_header + AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET);
    table_entry = master_data + AMDGPU_ATOM_COMMON_HEADER_SIZE +
                  (AMDGPU_ATOM_DATA_GPIO_PIN_LUT * 2);
    if (!amdgpu_atom_offset_valid(table_entry, 2, rom_size))
        return FALSE;

    table = amdgpu_atom_u16(rom, table_entry);
    if (!table || !amdgpu_atom_offset_valid(table, AMDGPU_ATOM_COMMON_HEADER_SIZE, rom_size))
        return FALSE;

    table_size = amdgpu_atom_u16(rom, table);
    if (table_size < AMDGPU_ATOM_COMMON_HEADER_SIZE + AMDGPU_ATOM_GPIO_PIN_ASSIGNMENT_SIZE ||
        !amdgpu_atom_offset_valid(table, table_size, rom_size))
        return FALSE;

    count = (table_size - AMDGPU_ATOM_COMMON_HEADER_SIZE) / AMDGPU_ATOM_GPIO_PIN_ASSIGNMENT_SIZE;
    for (entry = 0; entry < count; entry++)
    {
        ULONG off = table + AMDGPU_ATOM_COMMON_HEADER_SIZE +
                    (entry * AMDGPU_ATOM_GPIO_PIN_ASSIGNMENT_SIZE);
        ULONG data_a_index;
        UBYTE gpio_id;

        if (!amdgpu_atom_offset_valid(off, AMDGPU_ATOM_GPIO_PIN_ASSIGNMENT_SIZE, rom_size))
            break;

        data_a_index = amdgpu_atom_u32(rom, off + 0);
        gpio_id = rom[off + 6];

        if (gpio_id != i2c_id)
            continue;

        dsp_memzero(gpio, sizeof(*gpio));
        gpio->gpio_id       = gpio_id;
        gpio->data_a_offset = data_a_index;        /* DWORD (radeonsi: *4)      */
        gpio->mask_offset   = data_a_index - 1;    /* DWORD (radeonsi: (i-1)*4) */
        gpio->en_offset     = data_a_index + 1;    /* DWORD (radeonsi: (i+1)*4) */
        gpio->y_offset      = data_a_index + 2;    /* DWORD (radeonsi: (i+2)*4) */

        bug("[amdgpu-display] GPIO_Pin_LUT: i2c_id=0x%02x -> A=dw0x%lx MASK=dw0x%lx "
            "EN=dw0x%lx Y=dw0x%lx (data_a_index=0x%lx)\n",
            gpio_id, (unsigned long)gpio->data_a_offset,
            (unsigned long)gpio->mask_offset, (unsigned long)gpio->en_offset,
            (unsigned long)gpio->y_offset, (unsigned long)data_a_index);
        return TRUE;
    }

    D(bug("[amdgpu-display] GPIO_Pin_LUT: no entry for i2c_id=0x%02x\n", i2c_id));
    return FALSE;
}

/* Locate the VBIOS ATOM powerplayinfo table and return a host pointer to the
 * pmfw PPTable_t (the smc_pptable that PMFW/SMU expects), filling *size_out with
 * its byte length. Returns NULL on any parse failure. The pointer is into
 * ad->rom_base (the driver's host copy of the VBIOS), valid for the driver's
 * life. Used by amdgpu_smu.c to transfer the pptable to the SMU (the prerequisite
 * for SetAllowedFeaturesMask). The wrapper is struct smu_14_0_2_powerplay_table:
 *   +0  atom_common_table_header (4)
 *   +4  table_revision (1), +5 pptable_source (1)
 *   +6  pmfw_pptable_start_offset (u16) = start of PPTable_t within the wrapper
 *   +8  pmfw_pptable_size        (u16) = sizeof(PPTable_t)
 * Reading start/size from the ROM (not a hardcoded sizeof) keeps this robust to
 * pptable version changes. */
const UBYTE *amdgpu_atom_get_pptable(struct AmdgpuDevice *ad, ULONG *size_out)
{
    const UBYTE *rom;
    ULONG rom_size, rom_header, master_data, pp_entry, pp_table;
    ULONG pmfw_off, pmfw_size;

    if (!ad)
        return NULL;
    /* The VBIOS fetch is lazy and normally happens during display (dcn) init,
     * which runs AFTER 'smu' in the IP ladder — so at smu_hw_init time rom_base
     * is still NULL. Fetch it here on demand (idempotent) so the pptable is
     * available for the SMU feature-enable prerequisite. */
    if (!ad->rom_base)
        amdgpu_display_fetch_vbios(ad);
    if (!ad->rom_base || !ad->rom_size)
        return NULL;

    rom = ad->rom_base;
    rom_size = amdgpu_atom_rom_size(rom, ad->rom_size);

    if (!amdgpu_atom_offset_valid(0, 0x4a, rom_size) || rom[0] != 0x55 || rom[1] != 0xAA) {
        bug("[amdgpu-display] ATOM pptable: bad sig/size rom_size=%lu sig=%02x%02x\n",
            (unsigned long)rom_size, rom[0], rom[1]);
        return NULL;
    }

    rom_header = amdgpu_atom_u16(rom, AMDGPU_ATOM_ROM_HEADER_PTR);
    if (!amdgpu_atom_offset_valid(rom_header, AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET + 2, rom_size)) {
        bug("[amdgpu-display] ATOM pptable: bad rom_header=0x%lx (rom_size=%lu)\n",
            (unsigned long)rom_header, (unsigned long)rom_size);
        return NULL;
    }

    master_data = amdgpu_atom_u16(rom, rom_header + AMDGPU_ATOM_ROM_MASTER_DATA_OFFSET);
    pp_entry = master_data + AMDGPU_ATOM_COMMON_HEADER_SIZE +
               (AMDGPU_ATOM_DATA_POWERPLAYINFO * 2);
    if (!amdgpu_atom_offset_valid(pp_entry, 2, rom_size)) {
        bug("[amdgpu-display] ATOM pptable: bad pp_entry=0x%lx (master_data=0x%lx rom_size=%lu)\n",
            (unsigned long)pp_entry, (unsigned long)master_data, (unsigned long)rom_size);
        return NULL;
    }

    pp_table = amdgpu_atom_u16(rom, pp_entry);
    if (!pp_table || !amdgpu_atom_offset_valid(pp_table, 10, rom_size)) {
        bug("[amdgpu-display] ATOM pptable: pp_table=0x%lx invalid (master_data=0x%lx "
            "pp_entry=0x%lx rom_size=%lu)\n", (unsigned long)pp_table,
            (unsigned long)master_data, (unsigned long)pp_entry, (unsigned long)rom_size);
        return NULL;
    }

    pmfw_off  = amdgpu_atom_u16(rom, pp_table + 6);
    pmfw_size = amdgpu_atom_u16(rom, pp_table + 8);
    if (!pmfw_off || !pmfw_size ||
        !amdgpu_atom_offset_valid(pp_table + pmfw_off, pmfw_size + 4, rom_size)) {
        bug("[amdgpu-display] ATOM pptable: bad pmfw off=0x%lx size=0x%lx (pp_table@0x%lx)\n",
            (unsigned long)pmfw_off, (unsigned long)pmfw_size, (unsigned long)pp_table);
        return NULL;
    }

    bug("[amdgpu-display] ATOM powerplayinfo @0x%lx: PPTable_t off=0x%lx size=%lu bytes\n",
        (unsigned long)pp_table, (unsigned long)pmfw_off, (unsigned long)pmfw_size);
    if (size_out)
        *size_out = pmfw_size;
    return rom + pp_table + pmfw_off;
}

/* ---- bit-bang primitives (radeonsi_display.c:386-654) ------------------- *
 * All register touches use amdgpu_rreg32/amdgpu_wreg32 at the ABSOLUTE DWORD
 * offsets resolved above. The only writes are to the GPIO EN register, which
 * merely tri-states / drives the DDC SCL/SDA lines low (open-drain) — harmless.
 * ------------------------------------------------------------------------ */
static void amdgpu_gpio_ddc_delay(void)
{
    volatile ULONG i;
    for (i = 0; i < 4000; i++)
        ;
}

static void amdgpu_gpio_ddc_put(struct AmdgpuDevice *ad,
                                const struct AmdgpuGpioDDC *gpio, BOOL scl, BOOL sda)
{
    ULONG en = amdgpu_rreg32(ad, gpio->en_offset);

    if (scl) en &= ~AMDGPU_GPIO_DDC_SCL_MASK; else en |= AMDGPU_GPIO_DDC_SCL_MASK;
    if (sda) en &= ~AMDGPU_GPIO_DDC_SDA_MASK; else en |= AMDGPU_GPIO_DDC_SDA_MASK;

    amdgpu_wreg32(ad, gpio->en_offset, en);
    amdgpu_gpio_ddc_delay();
}

static void amdgpu_gpio_ddc_get(struct AmdgpuDevice *ad,
                                const struct AmdgpuGpioDDC *gpio, BOOL *scl, BOOL *sda)
{
    ULONG y = amdgpu_rreg32(ad, gpio->y_offset);
    if (scl) *scl = (y & AMDGPU_GPIO_DDC_SCL_MASK) ? TRUE : FALSE;
    if (sda) *sda = (y & AMDGPU_GPIO_DDC_SDA_MASK) ? TRUE : FALSE;
}

static BOOL amdgpu_gpio_ddc_open(struct AmdgpuDevice *ad, struct AmdgpuGpioDDC *gpio)
{
    ULONG a, mask, en, y_after;

    gpio->saved_a    = amdgpu_rreg32(ad, gpio->data_a_offset);
    gpio->saved_mask = amdgpu_rreg32(ad, gpio->mask_offset);
    gpio->saved_en   = amdgpu_rreg32(ad, gpio->en_offset);

    a    = gpio->saved_a    & ~AMDGPU_GPIO_DDC_LINE_MASK;
    mask = gpio->saved_mask |  AMDGPU_GPIO_DDC_LINE_MASK;
    en   = gpio->saved_en   & ~AMDGPU_GPIO_DDC_LINE_MASK;

    amdgpu_wreg32(ad, gpio->data_a_offset, a);
    amdgpu_wreg32(ad, gpio->mask_offset, mask);
    amdgpu_wreg32(ad, gpio->en_offset, en);
    amdgpu_gpio_ddc_delay();

    y_after = amdgpu_rreg32(ad, gpio->y_offset);
    bug("[amdgpu-display] GPIO DDC open id=0x%02lx saved A=0x%08lx MASK=0x%08lx EN=0x%08lx "
        "-> Y=0x%08lx scl=%u sda=%u\n",
        (unsigned long)gpio->gpio_id, (unsigned long)gpio->saved_a,
        (unsigned long)gpio->saved_mask, (unsigned long)gpio->saved_en,
        (unsigned long)y_after,
        (y_after & AMDGPU_GPIO_DDC_SCL_MASK) ? 1 : 0,
        (y_after & AMDGPU_GPIO_DDC_SDA_MASK) ? 1 : 0);

    return ((y_after & AMDGPU_GPIO_DDC_LINE_MASK) == AMDGPU_GPIO_DDC_LINE_MASK) ? TRUE : FALSE;
}

static void amdgpu_gpio_ddc_close(struct AmdgpuDevice *ad, const struct AmdgpuGpioDDC *gpio)
{
    amdgpu_wreg32(ad, gpio->en_offset, gpio->saved_en);
    amdgpu_wreg32(ad, gpio->data_a_offset, gpio->saved_a);
    amdgpu_wreg32(ad, gpio->mask_offset, gpio->saved_mask);
    amdgpu_gpio_ddc_delay();
}

static BOOL amdgpu_gpio_ddc_write_bit(struct AmdgpuDevice *ad,
                                      const struct AmdgpuGpioDDC *gpio, BOOL bit)
{
    BOOL scl, sda;
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, bit);
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, bit);
    amdgpu_gpio_ddc_get(ad, gpio, &scl, &sda);
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, bit);
    return scl ? TRUE : FALSE;
}

static BOOL amdgpu_gpio_ddc_read_bit(struct AmdgpuDevice *ad,
                                     const struct AmdgpuGpioDDC *gpio, BOOL *bit)
{
    BOOL scl, sda;
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, TRUE);
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, TRUE);
    amdgpu_gpio_ddc_get(ad, gpio, &scl, &sda);
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, TRUE);
    if (bit) *bit = sda;
    return scl ? TRUE : FALSE;
}

static void amdgpu_gpio_ddc_start(struct AmdgpuDevice *ad, const struct AmdgpuGpioDDC *gpio)
{
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, TRUE);
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, FALSE);
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, FALSE);
}

static void amdgpu_gpio_ddc_stop(struct AmdgpuDevice *ad, const struct AmdgpuGpioDDC *gpio)
{
    amdgpu_gpio_ddc_put(ad, gpio, FALSE, FALSE);
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, FALSE);
    amdgpu_gpio_ddc_put(ad, gpio, TRUE, TRUE);
}

static BOOL amdgpu_gpio_ddc_write_byte(struct AmdgpuDevice *ad,
                                       const struct AmdgpuGpioDDC *gpio, UBYTE value)
{
    ULONG bit;
    BOOL ack;

    for (bit = 0; bit < 8; bit++)
        if (!amdgpu_gpio_ddc_write_bit(ad, gpio, (value & (0x80 >> bit)) ? TRUE : FALSE))
            return FALSE;

    if (!amdgpu_gpio_ddc_read_bit(ad, gpio, &ack))
        return FALSE;
    return ack ? FALSE : TRUE;   /* ACK == SDA low */
}

static BOOL amdgpu_gpio_ddc_read_byte(struct AmdgpuDevice *ad,
                                      const struct AmdgpuGpioDDC *gpio,
                                      UBYTE *value, BOOL ack)
{
    ULONG bit;
    UBYTE data = 0;
    BOOL bv;

    for (bit = 0; bit < 8; bit++)
    {
        if (!amdgpu_gpio_ddc_read_bit(ad, gpio, &bv))
            return FALSE;
        if (bv)
            data |= (0x80 >> bit);
    }

    if (!amdgpu_gpio_ddc_write_bit(ad, gpio, ack ? FALSE : TRUE))
        return FALSE;
    if (value)
        *value = data;
    return TRUE;
}

/* HPD sense off the DCN block at hpd_pin-1 (radeonsi RadeonsiSampleHPD,
 * radeonsi_display.c:293-346). Reuses amdgpu_dcn.c's HPD register path. */
static BOOL amdgpu_atom_hpd_sensed(struct AmdgpuDevice *ad, ULONG connector)
{
    ULONG dcn, hpd_pin, idx, status;

    if (!ad->ip.dmu.valid)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn || connector >= ad->atom_connector_count ||
        !ad->atom_connectors[connector].has_hpd)
        return FALSE;

    hpd_pin = ad->atom_connectors[connector].hpd_pin;
    if (hpd_pin == 0 || hpd_pin > 5)
        return FALSE;
    idx = hpd_pin - 1;

    status = amdgpu_rreg32(ad, dcn + AMDGPU_HPD_INT_STATUS(idx));
    if (status == 0xFFFFFFFFUL)
        return FALSE;

    ad->atom_connectors[connector].hpd_state =
        (status & (DC_HPD_SENSE_DELAYED | DC_HPD_SENSE)) ? 1 : 0;
    return ad->atom_connectors[connector].hpd_state ? TRUE : FALSE;
}

/* Public: GPIO bit-banged DDC EDID read. Port of RadeonsiGpioDDCReadEDID
 * (radeonsi_display.c:656-776). Resolves connector -> i2c_id -> GPIO offsets,
 * then runs the standard addr-0x50 EDID transaction and validates the header. */
BOOL amdgpu_display_read_edid_gpio(struct AmdgpuDevice *ad, ULONG connector,
                                   UBYTE *edid, ULONG size)
{
    struct AmdgpuGpioDDC gpio;
    UBYTE i2c_id;
    ULONG read_len, i, checksum = 0;
    BOOL ok = FALSE;

    if (!ad || !edid || size < 128)
        return FALSE;
    if (!ad->rmmio)
        return FALSE;

    if (!amdgpu_display_parse_atom(ad))
        return FALSE;
    if (connector >= ad->atom_connector_count ||
        !ad->atom_connectors[connector].valid ||
        !ad->atom_connectors[connector].has_i2c)
    {
        D(bug("[amdgpu-display] GPIO DDC: connector %lu has no ATOM I2C route\n",
              (unsigned long)connector));
        return FALSE;
    }

#if AMDGPU_GPIO_DDC_REQUIRE_HPD
    if (ad->atom_connectors[connector].has_hpd &&
        !amdgpu_atom_hpd_sensed(ad, connector))
    {
        D(bug("[amdgpu-display] GPIO DDC: connector %lu not HPD-positive; skipping\n",
              (unsigned long)connector));
        return FALSE;
    }
#endif

    i2c_id = ad->atom_connectors[connector].i2c_id;
    if (!amdgpu_atom_find_gpio_ddc(ad, i2c_id, &gpio))
        return FALSE;

    read_len = (size < 128) ? size : 128;
    dsp_memzero(edid, size);

    bug("[amdgpu-display] GPIO DDC EDID read: connector=%lu i2c_id=0x%02x read_len=%lu\n",
        (unsigned long)connector, i2c_id, (unsigned long)read_len);

    if (!amdgpu_gpio_ddc_open(ad, &gpio))
    {
        D(bug("[amdgpu-display] GPIO DDC: SCL/SDA not both high after open — aborting\n"));
        amdgpu_gpio_ddc_close(ad, &gpio);
        return FALSE;
    }

    amdgpu_gpio_ddc_start(ad, &gpio);
    if (!amdgpu_gpio_ddc_write_byte(ad, &gpio, 0xA0))          /* write addr 0x50<<1 */
    {
        D(bug("[amdgpu-display] GPIO DDC: NACK on write addr 0xA0\n"));
        goto out;
    }
    if (!amdgpu_gpio_ddc_write_byte(ad, &gpio, 0x00))          /* EDID sub-offset 0  */
    {
        D(bug("[amdgpu-display] GPIO DDC: NACK on offset 0x00\n"));
        goto out;
    }
    amdgpu_gpio_ddc_start(ad, &gpio);                          /* repeated START     */
    if (!amdgpu_gpio_ddc_write_byte(ad, &gpio, 0xA1))          /* read addr          */
    {
        D(bug("[amdgpu-display] GPIO DDC: NACK on read addr 0xA1\n"));
        goto out;
    }

    for (i = 0; i < read_len; i++)
    {
        if (!amdgpu_gpio_ddc_read_byte(ad, &gpio, &edid[i],
                                       (i + 1 < read_len) ? TRUE : FALSE))
        {
            D(bug("[amdgpu-display] GPIO DDC: read failed at byte %lu\n", (unsigned long)i));
            goto out;
        }
    }

    if (read_len >= 8)
        bug("[amdgpu-display] GPIO DDC header: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            edid[0], edid[1], edid[2], edid[3], edid[4], edid[5], edid[6], edid[7]);

    for (i = 0; i < 128; i++)
        checksum = (checksum + edid[i]) & 0xFF;

    if (edid[0] == 0x00 && edid[1] == 0xFF && edid[2] == 0xFF && edid[3] == 0xFF &&
        edid[4] == 0xFF && edid[5] == 0xFF && edid[6] == 0xFF && edid[7] == 0x00 &&
        checksum == 0)
    {
        bug("[amdgpu-display] GPIO DDC: VALID EDID on connector %lu (i2c_id=0x%02x)\n",
            (unsigned long)connector, i2c_id);
        ok = TRUE;
    }
    else
    {
        D(bug("[amdgpu-display] GPIO DDC: invalid EDID checksum=0x%02lx\n",
              (unsigned long)checksum));
    }

out:
    amdgpu_gpio_ddc_stop(ad, &gpio);
    amdgpu_gpio_ddc_close(ad, &gpio);
    return ok;
}

/* Read a 128-byte EDID block from `connector` over DDC channel `ddc_channel`.
 * Returns TRUE only on a valid EDID header. See sequence comment above. */
BOOL amdgpu_display_read_edid(struct AmdgpuDevice *ad, ULONG connector,
                              UBYTE *edid, ULONG size)
{
    ULONG dcn, ddc_channel;
#if AMDGPU_ENABLE_DCN_I2C_WRITES
    ULONG read_len, i, control, status, timeout;
#endif

    if (!ad || !edid || size < 128)
        return FALSE;
    if (!ad->rmmio)
        return FALSE;

    /* PREFERRED PATH: GPIO bit-banged DDC. The DC_I2C engine reads dead on
     * RDNA4, so try the VBIOS-ATOM-routed GPIO bit-bang first; only fall through
     * to the (gated-off) DC_I2C engine below if GPIO could not resolve/route. */
    if (amdgpu_display_read_edid_gpio(ad, connector, edid, size))
        return TRUE;

    if (!ad->ip.dmu.valid)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return FALSE;

    /* No ATOM DDC-line routing (UNCERTAINTY 4): assume channel == connector. */
    ddc_channel = connector;
    if (ddc_channel >= AMDGPU_DDC_MAX_CHANNELS)
    {
        D(bug("[amdgpu-display] EDID: connector %lu DDC channel %lu outside range\n",
              (unsigned long)connector, (unsigned long)ddc_channel));
        return FALSE;
    }

    dsp_memzero(edid, size);

    D(bug("[amdgpu-display] EDID read: connector=%lu ddc=%lu (dcn seg2=0x%05lx)\n",
          (unsigned long)connector, (unsigned long)ddc_channel,
          (unsigned long)dcn));

    if (amdgpu_ddc_block_all_ones(ad, dcn))
        return FALSE;

#if !AMDGPU_ENABLE_DCN_I2C_WRITES
    D(bug("[amdgpu-display] EDID: DCN I2C WRITE path disabled "
          "(AMDGPU_ENABLE_DCN_I2C_WRITES=0) — read-only guard only, no transaction. "
          "Flip the macro to exercise the full DDC sequence on hardware.\n"));
    return FALSE;
#else
    read_len = 128;

    if (!amdgpu_ddc_acquire(ad, dcn))
    {
        D(bug("[amdgpu-display] EDID: SW arbitration not granted — aborting\n"));
        return FALSE;
    }

    /* (c) reset + select channel */
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_CONTROL,
                  DC_I2C_SW_STATUS_RESET | DC_I2C_DDC_SELECT(ddc_channel));

    /* (d) address phase */
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_DATA,
                  DC_I2C_DATA_INDEX_WRITE | DC_I2C_DATA_INDEX(0) |
                  DC_I2C_DATA_BYTE(AMDGPU_EDID_ADDR7));
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_DATA, DC_I2C_DATA_BYTE(0));
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_DATA,
                  DC_I2C_DATA_BYTE(AMDGPU_EDID_ADDR7) | DC_I2C_DATA_RW);

    /* (e) transactions: write sub-offset, then read read_len bytes */
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_TRANSACTION_0,
                  DC_I2C_TX_STOP_ON_NACK | DC_I2C_TX_START | DC_I2C_TX_COUNT(1));
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_TRANSACTION_1,
                  DC_I2C_TX_STOP_ON_NACK | DC_I2C_TX_START | DC_I2C_TX_RW |
                  DC_I2C_TX_STOP | DC_I2C_TX_COUNT(read_len));

    /* (f) go */
    control = DC_I2C_DDC_SELECT(ddc_channel) | DC_I2C_TRANSACTION_COUNT(1);
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_CONTROL, control);
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_CONTROL, control | DC_I2C_GO);

    /* (g) poll for completion */
    status = 0;
    timeout = 100000;
    do {
        status = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_SW_STATUS);
        if (status & (DC_I2C_SW_DONE | DC_I2C_SW_STOPPED_ON_NACK |
                      DC_I2C_SW_TIMEOUT | DC_I2C_SW_ABORTED))
            break;
    } while (--timeout);

    D(bug("[amdgpu-display] EDID: transaction status=0x%08lx timeout_left=%lu\n",
          (unsigned long)status, (unsigned long)timeout));

    if (timeout == 0 ||
        (status & (DC_I2C_SW_STOPPED_ON_NACK | DC_I2C_SW_TIMEOUT | DC_I2C_SW_ABORTED)) ||
        !(status & DC_I2C_SW_DONE))
    {
        amdgpu_ddc_release(ad, dcn);
        return FALSE;
    }

    /* (h) read data: set read pointer to index 3, then stream */
    amdgpu_wreg32(ad, dcn + AMDGPU_DDC_DATA,
                  DC_I2C_DATA_INDEX_WRITE | DC_I2C_DATA_RW | DC_I2C_DATA_INDEX(3));
    for (i = 0; i < read_len && i < size; i++)
    {
        ULONG data = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_DATA);
        edid[i] = (UBYTE)((data >> 8) & 0xFF);
    }

    /* (i) release */
    amdgpu_ddc_release(ad, dcn);

    /* validate header 00 FF FF FF FF FF FF 00 (radeonsi_display.c:2115) */
    if (!(edid[0] == 0x00 && edid[1] == 0xFF && edid[2] == 0xFF && edid[3] == 0xFF &&
          edid[4] == 0xFF && edid[5] == 0xFF && edid[6] == 0xFF && edid[7] == 0x00))
    {
        D(bug("[amdgpu-display] EDID: invalid header\n"));
        return FALSE;
    }

    D(bug("[amdgpu-display] EDID: valid header on connector %lu\n",
          (unsigned long)connector));
    return TRUE;
#endif /* AMDGPU_ENABLE_DCN_I2C_WRITES */
}

/* ------------------------------------------------------------------------ *
 * 3. EDID parser -> struct AmdgpuDisplayMode (w/h/refresh + CVT timing).
 *    Ported from radeonsi_hiddclass.c:668-978 (harvest) and
 *    radeonsi_accel.c:135-245 (CVT timing).
 * ------------------------------------------------------------------------ */

/* Established timings: 24 DMT modes flagged by bits at EDID 35..37
 * (radeonsi_hiddclass.c:697-725). {w,h,refresh}. */
static const UWORD amdgpu_established_modes[24][3] = {
    {  720, 400, 70 }, {  720, 400, 88 }, {  640, 480, 60 }, {  640, 480, 67 },
    {  640, 480, 72 }, {  640, 480, 75 }, {  800, 600, 56 }, {  800, 600, 60 },
    {  800, 600, 72 }, {  800, 600, 75 }, {  832, 624, 75 }, { 1024, 768, 87 },
    { 1024, 768, 60 }, { 1024, 768, 70 }, { 1024, 768, 75 }, { 1280,1024, 75 },
    { 1152, 870, 75 }, {    0,   0,  0 }, {    0,   0,  0 }, {    0,   0,  0 },
    {    0,   0,  0 }, {    0,   0,  0 }, {    0,   0,  0 }, {    0,   0,  0 },
};

/* Curated base modes the GPU can drive without EDID help
 * (radeonsi_hiddclass.c:678-693). */
static const UWORD amdgpu_base_modes[][3] = {
    {  640, 480, 60 }, {  800, 600, 60 }, { 1024, 768, 60 }, { 1280, 720, 60 },
    { 1280, 800, 60 }, { 1280,1024, 60 }, { 1366, 768, 60 }, { 1440, 900, 60 },
    { 1600, 900, 60 }, { 1680,1050, 60 }, { 1920,1080, 60 }, { 1920,1200, 60 },
    { 2560,1440, 60 }, { 3840,2160, 60 },
};
#define AMDGPU_NUM_BASE_MODES (sizeof(amdgpu_base_modes) / sizeof(amdgpu_base_modes[0]))

/* CVT timing — port of RadeonsiCalculateDisplayTiming() (radeonsi_accel.c:135).
 * Fills htotal/hsync_start/hsync_end/vtotal/vsync_start/vsync_end/pixelclock.
 * refresh is plain Hz (the radeonsi code treats refresh_rate as plain Hz too). */
static void amdgpu_calc_cvt_timing(struct AmdgpuDisplayMode *m)
{
    ULONG width = m->w, height = m->h, refresh = m->refresh;
    ULONG v_sync_rnd, v_front_porch, v_back_porch, v_blank, total_v_lines;
    ULONG h_period_est, h_blank, h_sync, h_front_porch;

    if (!refresh) refresh = 60;

    /* CVT constants (radeonsi_accel.c:155-161) */
    #define AMDGPU_CVT_MIN_V_PORCH    3
    #define AMDGPU_CVT_MIN_VSYNC      4
    #define AMDGPU_CVT_MIN_V_BPORCH   6
    #define AMDGPU_CVT_H_SYNC_PERCENT 8
    #define AMDGPU_CVT_CELL_GRAN      8
    #define AMDGPU_CVT_C_PRIME        30
    #define AMDGPU_CVT_M_PRIME        300

    v_sync_rnd    = AMDGPU_CVT_MIN_VSYNC;
    v_front_porch = AMDGPU_CVT_MIN_V_PORCH;
    v_back_porch  = AMDGPU_CVT_MIN_V_BPORCH;
    v_blank       = v_front_porch + v_sync_rnd + v_back_porch;
    total_v_lines = height + v_blank;

    h_period_est = (1000000 / refresh - AMDGPU_CVT_MIN_VSYNC * 1000) / total_v_lines;

    h_blank = (width * AMDGPU_CVT_C_PRIME - AMDGPU_CVT_M_PRIME * h_period_est) /
              (100 * h_period_est / AMDGPU_CVT_CELL_GRAN - AMDGPU_CVT_C_PRIME);
    h_blank = (h_blank / (2 * AMDGPU_CVT_CELL_GRAN)) * 2 * AMDGPU_CVT_CELL_GRAN;

    h_sync = (total_v_lines * AMDGPU_CVT_H_SYNC_PERCENT) / 100;
    h_sync = (h_sync / AMDGPU_CVT_CELL_GRAN) * AMDGPU_CVT_CELL_GRAN;

    h_front_porch = h_blank / 2 - h_sync;

    m->htotal      = (UWORD)(width + h_blank);
    m->hsync_start = (UWORD)(width + h_front_porch);
    m->hsync_end   = (UWORD)(m->hsync_start + h_sync);
    m->vtotal      = (UWORD)total_v_lines;
    m->vsync_start = (UWORD)(height + v_front_porch);
    m->vsync_end   = (UWORD)(m->vsync_start + v_sync_rnd);
    m->pixelclock  = ((ULONG)m->htotal * (ULONG)m->vtotal * refresh) / 1000; /* kHz */

    /* Fallback if arithmetic degenerated (radeonsi_accel.c:224-236) */
    if (m->htotal <= width || m->vtotal <= height)
    {
        m->htotal      = (UWORD)(width + width / 8);
        m->hsync_start = (UWORD)(width + 32);
        m->hsync_end   = (UWORD)(m->hsync_start + 64);
        m->vtotal      = (UWORD)(height + height / 32);
        m->vsync_start = (UWORD)(height + 3);
        m->vsync_end   = (UWORD)(m->vsync_start + 6);
        m->pixelclock  = ((ULONG)m->htotal * (ULONG)m->vtotal * refresh) / 1000;
    }
}

/* Dedup add (radeonsi_hiddclass.c:727-752). Applies optional monitor caps. */
static BOOL amdgpu_add_mode(struct AmdgpuDisplayMode *list, ULONG *count, ULONG max_count,
                            ULONG w, ULONG h, ULONG r,
                            ULONG max_w, ULONG max_h, ULONG max_hz)
{
    ULONG i;

    if (w < 320 || h < 200 || r < 24)
        return FALSE;
    if (max_w && w > max_w) return FALSE;
    if (max_h && h > max_h) return FALSE;
    if (max_hz && r > max_hz) return FALSE;

    for (i = 0; i < *count; i++)
        if (list[i].w == w && list[i].h == h && list[i].refresh == r)
            return FALSE;
    if (*count >= max_count)
        return FALSE;

    dsp_memzero(&list[*count], sizeof(list[*count]));
    list[*count].w = (UWORD)w;
    list[*count].h = (UWORD)h;
    list[*count].refresh = (UWORD)r;
    amdgpu_calc_cvt_timing(&list[*count]);
    (*count)++;
    return TRUE;
}

/* Detailed timing descriptor parse (radeonsi_hiddclass.c:754-788). */
static BOOL amdgpu_parse_detailed(const UBYTE *d, ULONG *w, ULONG *h, ULONG *r)
{
    ULONG pixclk_khz, h_active, v_active, htotal, vtotal, refresh;

    if (d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x00)
        return FALSE;   /* not a timing descriptor */

    pixclk_khz = ((d[1] << 8) | d[0]) * 10;
    if (pixclk_khz == 0)
        return FALSE;

    h_active = ((d[4] & 0xF0) << 4) | d[2];
    v_active = ((d[7] & 0xF0) << 4) | d[5];
    htotal   = h_active + (((d[4] & 0x0F) << 8) | d[3]);
    vtotal   = v_active + (((d[7] & 0x0F) << 8) | d[6]);

    if (h_active == 0 || v_active == 0 || htotal == 0 || vtotal == 0)
        return FALSE;

    /* Round to the nearest Hz: 59.94 Hz timings are "60", 70.08 Hz "70";
       truncating made them 59 and 69. */
    refresh = (pixclk_khz * 1000UL + (htotal * vtotal) / 2) / (htotal * vtotal);
    if (refresh < 24 || refresh > 240)
        return FALSE;

    *w = h_active;
    *h = v_active;
    *r = refresh;
    return TRUE;
}

/* Harvest all EDID modes + base list into `list`. Port of
 * RadeonsiHarvestEDIDModes() (radeonsi_hiddclass.c:890-978) plus the base-mode
 * append (radeonsi_hiddclass.c:1076-1082). Returns count. */
ULONG amdgpu_display_parse_edid(const UBYTE *edid, struct AmdgpuDisplayMode *list,
                                ULONG max_count)
{
    ULONG count = 0, md, i, b;
    ULONG max_w = 0, max_h = 0, max_hz = 0;

    if (!edid || !list || max_count == 0)
        return 0;

    /* range descriptor (0xFD): V max Hz cap (radeonsi_hiddclass.c:898-905) */
    for (md = 0; md < 4; md++)
    {
        const UBYTE *d = &edid[54 + md * 18];
        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFD)
            if (d[6] >= d[5])
                max_hz = d[6];
    }

    /* detailed timings (radeonsi_hiddclass.c:908-921) */
    for (md = 0; md < 4; md++)
    {
        const UBYTE *d = &edid[54 + md * 18];
        ULONG w, h, r;
        if (!amdgpu_parse_detailed(d, &w, &h, &r))
            continue;
        if (w > max_w) max_w = w;
        if (h > max_h) max_h = h;
        amdgpu_add_mode(list, &count, max_count, w, h, r, 0, 0, 0);
    }

    /* standard timings, 8 entries at 38..53 (radeonsi_hiddclass.c:923-952) */
    for (i = 0; i < 8; i++)
    {
        const UBYTE *s = &edid[38 + i * 2];
        ULONG hh, vv, rr;

        if ((s[0] == 0x01 && s[1] == 0x01) || s[0] == 0x00)
            continue;
        hh = (s[0] + 31UL) * 8UL;
        switch ((s[1] >> 6) & 0x3) {
            case 0:  vv = (hh * 10UL) / 16UL; break;   /* 16:10 */
            case 1:  vv = (hh * 3UL)  / 4UL;  break;   /* 4:3   */
            case 2:  vv = (hh * 4UL)  / 5UL;  break;   /* 5:4   */
            default: vv = (hh * 9UL)  / 16UL; break;   /* 16:9  */
        }
        rr = (s[1] & 0x3F) + 60UL;
        if (hh && vv)
        {
            if (hh > max_w) max_w = hh;
            if (vv > max_h) max_h = vv;
            amdgpu_add_mode(list, &count, max_count, hh, vv, rr, 0, 0, 0);
        }
    }

    /* established timings (radeonsi_hiddclass.c:954-973) */
    {
        ULONG byte_idx, bit_idx, slot = 0;
        for (byte_idx = 0; byte_idx < 3; byte_idx++)
        {
            UBYTE bits = edid[35 + byte_idx];
            for (bit_idx = 0; bit_idx < 8; bit_idx++, slot++)
            {
                if (slot >= 24) break;
                if (!(bits & (0x80 >> bit_idx))) continue;
                if (amdgpu_established_modes[slot][0] == 0) continue;
                amdgpu_add_mode(list, &count, max_count,
                                amdgpu_established_modes[slot][0],
                                amdgpu_established_modes[slot][1],
                                amdgpu_established_modes[slot][2], 0, 0, 0);
            }
        }
    }

    /* curated base modes, clipped to monitor caps (radeonsi_hiddclass.c:1076) */
    for (b = 0; b < AMDGPU_NUM_BASE_MODES; b++)
        amdgpu_add_mode(list, &count, max_count,
                        amdgpu_base_modes[b][0], amdgpu_base_modes[b][1],
                        amdgpu_base_modes[b][2], max_w, max_h, max_hz);

    D(bug("[amdgpu-display] EDID parse: %lu modes (caps %lux%lu @<=%luHz)\n",
          (unsigned long)count, (unsigned long)max_w,
          (unsigned long)max_h, (unsigned long)max_hz));
    return count;
}

/* ------------------------------------------------------------------------ *
 * Stage 3 viability: read-only survey of the ATOM master COMMAND table. Which
 * modeset commands does this VBIOS carry? On RDNA4 some are stripped (like
 * ProcessI2c was). SetPixelClock/SetCRTC_Timing present -> ATOM-assisted modeset
 * is viable (port the atom_bios interpreter and execute the tables); absent ->
 * the pixel clock needs the SMU/DCCG-direct route. NO table is executed here —
 * this only inspects the command-table directory. Port of the survey at
 * radeonsi_display.c:1050-1120 (RadeonsiDumpAtomCommandEntry).
 * ------------------------------------------------------------------------ */
static void amdgpu_display_survey_atom_commands(struct AmdgpuDevice *ad)
{
    static const struct { ULONG idx; const char *name; } cmds[] = {
        { AMDGPU_ATOM_CMD_SET_PIXEL_CLOCK,        "SetPixelClock"       },
        { AMDGPU_ATOM_CMD_SET_DCE_CLOCK,          "SetDCEClock"         },
        { AMDGPU_ATOM_CMD_SET_CRTC_TIMING,        "SetCRTC_Timing"      },
        { AMDGPU_ATOM_CMD_ENABLE_CRTC,            "EnableCRTC"          },
        { AMDGPU_ATOM_CMD_BLANK_CRTC,             "BlankCRTC"           },
        { AMDGPU_ATOM_CMD_SELECT_CRTC_SOURCE,     "SelectCRTC_Source"   },
        { AMDGPU_ATOM_CMD_ENABLE_GRAPH_SURFACES,  "EnableGraphSurfaces" },
        { AMDGPU_ATOM_CMD_DIG1_ENCODER_CONTROL,   "DIG1EncoderControl"  },
        { AMDGPU_ATOM_CMD_DIG1_TRANSMITTER_CTRL,  "DIG1TransmitterCtrl" },
        { AMDGPU_ATOM_CMD_ENABLE_DISP_POWER_GATE, "EnableDispPowerGate" },
    };
    const UBYTE *rom;
    ULONG rom_size, rom_header, master_cmd, ncmds = sizeof(cmds) / sizeof(cmds[0]);
    ULONG i, present = 0, need = 0;

    if (!amdgpu_display_fetch_vbios(ad) || !ad->rom_base)
        return;
    rom = ad->rom_base;
    rom_size = ad->rom_size;
    rom_header = amdgpu_atom_u16(rom, AMDGPU_ATOM_ROM_HEADER_PTR);
    if (!amdgpu_atom_offset_valid(rom_header, AMDGPU_ATOM_ROM_MASTER_COMMAND_OFFSET + 2, rom_size))
        return;
    master_cmd = amdgpu_atom_u16(rom, rom_header + AMDGPU_ATOM_ROM_MASTER_COMMAND_OFFSET);
    if (!master_cmd || !amdgpu_atom_offset_valid(master_cmd, AMDGPU_ATOM_COMMON_HEADER_SIZE, rom_size))
    {
        bug("[amdgpu-display] ATOM command table: missing/invalid master offset 0x%lx\n",
            (unsigned long)master_cmd);
        return;
    }
    bug("[amdgpu-display] ATOM master command table @0x%lx size=%lu rev=%u.%u:\n",
        (unsigned long)master_cmd, (unsigned long)amdgpu_atom_u16(rom, master_cmd),
        (unsigned)rom[master_cmd + 2], (unsigned)rom[master_cmd + 3]);

    /* ASIC_Init (command index 0) — the one that matters for GFX bring-up, and
     * the one this survey never looked at because it only asked about MODESET
     * commands. JOB-0084: the GC memory-path resets stay held (RESET_CTRL 0x30,
     * want 0x7F) and the Linux register oracle puts the healthy release inside
     * amdgpu_device_asic_init(), which for GC >= 11.0.0 runs
     * amdgpu_atomfirmware_asic_init() -> amdgpu_atom_execute_table(ctx,
     * ATOM_CMD_INIT=0, ...). Linux REQUIRES it (parse_cmd_header failure returns
     * -EINVAL) and Linux drives this card, so it should be present here even
     * though every display command is stripped. Print the offset and the
     * command header's frev/crev — Linux needs frev==2 && crev>=1 to build the
     * v2.1 parameter block. Read-only; nothing is executed. */
    {
        ULONG e0  = master_cmd + AMDGPU_ATOM_COMMON_HEADER_SIZE;   /* index 0 */
        ULONG t0  = amdgpu_atom_offset_valid(e0, 2, rom_size)
                  ? amdgpu_atom_u16(rom, e0) : 0;
        if (t0 && amdgpu_atom_offset_valid(t0, AMDGPU_ATOM_COMMON_HEADER_SIZE, rom_size))
            bug("[amdgpu-display]   ATOM cmd ASIC_Init            [ 0]: PRESENT "
                "@0x%lx size=%lu frev=%u crev=%u %s\n",
                (unsigned long)t0, (unsigned long)amdgpu_atom_u16(rom, t0),
                (unsigned)rom[t0 + 2], (unsigned)rom[t0 + 3],
                (rom[t0 + 2] == 2 && rom[t0 + 3] >= 1)
                    ? "<-- Linux-compatible (frev=2 crev>=1)"
                    : "<-- UNEXPECTED rev; Linux would return -EINVAL");
        else
            bug("[amdgpu-display]   ATOM cmd ASIC_Init            [ 0]: MISSING "
                "(entry=0x%lx) <-- would mean Linux cannot asic_init this card either\n",
                (unsigned long)t0);
    }
    for (i = 0; i < ncmds; i++)
    {
        ULONG entry = master_cmd + AMDGPU_ATOM_COMMON_HEADER_SIZE + (cmds[i].idx * 2);
        ULONG table = amdgpu_atom_offset_valid(entry, 2, rom_size)
                    ? amdgpu_atom_u16(rom, entry) : 0;
        /* the 4 clock/timing/encoder commands are the ones a modeset truly needs */
        BOOL core = (cmds[i].idx == AMDGPU_ATOM_CMD_SET_PIXEL_CLOCK ||
                     cmds[i].idx == AMDGPU_ATOM_CMD_SET_CRTC_TIMING ||
                     cmds[i].idx == AMDGPU_ATOM_CMD_ENABLE_CRTC ||
                     cmds[i].idx == AMDGPU_ATOM_CMD_DIG1_ENCODER_CONTROL);
        if (core) need++;
        if (table && amdgpu_atom_offset_valid(table, 6, rom_size))
        {
            present++;
            bug("[amdgpu-display]   ATOM cmd %-20s [%lu]: PRESENT @0x%lx size=%lu rev=%u.%u ps=%u%s\n",
                cmds[i].name, (unsigned long)cmds[i].idx, (unsigned long)table,
                (unsigned long)amdgpu_atom_u16(rom, table),
                (unsigned)rom[table + 2], (unsigned)rom[table + 3], (unsigned)rom[table + 5],
                core ? "  <core>" : "");
        }
        else
        {
            bug("[amdgpu-display]   ATOM cmd %-20s [%lu]: MISSING%s\n",
                cmds[i].name, (unsigned long)cmds[i].idx, core ? "  <core!>" : "");
        }
    }
    bug("[amdgpu-display] ATOM modeset viability: %lu/%lu present -> %s\n",
        (unsigned long)present, (unsigned long)ncmds,
        (present >= (need + 2)) ? "ATOM-ASSISTED MODESET LIKELY VIABLE (port atom_bios interpreter)" :
        "ATOM tables stripped -> SMU/DCCG-direct clock route needed");
}

/* ------------------------------------------------------------------------ *
 * Top-level entry: probe every connected connector, read+parse EDID, and fill a
 * PURE-DATA mode list. Returns the mode count (0 = none) and, on success, the
 * winning connector's label. This file stays free of OOP attr-base tags (a plain
 * module source can't reach the class attrbases under -D__OOP_NOATTRBASES__);
 * the displayclass turns these AmdgpuDisplayMode entries into aHidd_Sync arrays
 * with its own attr bases + the "<ad->name> <connector> <WxH>" description.
 * NOTE: the DDC read path is gated OFF by default (AMDGPU_ENABLE_DCN_I2C_WRITES
 * = 0) because radeonsi found the RDNA4 VBIOS lacks the I2C transaction
 * processor — so this returns 0 until DC_I2C is verified on hardware (or an ATOM
 * display-object + DP-AUX path replaces it).
 * ------------------------------------------------------------------------ */
ULONG amdgpu_display_probe_modes(struct AmdgpuDevice *ad,
                                 struct AmdgpuDisplayMode *modes, ULONG max,
                                 char *label, ULONG label_size)
{
    UBYTE edid[128];
    ULONG conn, mode_count;
    BOOL connected;

    if (!ad || !ad->rmmio || !ad->ip.dmu.valid || !ad->dmu_seg[2])
    {
        D(bug("[amdgpu-display] probe: no DMU display block\n"));
        return 0;
    }

    /* Read-only DC_I2C viability probe (no writes — safe). radeonsi warned the DC
     * I2C block reads all-zero on RDNA4; log its state so the boot decides DC_I2C
     * vs an ATOM+AUX EDID route before the write gate is ever flipped. */
    {
        ULONG dcn = ad->dmu_seg[2];
        ULONG ctl = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_CONTROL);
        ULONG arb = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_ARBITRATION);
        ULONG sw  = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_SW_STATUS);
        ULONG hw0 = amdgpu_rreg32(ad, dcn + AMDGPU_DDC_HW_STATUS(0));
        bug("[amdgpu-display] DC_I2C viability @dmu_seg2=0x%05lx: CONTROL=0x%08lx ARB=0x%08lx SW=0x%08lx HW0=0x%08lx -> %s\n",
            (unsigned long)dcn, (unsigned long)ctl, (unsigned long)arb,
            (unsigned long)sw, (unsigned long)hw0,
            (ctl == 0xFFFFFFFFUL) ? "ALL-ONES (block absent)" :
            (ctl == 0 && arb == 0 && sw == 0) ? "ALL-ZERO (radeonsi RDNA4 hole -> ATOM+AUX route)" :
            "SANE (DC_I2C candidate -> flip AMDGPU_ENABLE_DCN_I2C_WRITES)");
    }

    /* Stage 3 viability: survey the ATOM command tables (read-only) — decides
     * ATOM-assisted modeset vs the SMU/DCCG-direct clock route on this VBIOS. */
    amdgpu_display_survey_atom_commands(ad);

    /* Parse the VBIOS ATOM display-object table so we have real connector ->
     * DDC i2c_id routing for the GPIO bit-bang path. If it succeeds we iterate
     * the ATOM connector routes; otherwise we fall back to the register-HPD-by-
     * index enumeration. */
    {
        BOOL atom_ok = amdgpu_display_parse_atom(ad);
        ULONG nconn = (atom_ok && ad->atom_connector_count)
                        ? ad->atom_connector_count : AMDGPU_DCN_MAX_CONNECTORS;

        for (conn = 0; conn < nconn; conn++)
        {
            if (!amdgpu_display_get_connector_info(ad, conn, label, label_size, &connected))
                continue;

            /* Prefer ATOM-derived connectedness (HPD sense on the routed pin, or
             * an I2C route as a weaker signal) when the ATOM parse worked. */
            if (atom_ok && conn < ad->atom_connector_count &&
                ad->atom_connectors[conn].valid)
            {
                connected = ad->atom_connectors[conn].has_hpd
                                ? amdgpu_atom_hpd_sensed(ad, conn)
                                : ad->atom_connectors[conn].has_i2c;
            }
            if (!connected)
                continue;

            if (!amdgpu_display_read_edid(ad, conn, edid, sizeof(edid)))
            {
                D(bug("[amdgpu-display] probe: connector %lu connected but no EDID\n",
                      (unsigned long)conn));
                continue;
            }

            dsp_memzero(modes, sizeof(struct AmdgpuDisplayMode) * max);
            mode_count = amdgpu_display_parse_edid(edid, modes, max);
            if (mode_count == 0)
                continue;

            D(bug("[amdgpu-display] probe: %lu modes from connector %lu (%s)\n",
                  (unsigned long)mode_count, (unsigned long)conn, label));
            return mode_count;
        }
    }

    D(bug("[amdgpu-display] probe: no connector yielded a usable EDID mode list\n"));
    return 0;
}
