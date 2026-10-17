#ifndef IA_PACKAGES_H
#define IA_PACKAGES_H
/*
    Copyright (C) 2018-2026, The AROS Development Team. All rights reserved.
    $Id$
*/

void PACKAGES_InitSupport(struct List *);
void PACKAGES_AddCoreSkipPaths(struct List *);
void PACKAGES_DoInstall(Class * CLASS, Object * self, CONST_STRPTR destinationPath);
void PACKAGES_InstallBackdrop(CONST_STRPTR destinationPath);

#endif /* IA_PACKAGES_H */