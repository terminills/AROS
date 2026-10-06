#ifndef AMDGPU_INTERN_H
#define AMDGPU_INTERN_H

/*
 * amdgpu.hidd — AMD base driver, clean bring-up.
 * See README.md and ABI-RFC.md; decision: ACD-amdgpu-driver-stack-split.
 *
 * Structure mirrors AMD's Linux kernel module (IP-block init ladder)
 * so generation enablement tracks first-party source. The radeonsi
 * monolith is consulted as engineering knowledge only.
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/semaphores.h>
#include <oop/oop.h>

/* GFX levels, covering every generation Linux amdgpu drives (SI onwards).
   The numeric values are part of the query API (AmdgpuGpuInfo.gfx_level),
   so new levels are appended rather than inserted in order. Tier-1
   validated hardware is GFX10.3 (V620) and GFX12 (RX 9060 XT). */
enum amdgpu_gfx_level {
    AMDGPU_GFX_UNKNOWN = 0,
    AMDGPU_GFX8,        /* Polaris — DCE 11.2, no PSP (delayed: cards untested) */
    AMDGPU_GFX9,        /* Vega */
    AMDGPU_GFX10_1,     /* RDNA1  Navi 1x */
    AMDGPU_GFX10_3,     /* RDNA2  Navi 2x — V620 */
    AMDGPU_GFX11,       /* RDNA3  Navi 3x */
    AMDGPU_GFX12,       /* RDNA4  Navi 4x — RX 9060 XT */
    AMDGPU_GFX6,        /* GCN1   Southern Islands */
    AMDGPU_GFX7,        /* GCN2   Sea Islands */
};

struct AmdgpuPciId {
    UWORD deviceid;
    UBYTE gfx_level;    /* enum amdgpu_gfx_level */
    UBYTE tested;       /* lab-validated on real silicon */
    const char *name;
};

/* One IP block's register base, resolved from the discovery table.
   `base` is a dword register offset into the MMIO aperture. */
struct AmdgpuIpBase {
    ULONG   base;
    UBYTE   major;
    UBYTE   minor;
    UBYTE   revision;
    UBYTE   instance;
    BOOL    valid;
};

/* The IP blocks this driver resolves from discovery. Named after the
   kernel's HWIP blocks; grows as tiers implement more of them. */
struct AmdgpuIpMap {
    struct AmdgpuIpBase gc;       /* graphics/compute */
    struct AmdgpuIpBase nbio;     /* bus interface (VRAM memsize reg) */
    struct AmdgpuIpBase mmhub;    /* memory hub */
    struct AmdgpuIpBase athub;
    struct AmdgpuIpBase osssys;   /* interrupt handler */
    struct AmdgpuIpBase psp;      /* MP0 */
    struct AmdgpuIpBase smu;      /* MP1 */
    struct AmdgpuIpBase sdma0;
    struct AmdgpuIpBase dmu;      /* display */
    struct AmdgpuIpBase df;       /* data fabric */
    struct AmdgpuIpBase vcn;      /* video codec */
    struct AmdgpuIpBase thm;      /* thermal (BACO control) */
};

/* One ATOM display-object connector route, populated from the VBIOS
   displayobjectinfo table + its I2C/HPD object records. Faithful subset of
   radeonsi's carddata->atom_connectors[] (radeonsi_intern.h:355-369). Used to
   resolve connector -> DDC i2c_id for the GPIO bit-banged EDID path. */
struct AmdgpuAtomConnector {
    BOOL  valid;
    UWORD display_objid;
    UWORD encoder_objid;
    BOOL  has_i2c;
    UBYTE i2c_id;
    UBYTE i2c_line;
    UBYTE i2c_engine_id;
    UBYTE i2c_slave_addr;
    BOOL  has_hpd;
    UBYTE hpd_pin;
    UBYTE hpd_state;
};

/* One discovered GPU. Tier 1: identification + register aperture +
   VRAM size + IP discovery. Later tiers hang their state off this
   struct — it is the AROS analog of the kernel's struct amdgpu_device. */
struct AmdgpuDevice {
    struct MinNode      node;
    OOP_Object         *pciDevice;
    OOP_Object         *pciDriver;

    UWORD               vendorid;
    UWORD               deviceid;
    UBYTE               revision;
    UBYTE               gfx_level;
    UBYTE               tested;
    const char         *name;
    UBYTE               chip;           /* enum amdgpu_chip, or IP_DISCOVERY */
    UBYTE               chip_flags;     /* AMDGPU_PCIID_* */
    const char         *family;         /* Linux family: SI, CIK, VI, AI, NV, discovery */
    char                namebuf[48];

    /* register aperture (the small MMIO BAR) */
    IPTR                rmmio_phys;
    IPTR                rmmio_size;
    volatile ULONG     *rmmio;

    /* Serializes the shared BIF MM_INDEX/MM_INDEX_HI/MM_DATA aperture. Every
     * indirect VRAM access (amdgpu_vram_read/write) and every ATOM indexed
     * register access retargets MM_INDEX and then touches MM_DATA; that pair is
     * stateful, so two of them on different CPUs interleave and clobber each
     * other's index. Observed failure: an SDMA packet-stage vram_write racing a
     * fence-poll vram_read leaves the ring with RPTR stuck at 0x04 and RB_ENABLE
     * auto-cleared. Held across each WHOLE indexed transaction, never per-dword.
     * Init'd in amdgpu_mmio_map before any aperture access. (codex TRIG-0686.) */
    struct SignalSemaphore aperture_sem;

    /* large prefetchable BAR = VRAM aperture (mapped later tiers) */
    IPTR                vram_bar_phys;
    IPTR                vram_bar_size;

    /* doorbell BAR (RDNA; used from the CS tier onward) */
    IPTR                doorbell_phys;
    IPTR                doorbell_size;
    volatile ULONG     *doorbell;      /* mapped BAR2 doorbell aperture (MES submit) */

    ULONG               vram_mb;        /* 0 = not yet readable */

    /* IP discovery table (fetched from top of VRAM) */
    UBYTE              *disc_bin;
    ULONG               disc_size;
    struct AmdgpuIpMap  ip;

    /* DMU (display) register-base segments — the DCN scanout registers use
       BASE_IDX 2 (dmu_seg[2]); captured whole because record_base keeps only
       base0. Populated by the discovery parse for hw_id == AMDGPU_DMU_HWID. */
    ULONG               dmu_seg[6];

    /* GC (graphics/compute) register-base segments. GFX12 registers carry a
       BASE_IDX (0 or 1): GRBM/GFXHUB are BASE_IDX 0, the IMU/CP_MEC/RLC control
       registers are BASE_IDX 1. record_base keeps only base0, so the GFX IP
       block captures the whole segment map here (hw_id == AMDGPU_GC_HWID). */
    ULONG               gc_seg[6];
    ULONG               vcn_seg[6];
    UQUAD               vcn_fw_tmr_mc;
    ULONG               vcn_fw_size;
    ULONG               vcn_wptr;
    BOOL                vcn_ready;
    ULONG               mmhub_seg[6];  /* MMHUB segment map — MES SET_HW_RESOURCES */
    ULONG               osssys_seg[6]; /* OSSSYS segment map — MES SET_HW_RESOURCES */

    /* NBIO register-base segments. Navi44's Linux NBIO initialization uses
       BASE_IDX 5, whose registers sit outside the direct MMIO BAR and must be
       reached through the PCIe index/data aperture. */
    ULONG               nbio_seg[6];

    /* VBIOS ROM image, copied out of the PCI expansion-ROM BAR by
       amdgpu_display_fetch_vbios() (amdgpu_display.c). Mirrors radeonsi's
       carddata->rom_base/rom_size/rom_allocated. rom_base is a plain host copy;
       rom_size is its byte length; rom_allocated == it was AllocVec'd here. */
    UBYTE              *rom_base;
    ULONG               rom_size;
    BOOL                rom_allocated;

    /* ATOM display-object connector routing, parsed once from the VBIOS by
       amdgpu_display_parse_atom() (amdgpu_display.c). */
    BOOL                        atom_display_valid;
    ULONG                       atom_connector_count;
    struct AmdgpuAtomConnector  atom_connectors[8];

