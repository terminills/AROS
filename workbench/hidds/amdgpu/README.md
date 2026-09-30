# amdgpu.hidd — the AMD base driver (clean bring-up)

<!-- Created under a scoped, terminills-minted guard override (OVR-0002):
     new-directory build/doc files only, no core changes. -->

Per `ACD-amdgpu-driver-stack-split`: this is the base driver and sole
device owner of the AMD GPU stack — bring-up, firmware, memory,
command submission, power, display — with RadeonSI (GL, GFX8–10.1)
and RADV+ACO (Vulkan, GFX10.3+) as API clients over the
libdrm_amdgpu-shaped seam (see `ABI-RFC.md`).

**Doctrine (terminills):** the previous driver
(`workbench/hidds/radeonsi`) is engineering knowledge, not the
driver. This tree is written fresh; the monolith is consulted
read-only. Nothing is ported blindly — every tier validates on real
hardware before the next begins.

**Upstream tracking:** structure mirrors AMD's Linux kernel module
(IP-block init ladder, NBIO/GMC/PSP/GFX/SDMA/DCN separation) so
new-generation enablement is a diff against first-party source. MIT
register headers are vendored as excerpts under `asic_reg/` with
provenance; see the excerpt headers for the refresh recipe.

## Tier status

| Tier | Status | Hardware gate |
|---|---|---|
| 1. Device discovery: PCI probe, BAR/MMIO map, family id, VRAM size | in progress | V620 (passthrough, available now), RX 9060 XT (DUT) |
| 2. Query services (ac_drm query tier) | planned | V620 |
| 3. BO/memory domains | blocked on ABI RFC review (codex: aligned-allocation contract) | V620 |
| 4. CS/rings/fences | planned | V620 compute |
| 5. Userqueue (GFX12) | planned | RX 9060 XT |
| Display (DCE/DCN pluggable) | planned | RX 480 (DCE — delayed, cards need testing), 9060 XT (DCN4) |

Tier 1 is deliberately observe-only: the driver probes, identifies,
maps registers, and reports — it registers no display modes and
claims nothing, so the existing monolith keeps driving any AMD GPU
until this driver's gates pass on real silicon.

## Build

    make hidd-amdgpu

Not yet a member of any metatarget — it joins the ISO only when tier
1 passes on hardware.
