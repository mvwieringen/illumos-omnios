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
# Copyright 2020 Tintri by DDN, Inc. All rights reserved.
#

SUBDIRS = \
	netrlogon

ROOTOPTDIR = $(ROOT)/opt/libmlrpc-tests/tests

all     :=      TARGET = all
install :=      TARGET = install
clean   :=      TARGET = clean
clobber :=      TARGET = clobber
lint    :=      TARGET = lint

.KEEP_STATE:

install: $(SUBDIRS)

all: $(SUBDIRS)

clean lint: $(SUBDIRS)

$(ROOTOPTDIR):
	$(INS.dir)

clobber: $(SUBDIRS)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:
