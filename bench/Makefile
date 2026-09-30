# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

#
# Standalone Makefile for the guac_display benchmark.
#
# This deliberately sits outside the autotools build: the benchmark reaches
# into the private headers of libguac and is only ever built by hand, against
# a tree that has already been configured and built.
#
# Usage, from the top of an already-built source tree:
#
#     make -C bench
#     ./bench/guacbench
#

TOP := $(CURDIR)/..

CFLAGS += -O2 -g -Wall -pthread \
	-I$(TOP) \
	-I$(TOP)/src/libguac \
	$(shell pkg-config --cflags cairo)

LIBGUAC := $(TOP)/src/libguac/.libs/libguac.a

LIBS := $(LIBGUAC) \
	$(shell pkg-config --libs cairo libpng libwebp) \
	-ljpeg -lssl -lcrypto -lpthread -lm -ldl

# libguac may be built against either libuuid (util-linux) or OSSP uuid.
# Link whichever is present.
LIBS += $(shell pkg-config --silence-errors --libs uuid || echo -luuid)
ifneq ($(wildcard /usr/include/ossp/uuid.h),)
LIBS += -lossp-uuid
endif

all: guacbench

guacbench: guacbench.c $(LIBGUAC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f guacbench

.PHONY: all clean