    /* amdgpu_gart.c — PCIE GART (VMID0) so the PSP autoload fw set is staged in
       system RAM and addressed by GART VA (not raw VRAM MC). See amdgpu_gart.c. */
    APTR                gart_arena_raw;   /* unaligned AllocMem base (cleanup) */
    UBYTE              *gart_arena;       /* 4KB-aligned system-RAM fw staging arena */
    IPTR                gart_arena_phys;  /* its bus/physical base (page-aligned) */
    ULONG               gart_arena_size;
    UQUAD               gart_fw_va;       /* GART VA the arena is mapped at */
    UQUAD               gart_table_mc;    /* MC addr of the page table in VRAM */
    BOOL                gart_ready;
    /* Bump allocator over the GART pages ABOVE the fw arena. amdgpu_gart_map()
       hands out VAs for arbitrary system-RAM buffers (e.g. an AROS BitMap) so
       the GPU can blit to and from them. Bump-only: VA is not reclaimed. */
    ULONG               gart_next_page;
};

/* IP-block init ladder, mirroring the kernel's amdgpu_ip_block. Each
   tier fills in more blocks; unimplemented blocks log and pass. */
struct AmdgpuIpBlock {
    const char *name;
    BOOL (*hw_init)(struct AmdgpuDevice *ad);   /* NULL = not yet implemented */
    /* NULL = runs on every GPU. Otherwise FALSE when this GPU's version of
       the block has no implementation: the gate logs why, and the ladder
       skips the block without touching its registers. */
    BOOL (*supported)(struct AmdgpuDevice *ad);
};

struct AmdgpuBase {
    struct MinList      devices;
    ULONG               device_count;
    OOP_AttrBase        pciDeviceAB;
    OOP_AttrBase        pciDriverAB;
};

extern struct AmdgpuBase amdgpuBase;

/* amdgpu_pci.c */
BOOL amdgpu_pci_probe(struct AmdgpuBase *base);

/* amdgpu_mmio.c */
BOOL amdgpu_mmio_map(struct AmdgpuBase *base, struct AmdgpuDevice *ad);
ULONG amdgpu_rreg32(struct AmdgpuDevice *ad, ULONG dword_offset);
void amdgpu_wreg32(struct AmdgpuDevice *ad, ULONG dword_offset, ULONG val);
ULONG amdgpu_get_vram_mb(struct AmdgpuDevice *ad);

/* amdgpu_discovery.c */
BOOL amdgpu_discovery_init(struct AmdgpuDevice *ad, ULONG vram_mb);
/* Indirect VRAM access via the BIF MM aperture (MM_INDEX/MM_DATA). pos is a
 * 64-bit VRAM BYTE offset; buf holds `dwords` 32-bit words. */
void amdgpu_vram_read(struct AmdgpuDevice *ad, UQUAD pos, ULONG *buf, ULONG dwords);
void amdgpu_vram_write(struct AmdgpuDevice *ad, UQUAD pos, const ULONG *buf, ULONG dwords);

/* amdgpu_device.c */
const struct AmdgpuPciId *amdgpu_identify(UWORD vendorid, UWORD deviceid);

/* amdgpu_nbio.c — bus-interface register initialization */
BOOL amdgpu_nbio_hw_init(struct AmdgpuDevice *ad);

/* amdgpu_firmware.c — GPU microcode loader (foundation owns firmware) */
BOOL amdgpu_request_firmware(const char *name, UBYTE **data, ULONG *size);

/* The IP blocks that carry microcode, for amdgpu_fw_name(). */
enum AmdgpuFwIp {
    AMDGPU_FW_GC,       /* suffix picks the engine: "_pfp", "_me", "_rlc", ... */
    AMDGPU_FW_PSP,      /* "_sos", "_ta", "_toc" */
    AMDGPU_FW_SMU,      /* no suffix */
    AMDGPU_FW_SDMA,     /* no suffix */
    AMDGPU_FW_VCN       /* no suffix */
};

