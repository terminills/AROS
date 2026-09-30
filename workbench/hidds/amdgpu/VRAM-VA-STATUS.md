# Managed VRAM GPU bindings (JOB-0084)

Cold55 installed/rebooted library0.7 accepted all four actual GPU domain pairs:
GTT/GTT, GTT/VRAM, VRAM/GTT, VRAM/VRAM. OffsetIB and second-page destinations
76000001..4, both guard pairs, realEOP, pending-owner protections and exact
backing/reference recovery passed. Work/RAM57888 comparison native0 at
0000060f5cf0f1bf; executionnative0/error0/ioerr0 at000006460c735c78.
Raw cold55-vram-va-v1-pass.uart.log SHA256
1a96e451a53c8dd374fa9a91d6f3ebb1234431ea60d73695f80e7759683ab1d7.
VAv2Work/RAM78464 comparison native0 at000006863bfa620d; retainedGTT and
VRAMbinding/generation/64slot pressure tests native0 at0000069a3541a23c.
Raw cold55-va-v2-domains-pass.uart.log SHA256
f108c063f1da6c92f8ed1585c3eb45247d7c1d81520fbbddb88718caa5ee9f23.
These supersede the pending acceptance notes below, not the separate Mesa
adapter's still-GTT-only allocation policy. No visible hardware3D yet.

Library0.6 remains frozen and UNINSTALLED. Cold54 found a0.5 ring-position
comparison defect at the first wrap; library0.7/403068fd46 corrects that while
retaining this separately committed VRAM feature. Cold55 must pass the unchanged
ring client, then the VRAM/domain clients below. No VRAM acceptance yet.
No desktop allocator, pool budget, scanout, pointer,
GUI or filesystem changes. No new public vectors or structure layouts.

The existing BO registry now retains its aligned VRAM payload MC address
separately from the exact allocation MC address used for freeing. VA domains
advertise GTT plus VRAM only in this candidate. Whole-BO, immutable fresh-range
bindings retain the same BO/GPU references and quarantine on failed PTE changes.

GFX12 local VRAM PTEs use UC, VALID/EXE/READ/WRITE/IS_PTE, not SYSTEM/SNOOPED.
The address is actual MMMC_VM_FB_OFFSET shifted24 plus payload offset within
VRAM, NOT the CPU BAR or absolute MC-window address. Bounds validate every page
within physical VRAM and the48-bit PTE field. Each PTE is read back; both hubs
must acknowledge invalidation. Existing checked unbind retires both domains.

Primary reference: Linux drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c
amdgpu_vm_bo_update and amdgpu_vm_bo_update_mapping choose vram_base_offset
plus resource offset. gmc_v12_0_get_vm_pte and amdgpu_ttm_tt_pte_flags provide
local-memory flags; the existing native root setup already uses FB_OFFSET.
BAR WC stores/reads use full fences around publication/real-EOP retirement;
CachePreDMA/PostDMA remain on system GTT only. No CPU BAR is translated as RAM.

AmdgpuVramVaProbe requires the actual resident0.6 driver and both domain caps.
Four independent IB/destination pairs (GTT/GTT,GTT/VRAM,VRAM/GTT,VRAM/VRAM)
use64KiB-aligned8192B payloads, a256B IB offset and second-page destination.
Each checks real managed GPU execution, private EOP retirement, both adjacent
guard pairs, pending owner-release/unbind blocking and exact quota/ref cleanup.
It never claims a CPU or PTE check is GPU execution. AmdgpuVaProbe-v2 retains
old GTT tests and follows the advertised VRAM domain instead of demanding the
old absence; the frozenv1 remains for cold54.

Source8b133131bf built sequentially under native-smp lease: driver, new VRAM
client and updated VA client all actual compile/linkexit0 with no diagnostics.
Frozen amdgpu-vram-va-v1.hidd545184B SHA256
573df22fe9b465a0351b67450762c7ea20983633bf8a5dce9f975b2512869603;
AmdgpuVramVaProbe-v1 57888B SHA256
c1fdcd6cca239673f088fdbfb36bd64bc06ed7c82663450419a5bf5ec8394791;
AmdgpuVaProbe-v2-domains78464B SHA256
9d435500936fe3891f5f917f4a9bd5a35dc3092e78a635f554d693cd4c0dcb1b.
Logs build-vram-va-v1-driver.log/build-vram-va-v1-probe.log/
build-va-probe-v2-domains.log. Physical install/reboot/acceptance remains
pending. No rendered3D or complete Mesa storage capability is claimed.
