#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2015 Joyent, Inc.
#

PROG =			vdso_tool

include ../../../../../cmd/Makefile.cmd

OBJS =			vdso_tool.o

# Native tool, doesn't need stack protection.
STACKPROTECT = none

CLOBBERFILES +=		$(PROG)

NATIVECC_CFLAGS +=	$(CFLAGS) $(CCVERBOSE)
NATIVECC_LDLIBS +=	-lelf

.KEEP_STATE:

all: $(PROG)

install: all

lint:	lint_PROG

clean:
	$(RM) $(OBJS)

$(PROG): $(OBJS)
	$(NATIVECC) $(NATIVECC_CFLAGS) $(NATIVECC_LDLIBS) $(OBJS) -o $@
	$(POST_PROCESS)

include ../../../../../cmd/Makefile.targ