/* Name the microcode file for one IP block of this GPU from the versions IP
   discovery reported, e.g. "gc_12_0_0_pfp.bin" or "sienna_cichlid_pfp.bin".
   FALSE if the block was not discovered or buf is too small. */
BOOL amdgpu_fw_name(struct AmdgpuDevice *ad, enum AmdgpuFwIp ip,
                    const char *suffix, char *buf, ULONG len);
/* amdgpu_fw_name() + amdgpu_request_firmware(). */
BOOL amdgpu_request_ip_firmware(struct AmdgpuDevice *ad, enum AmdgpuFwIp ip,
                                const char *suffix, UBYTE **data, ULONG *size);
VOID amdgpu_release_firmware(UBYTE *data);

/* amdgpu_psp.c — Platform Security Processor (MP0): the firmware gateway */
BOOL amdgpu_psp_hw_init(struct AmdgpuDevice *ad);
/* LOAD_IP_FW of an image already at fw_mc (VRAM MC or GART VA). TRUE when the
   PSP completed the command; *status is its response status (0 = accepted). */
BOOL amdgpu_psp_load_ip_fw(struct AmdgpuDevice *ad, UQUAD fw_mc, ULONG fw_size,
                           ULONG fw_type, const char *name, ULONG *status);

/* amdgpu_smu.c — System Management Unit (MP1): display/pixel clock control */
BOOL amdgpu_smu_hw_init(struct AmdgpuDevice *ad);
ULONG amdgpu_smu_base(struct AmdgpuDevice *ad);   /* resolved MP1 base (MP0+0x200) */
/* Raw MP1 mailbox send, exported so the standalone probes (C:GfxResetProbe) can
 * drive ONE SMU message per run over the RNDIS bench instead of baking each
 * theory into the boot path — a wedged mailbox then costs a reboot, not a
 * bricked init. Returns the raw C2PMSG_90 status (SMU_RESP_OK==1, 0 = no
 * answer); *result gets C2PMSG_82 (the message's return VALUE, not status). */
ULONG amdgpu_smu_send_msg(struct AmdgpuDevice *ad, ULONG msg, ULONG arg,
                          ULONG *result);
/* Replay the whole Linux smu_smc_hw_setup() prefix + EnableAllSmuFeatures from a
 * probe. The boot path's feature-enable is parked, so a booted card has no
 * driver/tools DRAM address registered — which Linux treats as a hard
 * prerequisite — and a bare enable would not be the same experiment.
 * pwr_domain is FEATURE_PWR_DOMAIN_e: 0=ALL (smu_smc_hw_setup's value),
 * 4=GFX (smu_v14_0_2_enable_gfx_features' value — powers just the GFX block). */
void amdgpu_smu_full_setup(struct AmdgpuDevice *ad, ULONG pwr_domain);
/* One SMU metrics sample (SMU14 SmuMetrics_t, TABLE_SMU_METRICS=5), decoded.
 * Clocks in MHz, power in W, temperatures in degrees C, activity in percent. */
struct AmdgpuSmuMetrics {
    ULONG gfxclk_cur, socclk_cur, uclk_cur, fclk_cur;
    UWORD gfxclk_avg, uclk_avg, fclk_avg;
    UWORD gfx_activity, uclk_activity;
    UWORD socket_power, board_power;
    UWORD temp_edge, temp_hotspot, temp_mem;
    UBYTE fan_pwm;
    UWORD fan_rpm;
    ULONG counter;
};
/* Read one metrics sample through the driver DRAM table. Serialized with every
 * other SMU message. Returns FALSE if the SMU did not deliver the table. */
BOOL amdgpu_smu_read_metrics(struct AmdgpuDevice *ad, struct AmdgpuSmuMetrics *m);
BOOL amdgpu_sdma_hw_init(struct AmdgpuDevice *ad);

