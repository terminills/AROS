# amdgpu GFX engine bring-up (GFX12 / RDNA4 / Navi 44)

## Architecture: amdgpu wakes the whole engine at boot; radv does zero bring-up

The GFX (graphics/compute) engine is brought up **entirely inside amdgpu.hidd**, in
`amdgpu_gfx_hw_init` (the `gfx` IP block, run during `amdgpu_device_init` at boot).
By the time any 3D backend (radv / radeonsi) touches the card, the compute engine
is already alive. The radv winsys (`workbench/hidds/radeonsi/aros_radv_winsys.c`)
must do **no** engine bring-up — it only allocates a ring/HQD on the running
engine. Its current MEC/GFXHUB/HQD bring-up is redundant "old behavior" to be
stripped once amdgpu owns the sequence (same pattern as the mmio.library removal).

This is why PSP came first: the whole point of the PSP + GMC + display bring-up was
to get here — a powered, microcode-loaded GFX engine is what opens up 3D/compute
(radv → Zink GL, llama.cpp / Stable Diffusion Vulkan backends).

## The bring-up ladder

| Step | What | Status |
|------|------|--------|
| PSP  | MP0 bootloader, SOS/display firmware | done |
| GMC  | memory controller / VRAM aperture | done |
| DCN  | display scanout (modeset) | done |
| **IMU** | Intelligent Management Unit — powers/clocks the GFX block | **done (P0)** |
| **RLC autoload** | stage all CP/RLC/SDMA/MES ucode in a VRAM buffer; RLC bootstraps it | **P1 (next)** |
| GFXHUB VM | the GFX block's own VM/MC apertures | P2 |
| CP / MEC | command processor + compute microengine rings | P2 |

### P0 — IMU (done)
`amdgpu_gfx.c`. Translated from Linux `imu_v12_0.c` against on-disk
`asic_reg/gc/gc_12_0_0_offset.h`. Discovery captures `gc_seg[]` (IMU/CP_MEC/RLC
registers are BASE_IDX 1 = `gc_seg[1]`; GRBM is BASE_IDX 0). Loads the IMU
microcode, points `GFX_IMU_RLC_BOOTLOADER_ADDR` at a staged RLC_G, starts the IMU.
Result on the DUT: `GFX_IMU_GFX_RESET_CTRL` `0x00 → 0x30` — the IMU released
GRBM_RESETB (bit4) + DFLL_SRESETB (bit5). It is **alive**.

