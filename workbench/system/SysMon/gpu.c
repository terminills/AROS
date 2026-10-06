/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include "sysmon_intern.h"

#include <clib/alib_protos.h>

#include <proto/oop.h>
#include <proto/muimaster.h>
#include <hidd/gfx.h>
#include <zune/graph.h>

#include "locale.h"

extern OOP_Object *gfxhidd;
extern OOP_AttrBase HiddGfxAttrBase;

#define GPU_UNSET   (~(IPTR)0)

enum
{
    GPUS_LOAD,
    GPUS_MEMLOAD,
    GPUS_CORECLK,
    GPUS_MEMCLK,
    GPUS_TEMP,
    GPUS_HOTSPOT,
    GPUS_MEMTEMP,
    GPUS_POWER,
    GPUS_FAN,
    GPUS_COUNT
};

static IPTR gpuSample[GPUS_COUNT];
static struct Hook gpuReadHooks[2];

struct gpuPen { ULONG r, g, b; };
static struct gpuPen gpuPens[2] =
{
    { 0x9B9B9B9B, 0xDEDEDEDE, 0xC0C0C0C0 },
    { 0xDEDEDEDE, 0xB5B5B5B5, 0x9B9B9B9B },
};

static VOID GPUSample(VOID)
{
    struct TagItem tags[GPUS_COUNT + 1];
    ULONG i;

    for (i = 0; i < GPUS_COUNT; i++)
    {
        tags[i].ti_Tag = tHidd_Gfx_SensorLoad + i;
        tags[i].ti_Data = GPU_UNSET;
    }
    tags[GPUS_COUNT].ti_Tag = TAG_DONE;

    if (gfxhidd)
        OOP_GetAttr(gfxhidd, aHidd_Gfx_SensorAttribs, (IPTR *)tags);

    for (i = 0; i < GPUS_COUNT; i++)
        gpuSample[i] = tags[i].ti_Data;
}

BOOL GPUSensorsAvailable(VOID)
{
    GPUSample();
    return gpuSample[GPUS_LOAD] != GPU_UNSET;
}

AROS_UFH3(IPTR, GraphReadGPUValueFunc,
        AROS_UFHA(struct Hook *, h, A0),
        AROS_UFHA(IPTR *, storage, A2),
        AROS_UFHA(IPTR, which, A1))
{
    AROS_USERFUNC_INIT

    IPTR v = gpuSample[which];

    *storage = (v == GPU_UNSET) ? 0 : v;
    return TRUE;

    AROS_USERFUNC_EXIT
}

static Object *GPUValueObject(Object **store)
{
    return *store = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_PreParse, (IPTR)"\33r", MUIA_Text_Contents, (IPTR)"--",
            End;
}

Object *GPUPageObject(struct SysMonData *smdata)
{
    Object *page;
    ULONG i;

    page = VGroup,
        MUIA_CycleChain, 1,
        Child, HGroup,
            GroupFrameT(_(MSG_USAGE)),
            Child, (IPTR)(smdata->gpugraph = GraphObject,
                MUIA_Graph_InfoText,        (IPTR)"GPU --\n--.- %",
                MUIA_Graph_ValueCeiling,    1000,
                MUIA_Graph_ValueStep,       100,
                MUIA_Graph_PeriodCeiling,   100000,
                MUIA_Graph_PeriodStep,      10000,
                MUIA_Graph_PeriodInterval,  1000,
            End),
        End,
        Child, HGroup,
            Child, VGroup, GroupFrameT(_(MSG_GPU_CLOCKS)),
                Child, ColGroup(2),
                    Child, Label(_(MSG_GPU_CORE_CLOCK)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_CORECLK]),
                    Child, Label(_(MSG_GPU_MEM_CLOCK)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_MEMCLK]),
                    Child, Label(_(MSG_GPU_POWER)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_POWER]),
                    Child, Label(_(MSG_GPU_FAN)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_FAN]),
                End,
                Child, HVSpace,
            End,
            Child, VGroup, GroupFrameT(_(MSG_GPU_TEMPERATURES)),
                Child, ColGroup(2),
                    Child, Label(_(MSG_GPU_TEMP_EDGE)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_TEMP]),
                    Child, Label(_(MSG_GPU_TEMP_HOTSPOT)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_HOTSPOT]),
                    Child, Label(_(MSG_GPU_TEMP_MEM)),
                    Child, GPUValueObject(&smdata->gpuvalues[GPUS_MEMTEMP]),
                End,
                Child, HVSpace,
            End,
        End,
        Child, VGroup, GroupFrameT(_(MSG_VIDEO_RAM)),
            Child, (IPTR)(smdata->gpuvram = GaugeObject,
                GaugeFrame,
                MUIA_Gauge_Horiz, TRUE,
                MUIA_Gauge_Max, 1000,
                MUIA_Gauge_InfoText, (IPTR)"--",
            End),
        End,
        Child, HVSpace,
    End;

    if (page)
    {
        for (i = 0; i < 2; i++)
        {
            APTR src = (APTR)DoMethod(smdata->gpugraph, MUIM_Graph_GetSourceHandle, i);

            gpuReadHooks[i].h_Entry = (APTR)GraphReadGPUValueFunc;
            gpuReadHooks[i].h_Data = (APTR)(IPTR)(i == 0 ? GPUS_LOAD : GPUS_MEMLOAD);
            DoMethod(smdata->gpugraph, MUIM_Graph_SetSourceAttrib, src,
                MUIV_Graph_Source_PenSrc, &gpuPens[i]);
            DoMethod(smdata->gpugraph, MUIM_Graph_SetSourceAttrib, src,
                MUIV_Graph_Source_ReadHook, &gpuReadHooks[i]);
        }
    }

    return page;
}