/* amdgpu_vcn.c */
BOOL amdgpu_vcn_hw_init(struct AmdgpuDevice *ad);
/* GFX clock gating (Linux gfx_v12_0_update_gfx_clock_gating). */
BOOL amdgpu_gfx_set_clock_gating(struct AmdgpuDevice *ad, BOOL enable);
/* TRUE if AMDGPU=<...word...> is on the kernel command line. */
BOOL amdgpu_boot_option(const char *word);
/* amdgpu_cursor.c — DCN hardware cursor (composited in the scanout path, no blit) */
/*
 * Bytes mapped PAST the visible framebuffer in the VRAM aperture, for surfaces
 * that must be CPU-writable but are not scanned out -- currently the hardware
 * cursor. MUST be honoured by whoever sizes the MapPCI() in the VRAM handoff
 * AND by every consumer that places a surface past the fb, or the surface lands
 * outside the mapping and its writes go nowhere. Measured 2026-08-19: with no
 * reserve the 64x64 cursor sat 45056 bytes past a 0xFE9000-byte mapping, every
 * register read back correct, and nothing appeared on screen.
 */
#define AMDGPU_FB_AUX_RESERVE  0x100000UL
/* Existing displayable-bitmap arena. The map-driven BO allocator excludes
 * this whole region instead of sharing it or imposing a half-pool quota. */
#define AMDGPU_VRAM_POOL_SIZE  (64UL << 20)

BOOL amdgpu_cursor_init(struct AmdgpuDevice *ad, APTR fb_cpu, UQUAD fb_mc, ULONG fbsize,
                        ULONG scr_w, ULONG scr_h);
void amdgpu_cursor_set_pos(struct AmdgpuDevice *ad, LONG x, LONG y);
void amdgpu_cursor_set_visible(struct AmdgpuDevice *ad, BOOL visible);
void amdgpu_cursor_set_shape(struct AmdgpuDevice *ad, const ULONG *argb,
                             ULONG w, ULONG h, WORD hot_x, WORD hot_y);
/* SDMA damage-rectangle blit into the scanout; FALSE => caller must use the CPU
   path. The framebuffer BAR is uncached, so the CPU path runs at ~5 MB/s. */
/* Constant-fill a rect. dst_mc may be a VRAM MC address or a GART VA, so this
 * serves both framebuffer fills and (GART-mapped) bitmap fills. */
/* Rectangular VRAM->VRAM blit (both surfaces MC-addressable). Per-scanline
 * COPY_LINEAR. 120 rows leave two usable 64-byte fetch units beyond the
 * resident ring's mandatory unused fetch after both completion fences. */
#define AMDGPU_SDMA_COPY_RECT_MAX_ROWS 120
BOOL amdgpu_sdma_copy_rect_vram(struct AmdgpuDevice *ad,
                                UQUAD src_mc, ULONG src_stride, ULONG src_off,
                                UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                                ULONG row_bytes, ULONG rows);
/* One-packet 32-bit rectangle copy. Enabled only after the boot-time SUB_WIND
 * content oracle passes; FALSE lets callers retain the scanline fallback. */
BOOL amdgpu_sdma_copy_rect_vram32(struct AmdgpuDevice *ad,
                                  UQUAD src_mc, ULONG src_stride, ULONG src_off,
                                  UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                                  ULONG row_bytes, ULONG rows);
BOOL amdgpu_sdma_fill_rect(struct AmdgpuDevice *ad, UQUAD dst_mc, ULONG dst_stride,
                           ULONG dst_off, ULONG row_bytes, ULONG rows, ULONG value);

BOOL amdgpu_sdma_blit_rect(struct AmdgpuDevice *ad, UQUAD *src_va_cache,
                           ULONG *src_size_cache, APTR src_cpu, ULONG src_total,
                           ULONG src_stride, ULONG src_off,
                           UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                           ULONG row_bytes, ULONG rows);

/* Image DMA through a persistent GART scratch (PutImage/GetImage fast path).
   The caller buffer is transient (cannot own a cached GART VA) and gart_map is a
   bump allocator, so every transfer stages through one map-once scratch buffer.
   The per-row transform is applied CPU-side to/from the cached scratch, so both
   the raw-copy (Native) and the ARGB32<->native byteswap paths accelerate: even
   the byteswap beats writing the swapped bytes straight into the uncached VRAM
   BAR. <=64 rows per SDMA submission handled internally. Return FALSE for any
   unmet precondition -> caller uses the CPU. */
