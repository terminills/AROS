# AMDGPU UpdateRect compositor notification

## v1 — physical cold42 stack acceptance PASS

Source87d8951259 built502240B, SHA256
c74d7350b0b9ba473faab940d17475fe03ce8a7c43bcb4c6c1536f3a51172f2e.
Verified Work rollback501864B, candidate installed-byte comparison502240B
(native0 at00002c88d1d6ac50), completed physical OFF12000/dwell>=20s/ON800.
Cold42 selected AMDGPU00110000,1920x1080; no driver reload before tests.

Unchanged ScreenStackObserve-v2 compared IDENTICAL45048/native0 before use.
All six fresh HDMI stages520..525 matched. During UPDATED, active composite
MC80028bf000 immediately contains upper00dd33bb/lower00eebb33; before/after
hold samples agree, unchanged blue/green background samples agree, and fresh
522 at15:51:15UTC visibly shows both magenta/amber markers BEFORE reordering.
Both remain visible on TOP-RETURNED524; final restoration and all4096-pixel
backing checks pass. Actual native0/error0/ioerr0 at0000043ab3a11db3.

This closes the demonstrated missing UpdateRect notification. On this same
rebooted binary, unchanged SDMA copy checks returned native0 at0000054efa71c9fd,
drawing18PASS/0FAIL returned native0 at000005b8a029ca6c, and panning returned
native0 at0000070f0a8337c5, with fresh HDMI checks. The unchanged software cursor
probe returned native10 at0000066cb4340405: its final aggregate background
sentinel check failed despite all seven reported positions matching. Cause is
not established; this failure is preserved and pointer investigation is deferred
by user direction while 3D work proceeds. Not a clean full2D baseline,
hardware-pointer, general modeset or Mesa acceptance. Existing SFS requesters
were canceled through KVM, with failed input/launch delays retained in raw
logs. No GUI/filesystem changes. See registered artifact set
JOB-0084-native-completion-20260906/updaterect-v1-provenance.md.

## Diagnosis and scope

JOB-0084, 2026-09-07. REQ-0039 / OVR-0043 and Claude's TRIG-0951
handoff authorize the isolated BitMap UpdateRect change. The existing foreign
ARGB PutImage/GetImage edits are preserved and are not part of this commit.

Cold38's unchanged ScreenStackObserve-v2 showed correct magenta/amber backing
pixels but unchanged blue/green active composite pixels during UPDATED. Fresh
v4 HDMI capture agreed. The separate stale TOP-RETURNED capture was a KVM
capture issue, not evidence for a GPU cache or plane fix.

ADT Nouveau forwards UpdateRect on displayable bitmaps to the compositor's
BitMapRectChanged method. AMDGPU only refreshed data->disp (direct scanout),
missing bitmaps visible through a split-screen composite. This candidate adds
the same notification, preserving the direct-framebuffer refresh. Notification
runs outside the framebuffer lock, respecting compositor -> framebuffer lock
order. Existing compositor clipping, stack identity resolution and CopyBox
handling are reused without changes. No GUI, filesystem or pointer changes.

Required acceptance: focused driver build, immutable binary hash, rollback
copy and native installed-byte comparison, physical OFF/dwell/ON (cold42),
then the unchanged resident-driver ScreenStackObserve-v2 with active-composite
samples and fresh HDMI at every stage. Drawing, SDMA copy, cursor and panning
regressions follow. A successful build alone is not driver acceptance.
