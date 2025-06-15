CC=g++
CXX=g++
RANLIB=ranlib

LIBSRC=VirtualMemory.cpp
LIBOBJ=$(LIBSRC:.cpp=.o)

INCS=-I.
CFLAGS = -Wall -std=c++11 -g $(INCS)
CXXFLAGS = -Wall -std=c++11 -g $(INCS)

OSMLIB = libVirtualMemory.a
TARGETS = $(OSMLIB)

TAR=tar
TARFLAGS=-cvf
TARNAME=ex4.tar
TARSRCS=$(LIBSRC) Makefile README

all: $(TARGETS)

$(OSMLIB): $(LIBOBJ)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

uthreads_program: $(LIBOBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	$(RM) $(TARGETS) $(OSMLIB) $(LIBOBJ) *~ *core

depend:
	makedepend -- $(CFLAGS) -- $(LIBSRC)

tar:
	$(TAR) $(TARFLAGS) $(TARNAME) $(TARSRCS)