#define AMDGPU_XFER_RAW          0   /* plain copy (Native format) */
#define AMDGPU_XFER_BSWAP        1   /* upload: byteswap each pixel (ARGB32 -> native) */
#define AMDGPU_XFER_BSWAP_ALPHA  2   /* download: byteswap + force opaque alpha (native -> ARGB32) */

BOOL amdgpu_sdma_upload_rect(struct AmdgpuDevice *ad,
                             const APTR src_cpu, ULONG src_stride,
                             UQUAD dst_mc, ULONG dst_stride,
                             ULONG row_bytes, ULONG rows, ULONG xform);
BOOL amdgpu_sdma_download_rect(struct AmdgpuDevice *ad,
                               UQUAD src_mc, ULONG src_stride,
                               APTR dst_cpu, ULONG dst_stride,
                               ULONG row_bytes, ULONG rows, ULONG xform);

/* DMA engine-path evidence (codex TRIG-0537): running attempt/success counts so
   a test can prove the DMA path actually executed rather than silently CPU-fell
   back. Counts are cumulative since boot. */
void amdgpu_sdma_dma_counts(ULONG *up_attempt, ULONG *up_ok,
                            ULONG *dn_attempt, ULONG *dn_ok);

/* amdgpu_atom.c — run the VBIOS ATOM ASIC_Init command table (Linux
 * amdgpu_device_asic_init -> amdgpu_atomfirmware_asic_init). Programs clocks +
 * the memory controller; the step that walks GFX_IMU_GFX_RESET_CTRL 0x30 -> 0x7F
 * on a healthy card. Uses the self-contained interpreter in
 * atom_bios.c. */
BOOL amdgpu_atom_asic_init(struct AmdgpuDevice *ad);

/* amdgpu_reset.c — MODE1 ASIC reset (SOC24/Navi44) via the SMU debug mailbox.
 * PROVEN on the DUT: resets the ASIC + re-inits the PSP from cold, and is
 * DISPLAY-SAFE — the desktop survives the reset (verified live: a working
 * ScreenMode resolution change AFTER a reset-on boot; it does NOT black the GOP
 * scanout, contrary to the initial fear). It does NOT by itself unblock the
 * write-blocked GFX block — that is GFX-power gating (needs the SMU GFX power-up
 * sequenced BEFORE the GFX fw load), not GOP-leftover state (see GFX_BRINGUP.md).
 * Left ON as the Linux-correct first init step + the foundation for SMU power-up. */
#ifndef AMDGPU_MODE1_RESET_ON_INIT
#define AMDGPU_MODE1_RESET_ON_INIT 0
#endif
BOOL amdgpu_mode1_reset(struct AmdgpuDevice *ad);
BOOL amdgpu_baco_reset(struct AmdgpuDevice *ad);
/* PCI power cycle D0 -> D3hot -> D0 before init, mirroring what Linux gets for
 * free across rmmod/modprobe. See the rationale at the call site. */
BOOL amdgpu_pci_power_cycle(struct AmdgpuDevice *ad);

/* amdgpu_dcn.c — display controller observe (live scanout / framebuffer) */
BOOL amdgpu_dcn_hw_init(struct AmdgpuDevice *ad);
BOOL amdgpu_gfx_hw_init(struct AmdgpuDevice *ad);
BOOL gfx_v12_cp_gfx_ring_bringup(struct AmdgpuDevice *ad);  /* direct CP ring test (rail already powered) */
/* MEC compute-queue dispatch primitive (the reusable cs_submit foundation).
 * gfx_v12_compute_queue_ensure() one-time-inits the 64KiB GART queue buffer;
 * gfx_v12_compute_submit() installs the HQD with `ndw` PM4 dwords present and
 * kicks it (proven doorbell-less MMIO install), returning the new wptr (dwords)
 * or 0 on failure. fence_va/fence_cpu expose the queue's fence scratch so a
 * caller can target a WRITE_DATA fence and poll it. */
BOOL gfx_v12_compute_queue_ensure(struct AmdgpuDevice *ad);
ULONG gfx_v12_compute_submit(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw);
UQUAD gfx_v12_compute_fence_va(void);
volatile ULONG *gfx_v12_compute_fence_cpu(void);
/* GFX/IMU state snapshot, callable from the IP ladder so the post-PSP
 * window (where the IMU finishes booting) is observable. */
