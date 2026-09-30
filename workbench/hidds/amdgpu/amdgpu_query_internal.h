#ifndef AMDGPU_QUERY_INTERNAL_H
#define AMDGPU_QUERY_INTERNAL_H

struct AmdgpuDevice;
struct AmdgpuFirmwareInfo;
/* Init ladder only, before publishing the display and public CS service. */
void amdgpu_render_info_init(struct AmdgpuDevice *ad);
/* PSP init ladder only. The public query copies the final cached record. */
void amdgpu_firmware_info_reset(struct AmdgpuDevice *ad);
void amdgpu_firmware_info_record(struct AmdgpuDevice *ad,
                                 const struct AmdgpuFirmwareInfo *info);

#endif