static VOID GPUSetValue(struct SysMonData *smdata, ULONG idx, CONST_STRPTR fmt, IPTR div)
{
    TEXT buffer[32];

    if (gpuSample[idx] == GPU_UNSET)
        set(smdata->gpuvalues[idx], MUIA_Text_Contents, (IPTR)"--");
    else
    {
        __sprintf(buffer, fmt, (ULONG)(gpuSample[idx] / div));
        set(smdata->gpuvalues[idx], MUIA_Text_Contents, (IPTR)buffer);
    }
}

VOID UpdateGPUInformation(struct SysMonData *smdata)
{
    TEXT buffer[96];
    struct TagItem memTags[] =
    {
        { tHidd_Gfx_MemTotal,   0 },
        { tHidd_Gfx_MemFree,    0 },
        { TAG_DONE,             0 }
    };
    ULONG load, memload;

    if (!smdata->gpugraph)
        return;

    GPUSample();

    load = (gpuSample[GPUS_LOAD] == GPU_UNSET) ? 0 : gpuSample[GPUS_LOAD];
    memload = (gpuSample[GPUS_MEMLOAD] == GPU_UNSET) ? 0 : gpuSample[GPUS_MEMLOAD];
    __sprintf(buffer, "GPU %d.%d %%\n%s %d.%d %%", load / 10, load % 10,
        _(MSG_GPU_MEM_SHORT), memload / 10, memload % 10);
    set(smdata->gpugraph, MUIA_Graph_InfoText, (IPTR)buffer);

    GPUSetValue(smdata, GPUS_CORECLK, "%ld MHz", 1000000);
    GPUSetValue(smdata, GPUS_MEMCLK, "%ld MHz", 1000000);
    GPUSetValue(smdata, GPUS_POWER, "%ld W", 1000);
    GPUSetValue(smdata, GPUS_FAN, "%ld RPM", 1);
    GPUSetValue(smdata, GPUS_TEMP, "%ld \xB0" "C", 1000);
    GPUSetValue(smdata, GPUS_HOTSPOT, "%ld \xB0" "C", 1000);
    GPUSetValue(smdata, GPUS_MEMTEMP, "%ld \xB0" "C", 1000);

    OOP_GetAttr(gfxhidd, aHidd_Gfx_MemoryAttribs, (IPTR *)memTags);
    if (memTags[0].ti_Data)
    {
        UQUAD total = memTags[0].ti_Data;
        UQUAD used = total - memTags[1].ti_Data;

        __sprintf(buffer, "%ld / %ld MB", (ULONG)(used >> 20), (ULONG)(total >> 20));
        set(smdata->gpuvram, MUIA_Gauge_Current, (IPTR)((used * 1000) / total));
        set(smdata->gpuvram, MUIA_Gauge_InfoText, (IPTR)buffer);
    }
}