void amdgpu_gfx_state_snapshot(struct AmdgpuDevice *ad, const char *tag);
BOOL amdgpu_dcn_get_scanout(struct AmdgpuDevice *ad, UQUAD *mc, ULONG *w, ULONG *h, BOOL *master);
void amdgpu_dcn_set_scanout(struct AmdgpuDevice *ad, UQUAD mc);
/* Reprogram the scanout pixel format (depth). Format only, not timing. */
BOOL amdgpu_dcn_set_scanout_format(struct AmdgpuDevice *ad, ULONG bytesperpixel);

/* amdgpu_display.c — DDC/EDID mode probe (DRAFT, ported from radeonsi).
   Number of DCN connectors amdgpu HPD-senses (amdgpu_dcn.c DCN_MAX_CONNECTORS). */
#define AMDGPU_DCN_MAX_CONNECTORS   6
#define AMDGPU_DISPLAY_MAX_MODES    32

/* One probed display mode: geometry + CVT-derived timing. Mirrors the subset of
   radeonsi's struct RadeonDisplayMode the sync-tag builder needs. */
struct AmdgpuDisplayMode {
    UWORD w, h;
    UWORD refresh;        /* Hz */
    UWORD flags;
    ULONG pixelclock;     /* kHz */
    UWORD hsync_start, hsync_end, htotal;
    UWORD vsync_start, vsync_end, vtotal;
};

/* Pure-data API: this file probes connectors/EDID into AmdgpuDisplayMode[]; the
   displayclass (which owns the OOP attr bases) turns them into aHidd_Sync tags. */
void amdgpu_display_connector_name(struct AmdgpuDevice *ad, ULONG idx, char *label, ULONG label_size);
BOOL amdgpu_display_connector_connected(struct AmdgpuDevice *ad, ULONG idx);
BOOL amdgpu_display_get_connector_info(struct AmdgpuDevice *ad, ULONG idx, char *label, ULONG label_size, BOOL *connected);
BOOL amdgpu_display_read_edid(struct AmdgpuDevice *ad, ULONG connector, UBYTE *edid, ULONG size);
/* GPIO bit-banged DDC EDID read (preferred path — the DC_I2C engine reads dead
   on RDNA4). Resolves connector -> i2c_id -> DDC GPIO register offsets from the
   VBIOS ATOM GPIO_Pin_LUT, then bit-bangs the addr-0x50 EDID transaction. */
BOOL amdgpu_display_read_edid_gpio(struct AmdgpuDevice *ad, ULONG connector, UBYTE *edid, ULONG size);
/* Copy the VBIOS ROM image out of the PCI expansion-ROM BAR into ad->rom_base
   (lazy; TRUE if a valid 0x55AA image is available). */
BOOL amdgpu_display_fetch_vbios(struct AmdgpuDevice *ad);
/* Parse the ATOM displayobjectinfo table into ad->atom_connectors[] (lazy). */
BOOL amdgpu_display_parse_atom(struct AmdgpuDevice *ad);
/* Locate the VBIOS ATOM powerplayinfo table; return a host pointer to the pmfw
   PPTable_t (into ad->rom_base) + its byte size, or NULL. Used by the SMU
   feature-enable prerequisite (transfer the pptable to the SMC). */
const UBYTE *amdgpu_atom_get_pptable(struct AmdgpuDevice *ad, ULONG *size_out);
ULONG amdgpu_display_parse_edid(const UBYTE *edid, struct AmdgpuDisplayMode *list, ULONG max_count);
ULONG amdgpu_display_probe_modes(struct AmdgpuDevice *ad, struct AmdgpuDisplayMode *modes, ULONG max, char *label, ULONG label_size);

/* amdgpu_modeset.c — Stage 3 DCN-direct modeset (OTG/HUBP timing). Increment 1
   is read-only: decode + log the live OTG timing the firmware programmed. */
