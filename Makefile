#CHARMC =../../../bin/charmc $(OPTS)
#CHARMC_SMP =../../../bin/charmc $(OPTS)

CHARMCFLAGS = $(OPTS) -O3

BINARY=histo_nonSmp
all: $(BINARY)

include Makefile_htram

histo_nonSmp.def histo_nonSmp.decl:
	$(CHARMC) histo_nonSmp.ci

histo_nonSmp: histo_nonSmp.ci histo_nonSmp.C histo_nonSmp.def histo_nonSmp.decl.h libtramnonsmp.a
	$(CHARMC) histo_nonSmp.C libtramnonsmp.a -o histo_nonsmp -O3 -language charm++

histo-non-smp-run:
	./charmrun +p32 ./histo_nonsmp.out -n 1000000 -T 10000 +setcpuaffinity

histo_smp: histo.o libhtram.a
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram.a -language charm++ -o $@ $+ -std=c++1z

histo_smp_group: histo_group.o
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_group.a -language charm++ -o $@ $+ -std=c++1z

histo_smp_sort: histo_sort.o
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_sort.a -language charm++ -o $@ $+ -std=c++1z

ig_nonSmp.decl.h ig_nonSmp.def.h:
	$(CHARMC) ig_nonSmp.ci

ig_nonSmp: ig_nonSmp.decl.h  ig_nonSmp.def.h tramNonSmp.decl.h libtramnonsmp.a
	$(CHARMC) ig_nonSmp.C libtramnonsmp.a -O3 -language charm++  -o $@

.SECONDARY: $(patsubst %.C,%.decl.h,$(wildcard *.C))
.SECONDARY: $(patsubst %.C,%.def.h,$(wildcard *.C))

histo.def.h histo.decl.h: histo.ci.stamp

histo_g.def.h histo_g.decl.h: histo_g.ci.stamp

histo_s.def.h histo_s.decl.h: histo_s.ci.stamp

histo.ci.stamp: histo.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $<
	touch $@

histo_g.ci.stamp: histo.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $< -DGROUPBY=1
	touch $@

histo.o: histo.C histo.decl.h histo.def.h
	$(CHARMC_SMP) $(CHARMCFLAGS) -c histo.C

histo_group.o: histo.C histo_g.decl.h histo_g.def.h
	$(CHARMC_SMP) $(CHARMCFLAGS) -c histo.C -o histo_group.o -DGROUPBY=1

histo_s.ci.stamp: histo.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $< -DSORTBY=1
	touch $@

histo_sort.o: histo.C histo_s.decl.h histo_s.def.h
	$(CHARMC_SMP) $(CHARMCFLAGS) -c histo.C -o histo_sort.o -DSORTBY=1



test: $(BINARY)
	$(call run, +p4 ./histo 14 8 )
smp-run:
	./histo_smp -n 1000000 -T 10000 +setcpuaffinity ++ppn 32  +pemap 0-31 +commap 34
non-smp-run:
	./charmrun +p32 ./histo -n 1000000 -T 10000 +setcpuaffinity ++local


clean:
	$(MAKE) clean-libs
	rm -f *.o *.decl.h *.def.h $(BINARY) charmrun* *.stamp
	rm histo_nonsmp
	rm histo_smp