### P1 ROOT CAUSE (2026-08-13): the GFX block is WRITE-BLOCKED in the live-GOP state
Decisive diagnostic on the DUT: `SCRATCH_10 <- 0xDEADBEEF` reads back `0x0`
(WRITES BLOCKED); I-RAM readback `0xffffffff`; CORE_CTRL `0x9 -> 0x9` (clear of
bit0 didn't stick). So the GFX/IMU block is READ-ONLY — reads return latched
values (CORE_CTRL=0x9, RESET_CTRL=0x30) but writes are dropped. **Every write the
gfx block does (golden RAM, I-RAM, bootloader, CORE_CTRL) is a no-op, and 0x30 is
GOP leftover, not our doing.** The bring-up code (below) is correct; the block is
power/clock-gated or held in a reset amdgpu doesn't own, because we adopt the live
GOP display (no GPU reset) whereas Linux resets the ASIC before gfx.

**The fork (each substantial, dictates the whole next approach):**
- (a) **SMU (MP1) GFX power-up** — send the PPSMC message that ungates GFX. Our
  SMU block only probes the mailbox today; the SMU was "live but autonomous" and
  didn't ack earlier, so this needs real SMU/SMC bring-up.
- (b) **GFX-scoped reset** that spares DCN — put GFX in a clean amdgpu-owned reset
  without killing the scanout. Risky (may disturb display).
- (c) **PSP-managed GFX load** — let MP0/PSP power + load GFX firmware (PSP-load
  mode), the way it already loads SOS.

Until one of these makes the block writable, the IMU/RLC/CP path cannot proceed.

### RESOLVED (2026-08-13): PSP-load is refused from live-GOP; the lever is a MODE1 reset
Path (c) was tried to completion and is DECISIVELY refused by the PSP from the
adopt-GOP state. On the DUT with the full GFX12 fw set + TMR setup:
- `LOAD_TOC -> status=0x11, tmr_size=0` (TMR never sized) — and this is NOT a bug
  in our command: `gc_12_0_0_toc.bin` is valid (2048B, ucode_size=1792,
  array_off=256), the frame layout matches Linux `psp_prep_load_toc_cmd_buf`
  exactly, and the IMU loads (`LOAD_IP_FW IMU_I/IMU_D -> status=0`) prove the
  submit/response channel + VRAM staging both work.
- Consequently all 16 GFX fw loads `-> 0xffff0006` (no TMR to load into) and
  `AUTOLOAD_RLC -> 0xffff0007` (fw set incomplete). Write-test still BLOCKED,
  IMU never runs, `GFX_IMU_GFX_RESET_CTRL` stuck at 0x30.

So the PSP's whole GFX-load path assumes a freshly-reset ASIC — exactly the state
we skip by adopting GOP. **All three forks share one root cause: no ASIC reset.**
The reset lever for GC12 is NOT the PSP (psp_v14_0 has no mode1_reset) but the SMU
(MP1) **debug** mailbox — fire-and-forget, no SMU ack (why "SMU didn't ack" never
mattered). From Linux `smu_v14_0_2_mode1_reset` ->
`smu_cmn_send_debug_smc_msg(DEBUGSMC_MSG_Mode1Reset=2)` ->
`smu_msg_v1_send_debug_msg`: write MP1 `C2PMSG_53`(param)=0, `C2PMSG_75`(msg)=2,
`C2PMSG_54`(resp)=0 (mp_14_0_2 offsets 0x0075/0x008b/0x0076 == our 0x40+N scheme).
Then poll `RCC_CONFIG_MEMSIZE != 0xffffffff` for the ASIC to return; mode1
preserves PCI config so BARs + discovery bases survive (no re-map/re-discover).

### RESULT (task #42): reset WORKS + is DISPLAY-SAFE, but does NOT unblock GFX
`amdgpu_reset.c` = `amdgpu_mode1_reset()`, called at the end of `ip_common_init`
(after discovery, before gmc/psp/gfx/dcn), gated by `AMDGPU_MODE1_RESET_ON_INIT`.
DUT result (2026-08-13):
- `MODE1 complete — ASIC back, RCC_CONFIG_MEMSIZE=0x3fb0 (16304 MB)` — the reset
  fires and the ASIC returns; PSP re-inits from cold (SOS + KM ring). Works.
- **DISPLAY-SAFE** — the desktop survives the reset (verified live: a ScreenMode
  resolution change on a reset-on boot). It does NOT black the GOP scanout; the
  earlier "display goes dark, needs a re-modeset" fear was wrong.
- **BUT the GFX block is STILL write-blocked after the reset** — `WRITE TEST ...
  read=0x02e3 (WRITES BLOCKED)`, IMU still times out, `LOAD_TOC` still `0x11`,
  identical to the pre-reset boot. So a genuine ASIC reset changes nothing about
  the GFX block: the write-block is NOT GOP-leftover state.

### SUPERSEDED (SMU-power theory): discrete GFX12 is a PURE PSP-autoload path
Two deep source-dives (Linux) established the discrete Navi44 truth: firmware
load_type is AMDGPU_FW_LOAD_PSP; the driver NEVER pokes GC/IMU registers during
bring-up — the PSP powers + runs the IMU + RLC autoload internally at
AUTOLOAD_RLC, and the driver only waits for CP_STAT==0 / RLC bootload-complete.
`EnableGfxImu` is APU-only (smu_v14_0_2 doesn't map it), so the SMU is NOT the
discrete lever. Our `amdgpu_gfx.c` DIRECT-IMU sequence (GFX_IMU_* writes, the
==0x1f poll) is RLC_BACKDOOR_AUTO-only; its dropped writes are the EXPECTED
symptom of a PSP-gated block, not a bug to fix.

### PSP-path correction DONE; blocker narrowed to ONE command: LOAD_TOC 0x11
`amdgpu_psp.c` now matches the discrete PSP sequence: FB fw-reservation handshake
(FB_FW_RESERV_ADDR/EXT, required for SOS>=0x3b0e0d) → LOAD_TOC → SMC/PMFW first →
CP(PFP/ME/MEC)+MES → IMU → RLC sub-blobs → RLC_G LAST → AUTOLOAD_RLC. SETUP_TMR is
never sent (boot_time_tmr/psp_skip_tmr). Confirmed on the DUT: SMC(18)+IMU_I(68)+
IMU_D(69) load status=0; FB_FW_RESERV returns a VALID region (base=0x3f7e00000
size=0x3200000 = vram_size-size, the boot-time TMR IS armed).

**THE WALL: `LOAD_TOC (0x20) -> status=0x11, tmr_size=0`.** Because it fails, every
GFX autoload component rejects (RLC_G=0xffff0000, rest=0xffff0006 BAD_PARAMETERS)
and AUTOLOAD_RLC=0xffff0007 BAD_STATE. RULED OUT as the cause: TOC version (byte-
identical to linux-firmware), tight/unpadded staging (now 1 MiB-aligned + fully
zero-filled like psp_copy_fw), SETUP_TMR interference (removed), SMC-not-loaded
(loads), FB-reservation un-armed (query succeeds, region valid). The command frame
matches psp_prep_load_toc_cmd_buf exactly and the response read is proven correct
(FB_FW_RESERV reads valid uresp at the same +864/+928 offsets). So the PSP rejects
a byte-identical TOC with a correct frame while every other command on the ring
succeeds — an intrinsic 14.0.2 LOAD_TOC quirk. Static source-diffing exhausted;
next = decode PSP status 0x11 specifically (git history / TEE tables / a real
Linux PSP command trace to byte-diff).

### PSP-load progress (2026-08-13): channel works, AUTOLOAD_RLC is the power-up trigger
Implemented the PSP GPCOM ring command-submit (amdgpu_psp.c: psp_gpcom_submit +
psp_load_ip_fw + psp_load_gfx_firmware; wptr tracked locally, C2PMSG_102 reads 0).
Confirmed on the DUT:
- `LOAD_IP_FW IMU_I(68)` + `IMU_D(69)` → status=0 (OK). PSP command channel is real.
- `AUTOLOAD_RLC(0x21)` → status=**0xffff0007** — the PSP validated the command and
  REJECTED it because only the IMU is loaded. AUTOLOAD_RLC is the trigger that makes
  the PSP power+bring-up GFX; it needs the FULL fw set first (sibling of the winsys'
  documented 0xffff000d BAD_STATE for wrong-type loads).
- Loading firmware alone does NOT power the block: driver still can't clear CORE_CTRL
  bit0 (0x9->0x9), RLC/BOOTLOAD_STATUS=0, RESET_CTRL=0x30.

**NEXT (mechanical): extend psp_load_gfx_firmware to LOAD_IP_FW the whole GFX12 set
before AUTOLOAD_RLC** — RLC_G(8) [+ RLC sub-blobs RESTORE_LIST_GPM_MEM(20)/SRM_MEM(21)/
SRM_CNTL(22), RLC_IRAM(26), RLC_DRAM_BOOT(48)], RS64 PFP(87)/ME(88)/MEC(89) + their
P-stacks(94..), CP_MES(33)+MES_STACK(34). Parse each from its .bin (gfx_firmware_
header_v2_0: ucode@40/data@48; mes_v1_0; rlc v2.1/2.2 sub-fields already mapped in
amdgpu_gfx.c). Stage 4KB-aligned in VRAM, LOAD_IP_FW with the type, then AUTOLOAD_RLC
should return 0 and the PSP powers the block → RESET_CTRL 0x1f → radv compute.

### Chosen path (c): PSP-load — VIABLE, prerequisites already met
The DUT boot confirms **SOS is alive** (`*** SILICON AWAKE *** PSP sign-of-life`)
and the **PSP KM ring is open** (`*** PSP KM RING CREATED *** C2PMSG_64=0x80c20000`).
So the PSP can be told to load+power the GFX block: it has the privilege to write
the gated block that amdgpu can't. The amdgpu_psp block only does `ring_create`
today — implement the ring command-submit and post `GFX_CMD_ID_LOAD_IP_FW` for
`IMU_I`(68) + `IMU_D`(69) (then `RLC_G`(8), `RS64_PFP/ME/MEC`(87/88/89)). The PSP
loads the IMU into the block and powers it → block writable → IMU runs → reset
releases → radv compute. See task #40 for the full, ready-to-code protocol spec
(psp_gfx_rb_frame, cmd buffer layout, C2PMSG_67/102 wptr, fence poll — all from
Linux amdgpu_psp.c + psp_gfx_if.h + psp_v14_0.c).

### P1 status: implemented + correct, but the block is write-blocked (see above)
All gfx-block steps are implemented and verified against Linux + the register
headers, and confirmed correct on the DUT:
- **golden RLC RAM** (`program_rlc_ram`, 77 entries, fb_base=0x8000 top=0x83fb) ✓
- **RLC autoload buffer** (TOC-parsed, RLC_G + sub-components staged at their
  offsets, bootloader pointed at RLC_G) ✓
- **IMU microcode** load + setup + start ✓
- **wait** with a real ~100ms window ✓ (timing ruled out)

Yet `GFX_IMU_GFX_RESET_CTRL` stalls at **0x30** (GRBM_RESETB bit4 + DFLL_SRESETB
bit5 released; HARD/EA/UTCL2/SDMA held). The sequence matches
`gfx_v12_0_hw_init` exactly; nothing is missing. **Leading hypothesis:** the GFX
block is in a GOP/vBIOS-initialised state — we adopt the live display (NO GPU
reset, to preserve the scanout), whereas Linux does a full ASIC reset before IMU
bring-up. The IMU backdoor-autoload likely cannot cleanly re-init a
firmware-touched GFX block without a reset. Next investigations: (a) a GFX-only
soft reset (SRBM/GRBM) before the IMU that doesn't disturb DCN; (b) IMU
status/scratch (only regGFX_IMU_SCRATCH_10 is in the header) to see what it
waits on; (c) whether a mode2 reset can be scoped to GFX. This is the real
boundary — a clean-reset-vs-live-display tension unique to our adopt-GOP path.

### P1 — RLC autoload buffer (the master-reset release)
`GFX_IMU_GFX_RESET_CTRL` reaches the wanted `0x1f` (HARD/EA/UTCL2/SDMA/GRBM
released) only after the RLC finishes bootstrapping. The IMU bootloads only
RLC_G; the RLC then reads a **TOC** and pulls the rest from a full VRAM autoload
buffer. Implement in `amdgpu_gfx_hw_init`:

1. Load `gc_12_0_0_toc.bin`; parse `RLC_TABLE_OF_CONTENT_V2` entries
   (`offset = entry.offset * 8 * 4`; `size = entry.size_x16 ? size*1024*4 : size*4`)
   → per-`SOC24_FIRMWARE_ID` (offset,size).
2. Allocate the VRAM autoload buffer (TOC total, min 23 MiB).
3. Copy each ucode into the buffer at its TOC offset, each via its own v2 header:
   RLC_G, RLC_TOC, RS64 PFP (ucode + P0/P1 stacks), RS64 ME (+ P0/P1 stacks),
   RS64 MEC (+ P0–P3 stacks), MES (uni_mes), SDMA.
4. Point `GFX_IMU_RLC_BOOTLOADER_ADDR` at RLC_G *inside* the buffer.
5. IMU load/setup/start → RLC bootstraps → HARD_RESETB drops → `0x1f`.
   GFX registers (GRBM_STATUS etc.) then read live values.

Firmware is staged under `Devs/Firmware/amdgpu/gc_12_0_0_*.bin` and kept on the
rescue image (the RESCUE_SLIM strip now drops only the 893M nvidia GSP, keeps the
19M amdgpu set — `arch/x86_64-pc/boot/iso/mmakefile.src`).

### P2 — GFXHUB VM + CP/MEC rings
With the engine out of reset: program the GFXHUB VM apertures, bring the CP/MEC
online, init a compute ring. The winsys already has HQD/MQD code that will then
work against live registers — after which its bring-up is removed and it only
allocates rings.

## Test loop
`qemu-test/radv-cycle.sh`: build `hidd-radv-probe` → `all` → `distfiles` →
`rescue` → deploy+boot on the DUT. `amdgpu_gfx_hw_init` runs at boot (watch the
boot serial for `[amdgpu-gfx]`). `RadvProbe` (run manually on the DUT) exercises
the radv/winsys path once the engine is up.
