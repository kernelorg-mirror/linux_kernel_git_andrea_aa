# Makefile for vm selftests

CFLAGS = -Wall
BINARIES = hugepage-mmap hugepage-shm map_hugetlb thuge-gen hugetlbfstest userfaultfd
BINARIES += transhuge-stress

all: $(BINARIES)
%: %.c
	$(CC) $(CFLAGS) -o $@ $^ -lrt
userfaultfd: userfaultfd.c
	$(CC) $(CFLAGS) -o $@ $^ -g -lrt -lpthread

TEST_PROGS := run_vmtests
TEST_FILES := $(BINARIES)

include ../lib.mk

clean:
	$(RM) $(BINARIES)
