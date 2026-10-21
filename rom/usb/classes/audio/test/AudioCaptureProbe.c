/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Bounded native capture/stop/reopen test. Optional HEX argument exports the
 * first second of stereo PCM to stdout for retrieval from a serial console.
 */
#include <exec/types.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>
#include <aros/asmcall.h>
#include <utility/hooks.h>
#include <stdio.h>
#include <string.h>

struct Library *AHIBase;
static volatile ULONG calls, samples, nonzero, peak, saved;
static WORD pcm[48000 * 2];

AROS_UFH3(void, RecordHook,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(struct AHIAudioCtrl *, control, A2),
    AROS_UFHA(struct AHIRecordMessage *, message, A1))
{
    AROS_USERFUNC_INIT
    WORD *data = message->ahirm_Buffer;
    ULONG i;
    calls++;
    if(message->ahirm_Type == AHIST_S16S)
    {
        for(i = 0; i < message->ahirm_Length * 2; i++)
        {
            LONG value = data[i];
            if(saved < sizeof(pcm) / sizeof(pcm[0]))
                pcm[saved++] = data[i];
            if(value < 0) value = -value;
            if(value) nonzero++;
            if(value > peak) peak = value;
            samples++;
        }
    }
    AROS_USERFUNC_EXIT
}

int main(int argc, char **argv)
{
    struct MsgPort *port = CreateMsgPort();
    struct AHIRequest *io = NULL;
    struct Hook hook = {0};
    ULONG id = AHI_INVALID_ID, selected = AHI_INVALID_ID;
    int pass, result = 20;
    BOOL opened = FALSE;

    if(!port) goto cleanup;
    io = (struct AHIRequest *)CreateIORequest(port, sizeof(*io));
    if(!io) goto cleanup;
    io->ahir_Version = 4;
    if(OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0))
        goto cleanup;
    opened = TRUE;
    AHIBase = (struct Library *)io->ahir_Std.io_Device;

    while((id = AHI_NextAudioID(id)) != AHI_INVALID_ID)
    {
        char name[128] = {0};
        ULONG record = 0;
        AHI_GetAudioAttrs(id, NULL, AHIDB_BufferLen, sizeof(name),
            AHIDB_Name, (IPTR)name, AHIDB_Record, (IPTR)&record, TAG_DONE);
        printf("AudioCaptureProbe: mode=%08lx record=%lu name=%s\n",
            (unsigned long)id, (unsigned long)record, name);
        /* USB audio reserves the 0x003b family. Never record another device. */
        if(record && (id & 0xffff0000UL) == 0x003b0000UL)
            selected = id;
    }
    fflush(stdout);
    if(selected == AHI_INVALID_ID) goto cleanup;
    hook.h_Entry = (HOOKFUNC)RecordHook;
    for(pass = 0; pass < 2; pass++)
    {
        struct AHIAudioCtrl *control;
        ULONG error, stopped;
        calls = samples = nonzero = peak = saved = 0;
        control = AHI_AllocAudio(AHIA_AudioID, selected, AHIA_MixFreq, 48000,
            AHIA_Channels, 1, AHIA_Sounds, 1,
            AHIA_RecordFunc, (IPTR)&hook, TAG_DONE);
        if(!control) goto cleanup;
        error = AHI_ControlAudio(control, AHIC_Record, TRUE, TAG_DONE);
        printf("AudioCaptureProbe: pass=%d start=%lu\n", pass + 1, (unsigned long)error);
        fflush(stdout);
        if(!error) Delay(250);
        AHI_ControlAudio(control, AHIC_Record, FALSE, TAG_DONE);
        stopped = calls;
        Delay(10);
        printf("AudioCaptureProbe: pass=%d callbacks=%lu samples=%lu nonzero=%lu peak=%lu stopped=%s\n",
            pass + 1, (unsigned long)calls, (unsigned long)samples,
            (unsigned long)nonzero, (unsigned long)peak, calls == stopped ? "yes" : "no");
        fflush(stdout);
        AHI_FreeAudio(control);
        if(error || !samples || calls != stopped) goto cleanup;
    }
    if(argc > 1 && !strcmp(argv[1], "HEX"))
    {
        ULONG i;
        printf("PCM_BEGIN rate=48000 channels=2 bits=16 samples=%lu\n", (unsigned long)saved);
        for(i = 0; i < saved; i++)
        {
            UWORD value = (UWORD)pcm[i];
            printf("%02x%02x", value & 255, value >> 8);
            if((i & 31) == 31) putchar('\n');
        }
        printf("\nPCM_END\n");
    }
    result = 0;
cleanup:
    if(opened) CloseDevice((struct IORequest *)io);
    if(io) DeleteIORequest((struct IORequest *)io);
    if(port) DeleteMsgPort(port);
    printf("AudioCaptureProbe: %s\n", result ? "FAIL" : "PASS");
    return result;
}
