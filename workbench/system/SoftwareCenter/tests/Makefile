# Copyright (C) 2026, The AROS Development Team. All rights reserved.
#
# Host-side unit tests for the libpkg portable core.  Not part of the AROS
# mmake build; run `make -C workbench/system/SoftwareCenter/tests` before
# committing anything under libpkg/core.

CC      ?= cc
CORE    := ../libpkg/core
VENDOR  := ../libpkg/vendor
CRED    := $(VENDOR)
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CPPFLAGS:= -I$(CORE) -I$(CRED)
OUT     := build

SRCS := $(CORE)/pkg_stanza.c $(CORE)/pkg_verify.c $(CORE)/pkg_tar.c \
        $(VENDOR)/tweetnacl.c $(CRED)/credentials_crypto.c core_test.c

all: run

$(OUT):
	mkdir -p $(OUT)

$(OUT)/core_test: $(SRCS) $(CORE)/pkg_core.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Wno-unused-function -o $@ $(SRCS) -lz

# ustar fixture built with the system tar: PKGINFO, a dir, a >1-block file
$(OUT)/fixture.tar.gz: | $(OUT)
	rm -rf $(OUT)/fx && mkdir -p $(OUT)/fx/files/C
	printf 'Package: Fixture\nVersion: 1.0\nArch: all\nSummary: test\nDescription: x\n' > $(OUT)/fx/PKGINFO
	printf 'hello' > $(OUT)/fx/files/C/Hello
	head -c 300000 /dev/urandom > $(OUT)/fx/files/big.bin
	tar --format=ustar --owner=0 --group=0 --mtime='2026-01-01' -C $(OUT)/fx -cf - PKGINFO files | gzip -n > $@

run: $(OUT)/core_test $(OUT)/fixture.tar.gz
	./$(OUT)/core_test $(OUT)/fixture.tar.gz

clean:
	rm -rf $(OUT)

.PHONY: all run clean
