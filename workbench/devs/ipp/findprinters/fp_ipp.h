/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef FP_IPP_H
#define FP_IPP_H

#include <exec/types.h>

#define FP_ICONS        4
#define FP_SUPPLIES     8

/* What a printer says about itself (RFC 8011 printer description and status). */
struct PrinterInfo {
    char pi_Model[128];                 /* printer-make-and-model */
    char pi_Icons[FP_ICONS][256];       /* printer-icons: pictures of the printer, small to large */
    LONG pi_NumIcons;
    char pi_State[16];                  /* idle, processing, stopped */
    char pi_Reasons[160];               /* printer-state-reasons, comma separated ("none" when all is well) */
    char pi_Message[128];               /* printer-state-message */
    char pi_Paper[96];                  /* media-ready */
    char pi_Supply[FP_SUPPLIES][48];    /* marker-names */
    LONG pi_Level[FP_SUPPLIES];         /* marker-levels, percent; -1 unknown */
    char pi_Colour[FP_SUPPLIES][16];    /* marker-colors, #RRGGBB */
    LONG pi_NumSupplies;
};

/* Get-Printer-Attributes on an ipp:// printer; FALSE when it does not answer. */
BOOL IPP_GetInfo(const char *uri, struct PrinterInfo *pi);

/* The printer's picture of itself (printer-icons), the one nearest 128 pixels, written to path
   as a PNG. FALSE when it has none or it could not be read; then *from is the picture's URL
   (NULL if it has none) and *status the HTTP status, -1 for no answer. */
BOOL IPP_SavePicture(const struct PrinterInfo *pi, const char *path, const char **from, LONG *status);

#endif
