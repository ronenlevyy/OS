CC=g++
CXX=g++

CODESRC= memory_latency.cpp
EXESRC= $(CODESRC) measure.cpp
EXEOBJ= memory_latency

INCS=-I.
CFLAGS = -Wall -std=c++11 -O3 $(INCS) -o
CXXFLAGS = -Wall -std=c++11 -O3 $(INCS) -o

TARGETS = $(EXEOBJ)

TAR=tar
TARFLAGS=-cvf
TARNAME=ex1.tar
TARSRCS=$(CODESRC) Makefile README lscpu.png page_size.png results.png

all: $(TARGETS)

$(TARGETS): $(EXESRC)
	$(CXX) $(CXXFLAGS) $@ $^

clean:
	$(RM) $(TARGETS)

depend:
	makedepend -- $(CFLAGS) -- $(SRC) $(EXESRC)

tar:
	$(TAR) $(TARFLAGS) $(TARNAME) $(TARSRCS)# DO NOT DELETE

memory_latency.o: memory_latency.h measure.h
measure.o: memory_latency.h measure.h