BOOL amdgpu_modeset_read_timing(struct AmdgpuDevice *ad, ULONG otg_inst,
                                struct AmdgpuDisplayMode *out);
void amdgpu_modeset_survey(struct AmdgpuDevice *ad);
/* Place the pipe's output rect on the raster. With set_viewport this makes a
 * smaller-than-native screenmode work (letterboxed) without touching timing. */
/* Bracket HUBP/DPP plane updates so DCN latches them atomically on VUPDATE.
   Updating a live pipe unlocked underflows it -- a 1920x1080 -> 800x600 change
   reset the machine. lock() returns FALSE if the lock could not be taken (or
   the device stopped responding); unlock() is safe to call regardless. */
/* READ-ONLY survey of the DCCG pixel-clock DTO; derives refclk from live state
   and cross-checks the formula against the raster the panel is displaying. */
void amdgpu_modeset_survey_dto(struct AmdgpuDevice *ad);
BOOL amdgpu_modeset_lock(struct AmdgpuDevice *ad, ULONG otg_inst);
void amdgpu_modeset_unlock(struct AmdgpuDevice *ad, ULONG otg_inst);
BOOL amdgpu_modeset_set_recout(struct AmdgpuDevice *ad, ULONG dscl_inst,
                               ULONG x, ULONG y, ULONG width, ULONG height);
/* Increment 2: program OTG timing for one instance from a probed mode.
   INERT -- nothing calls this yet. Retiming a live OTG blanks the display, and
   the matching DCCG DTO pixel-clock programming does not exist yet; without it
   the panel would be handed correct timing at the wrong rate. */
BOOL amdgpu_modeset_program_timing(struct AmdgpuDevice *ad, ULONG otg_inst,
                                   const struct AmdgpuDisplayMode *mode);

BOOL amdgpu_modeset_set_viewport(struct AmdgpuDevice *ad, ULONG hubp_inst,
                                 ULONG width, ULONG height, ULONG pitch_pixels);

/* amdgpu_gmc.c — graphics memory controller observe (aperture map) */
BOOL amdgpu_gmc_hw_init(struct AmdgpuDevice *ad);

/* amdgpu_gart.c — PCIE GART (VMID0) enable: system-RAM fw staging via GART VAs.
 * Sets ad->gart_ready; the PSP GFX-fw path then stages into ad->gart_arena and
 * hands the PSP ad->gart_fw_va + offset instead of a raw VRAM MC address. */
BOOL amdgpu_gart_enable(struct AmdgpuDevice *ad, UQUAD vram_start);
void amdgpu_gart_dump_fault(struct AmdgpuDevice *ad, const char *tag);
/* Program GFXHUB VMID0 — only valid AFTER the GFX rail is powered. SDMA and the
   GFX rings translate through GFXHUB, not MMHUB. */
void amdgpu_gart_enable_gfxhub(struct AmdgpuDevice *ad);
/* Map an arbitrary system-RAM buffer into the GART and return its GPU VA
   (0 on failure). The buffer need not be page-aligned; the returned VA carries
   the same in-page offset. amdgpu_gart_unmap() invalidates the PTEs again. */
UQUAD amdgpu_gart_map(struct AmdgpuDevice *ad, APTR buf, ULONG size);
void  amdgpu_gart_unmap(struct AmdgpuDevice *ad, UQUAD va, ULONG size);

/* --- Device/Query tier (amdgpu_query.c) ---
   The public ABI (opaque device handle, AmdgpuGpuInfo, AmdgpuIpType and the
   query prototypes) lives in the exported header so Mesa's ac_drm seam can
   consume it without seeing driver internals. Included AFTER struct
   AmdgpuDevice is defined above, which completes the type for our own use. */
#include <hidd/amdgpu.h>
BOOL amdgpu_device_init(struct AmdgpuBase *base, struct AmdgpuDevice *ad);
const char *amdgpu_gfx_level_name(UBYTE level);
/* Fill chip, chip_flags, family and (for listed chips) gfx_level from the
   PCI device ID, as Linux amdgpu's pciidlist does. */
void amdgpu_identify_chip(struct AmdgpuDevice *ad);

#endif /* AMDGPU_INTERN_H */
