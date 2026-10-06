/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include "sysmon_intern.h"

#include <clib/alib_protos.h>

#include <proto/oop.h>
#include <proto/muimaster.h>
#include <hidd/gfx.h>
#include <zune/graph.h>
#include <proto/dos.h>

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
    GPUS_2D,
    GPUS_VIDEO,
    GPUS_FWLOAD,
    GPUS_COUNT
};

static IPTR gpuSample[GPUS_COUNT];
static struct Hook gpuReadHooks[3];

struct gpuPen { ULONG r, g, b; };
static struct gpuPen gpuPens[3] =
{
    { 0x9B9B9B9B, 0xDEDEDEDE, 0xC0C0C0C0 },
    { 0x9E9E9E9E, 0x9B9B9B9B, 0xDEDEDEDE },
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
    struct TagItem tags[] =
    {
        { tHidd_Gfx_InfoArchitecture, 0 },
        { TAG_DONE, 0 }
    };

    if (gfxhidd)
        OOP_GetAttr(gfxhidd, aHidd_Gfx_InfoAttribs, (IPTR *)tags);
    GPUSample();
    return tags[0].ti_Data || gpuSample[GPUS_LOAD] != GPU_UNSET;
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

enum
{
    GPUI_NAME,
    GPUI_CHIP,
    GPUI_ARCH,
    GPUI_MEMORY,
    GPUI_BUS,
    GPUI_ENGINES,
    GPUI_DRIVER,
    GPUI_LOGO,
    GPUI_COUNT
};

static Object *GPUInfoObject(void)
{
    static const ULONG labels[GPUI_LOGO] =
    {
        MSG_GPU_NAME, MSG_GPU_CHIP, MSG_GPU_ARCH, MSG_GPU_MEMORY_INFO,
        MSG_GPU_BUS, MSG_GPU_ENGINES, MSG_GPU_DRIVER
    };
    struct TagItem tags[GPUI_COUNT + 1];
    Object *grp, *cols, *logo = NULL;
    BPTR lock;
    ULONG i;

    for (i = 0; i < GPUI_COUNT; i++)
    {
        tags[i].ti_Tag = tHidd_Gfx_InfoName + i;
        tags[i].ti_Data = 0;
    }
    tags[GPUI_COUNT].ti_Tag = TAG_DONE;
    OOP_GetAttr(gfxhidd, aHidd_Gfx_InfoAttribs, (IPTR *)tags);

    if (tags[GPUI_LOGO].ti_Data && (lock = Lock((CONST_STRPTR)tags[GPUI_LOGO].ti_Data, SHARED_LOCK)))
    {
        UnLock(lock);
        logo = MUI_NewObject(MUIC_Dtpic, MUIA_Dtpic_Name, tags[GPUI_LOGO].ti_Data, TAG_DONE);
    }

    cols = ColGroup(2), End;
    if (!cols)
        return NULL;
    for (i = 0; i < GPUI_LOGO; i++)
    {
        if (!tags[i].ti_Data)
            continue;
        DoMethod(cols, OM_ADDMEMBER, (IPTR)Label(_(labels[i])));
        DoMethod(cols, OM_ADDMEMBER, (IPTR)(TextObject, TextFrame, MUIA_Background, MUII_TextBack,
            MUIA_Text_Contents, tags[i].ti_Data, End));
    }

    grp = HGroup, GroupFrameT(_(MSG_GPU_ADAPTER)),
        logo ? Child : TAG_IGNORE, (IPTR)logo,
        Child, (IPTR)cols,
    End;
    return grp;
}

Object *GPUPageObject(struct SysMonData *smdata)
{
    Object *page, *info;
    ULONG i;

    info = GPUInfoObject();
    page = VGroup,
        MUIA_CycleChain, 1,
        info ? Child : TAG_IGNORE, (IPTR)info,
        Child, HGroup,
            GroupFrameT(_(MSG_USAGE)),
            Child, (IPTR)(smdata->gpugraph = GraphObject,
                MUIA_Graph_InfoText,        (IPTR)"3D --  2D --\nVideo --",
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
        static const ULONG traces[3] = { GPUS_LOAD, GPUS_2D, GPUS_VIDEO };

        for (i = 0; i < 3; i++)
        {
            APTR src = (APTR)DoMethod(smdata->gpugraph, MUIM_Graph_GetSourceHandle, i);

            gpuReadHooks[i].h_Entry = (APTR)GraphReadGPUValueFunc;
            gpuReadHooks[i].h_Data = (APTR)(IPTR)traces[i];
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
    ULONG v3d, v2d, vid, mem;

    if (!smdata->gpugraph)
        return;

    GPUSample();

#define GPUPCT(x) ((gpuSample[x] == GPU_UNSET) ? 0 : gpuSample[x])
    v3d = GPUPCT(GPUS_LOAD);
    v2d = GPUPCT(GPUS_2D);
    vid = GPUPCT(GPUS_VIDEO);
    mem = GPUPCT(GPUS_MEMLOAD);
#undef GPUPCT
    __sprintf(buffer, "3D %d.%d %%  2D %d.%d %%\n%s %d.%d %%  %s %d.%d %%",
        v3d / 10, v3d % 10, v2d / 10, v2d % 10,
        _(MSG_GPU_VIDEO), vid / 10, vid % 10,
        _(MSG_GPU_MEM_SHORT), mem / 10, mem % 10);
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
