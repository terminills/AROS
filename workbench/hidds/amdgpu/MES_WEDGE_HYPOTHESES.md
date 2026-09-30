# Navi44 MES/GFX wedge hypotheses

The first 20 are the highest-value checks. Each hypothesis must be tested by a
single observable transition, not by a successful register read.

1. PSP staged non-unified MES, but the driver starts `uni_mes`.
2. MES scheduler pipe needs `mes.bin`, not `uni_mes.bin`.
3. MES KIQ pipe needs `mes1.bin`, not the scheduler image.
4. `uc_start_addr` high dword must be treated as a 32-bit sentinel, not shifted into a 64-bit PC.
5. MES program-counter registers expect byte address, not address divided by four.
6. MES pipe0 and pipe1 require different program start addresses.
7. The driver writes pipe0 start values while pipe1 remains selected.
8. MES reset pulse is missing a readback delay.
9. MES active bits are written while reset remains asserted.
10. MES active bits must preserve unrelated CP_MES_CNTL state.
11. MES firmware autoload owns the pipe and rejects direct restart.
12. PSP autoload uses `mes.bin` but direct startup uses a different image.
13. `MAP_QUEUES` is submitted to the wrong ME selector.
14. `MAP_QUEUES` engine selector is wrong for the KIQ pipe.
15. `MAP_QUEUES` packet count is off by one.
16. `MAP_QUEUES` packet opcode is encoded for the wrong GFX generation.
17. KIQ doorbell index is wrong by one.
18. Doorbell index needs byte offset, not dword index.
19. BAR2 mapping starts at the wrong physical base.
20. Doorbell BAR writes are cached or write-combined incorrectly.
21. KIQ ring wptr shadow address is wrong.
22. KIQ ring rptr report address is wrong.
23. KIQ ring base needs a different shift than `>> 8`.
24. KIQ ring base high field needs `>> 32`, not `>> 40`.
25. Scheduler ring has the same address-shift error.
26. MQD base fields are GPU addresses, not GART virtual addresses.
27. MQD base high fields are truncated to 16 bits incorrectly.
28. MQD must be physically contiguous.
29. MQD must be cache-flushed after every field update.
30. EOP buffer is absent or has the wrong alignment.
31. EOP control size is wrong.
32. PQ control value is for compute queues, not MES queues.
33. IB control value disables the required indirect-buffer mode.
34. `DB_UPDATED_MSG_EN` is invalid for this MES firmware.
35. Persistent state value is from the wrong firmware generation.
36. HQD activation must happen through MES, not MMIO.
37. KIQ HQD must be activated after MES starts, not before.
38. Scheduler HQD must be activated on a different ME/pipe selector.
39. GRBM selection leaves the wrong queue selected during writes.
40. GRBM selection is not restored before doorbell submission.
41. VMID 0 is not privileged for the MES queues.
42. MES queues require VMID 1 or a firmware-reserved VMID.
43. GART page tables are visible to SDMA but not MES.
44. GFXHUB VMID0 base is programmed after queue activation.
45. GFXHUB TLB needs an RLC-mediated invalidate before queue submission.
46. The failed GFXHUB invalidate leaves stale zero translations.
47. GART PTE flags lack the MES-required read/write permission.
48. GART PTE flags lack executable permission for firmware fetch.
49. GART PTE address mask truncates high physical bits.
50. The GART table write has not reached VRAM before MES reads it.
51. CachePreDMA direction is wrong for the queue buffer.
52. CachePostDMA is missing after GPU rptr writes.
53. Write-combining hides the queue packet from the GPU.
54. The full-buffer cache flush overwrites a later CPU update.
55. The queue packet is written after the cache flush.
56. The KIQ wptr update is not ordered after packet visibility.
57. A readback fence is required after the doorbell write.
58. The doorbell write needs an MMIO flush read.
59. CP_ME_CNTL still holds the ME halt bit after MES activation.
60. CP_ME_CNTL programming resets MES state.
61. GFXOFF powers down the ME between queue activation and submission.
62. RLC clock gating drops ME writes while reads still work.
63. RLC F32 is alive but not servicing CP handshake traffic.
64. RLC GPM state is not compatible with MES startup.
65. PSP autoload leaves CP in a transitional state when MES is restarted.
66. `SET_HW_RESOURCES` packet layout is wrong for the firmware ABI.
67. VMID masks in `SET_HW_RESOURCES` exclude VMID0.
68. GC segment bases in `SET_HW_RESOURCES` are wrong.
69. MMHUB segment bases in `SET_HW_RESOURCES` are wrong.
70. OSSSYS segment bases in `SET_HW_RESOURCES` are wrong.
71. Scheduler context address is not aligned as required.
72. Queue fence address is not aligned as required.
73. API status fence address is not GPU-visible.
74. API status fence value is at the wrong packet offset.
75. Scheduler packet length is wrong.
76. Scheduler packet type is wrong.
77. Scheduler packet is submitted to the wrong ring.
78. Scheduler ring doorbell is not the scheduler queue doorbell.
79. MES scheduler requires a KIQ `MAP_QUEUES` packet with different payload order.
80. KIQ itself requires `MAP_QUEUES` before accepting any packet.
81. KIQ needs a firmware-specific `SET_RESOURCES` before `MAP_QUEUES`.
82. MES pipe1 firmware is not loaded even though pipe0 PC moves.
83. Pipe0 PC movement is a stale/autoload PC, not proof of packet service.
84. MES PC register selection reads pipe0 for both pipes.
85. MES PC is measured before the firmware reaches idle state.
86. MES errors are exposed only through scratch/mailbox state, not PC.
87. MES firmware rejects the queue because the MQD header is invalid.
88. MQD header version is wrong.
89. MQD static thread management fields are wrong.
90. MQD quantum configuration prevents scheduling.
91. MQD queue priority is invalid.
92. MQD `mqd_control` privilege state is wrong.
93. Scheduler queue requires a valid process-quantum or EOP state.
94. Graphics queue requires `ADD_QUEUE`, which has not been implemented.
95. Graphics queue uses the wrong ME/pipe/queue ID in `ADD_QUEUE`.
96. Graphics queue must be registered before CP_GFX_HQD is activated.
97. Direct CP_RB0 testing is invalid on GFX12 async-gfx mode.
98. PFP success is only firmware fetch/cache evidence, not graphics dispatch evidence.
99. `CP_STAT=0x80038000` is a queue-dispatch stall, not a generic memory fault.
100. The missing step is the non-unified MES pipe selection plus MES-managed `ADD_QUEUE`.
