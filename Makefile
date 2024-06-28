CHARMC =../../../ofi-linux-x86_64-cxi-slurmpmi2cray/bin/charmc $(OPTS)
CHARMC_SMP =../../../ofi-linux-x86_64-cxi-slurmpmi2cray-smp/bin/charmc $(OPTS)

CHARMCFLAGS = $(OPTS) -O3

BINARY=histo_nonSmp histo_smp
all: $(BINARY)

include Makefile_htram

.PHONY: histo_nonSmp histo_smp

histo_smp: histo.C histo.ci histo.decl.h histo.def.h libhtram_group.a
	$(CHARMC_SMP) histo.ci -DTRAM_SMP -DGROUPBY
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_group.a -language charm++ -o $@ $< -std=c++1z -DTRAM_SMP -DGROUPBY 

smp_ig: smp_ig.C smp_ig.ci smp_ig.decl.h smp_ig.def.h libhtram_group.a
	$(CHARMC_SMP) smp_ig.ci -DTRAM_SMP -DGROUPBY
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_group.a -language charm++ -o $@ $< -std=c++1z -DTRAM_SMP -DGROUPBY 

histo_nonSmp: histo.C histo.ci histo.decl.h histo.def.h libtramnonsmp.a
	$(CHARMC) histo.ci -DTRAM_NON_SMP
	$(CHARMC) $(CHARMCFLAGS) libtramnonsmp.a -language charm++ -o $@ $< -std=c++1z -DTRAM_NON_SMP

histo-non-smp-run: histo_nonSmp
	./charmrun +p32 ./histo_nonSmp -n 1000000 -T 10000 +setcpuaffinity

histo-smp-run: histo_smp
	./charmrun +p32 ./histo_smp -n 1000000 -T 10000 +setcpuaffinity

ig_nonSmp.decl.h ig_nonSmp.def.h:
	$(CHARMC) ig_nonSmp.ci

ig_nonSmp: ig_nonSmp.decl.h  ig_nonSmp.def.h tramNonSmp.decl.h libtramnonsmp.a
	$(CHARMC) ig_nonSmp.C libtramnonsmp.a -O3 -language charm++  -o $@

.SECONDARY: $(patsubst %.C,%.decl.h,$(wildcard *.C))
.SECONDARY: $(patsubst %.C,%.def.h,$(wildcard *.C))

histo.def.h histo.decl.h: histo.ci.stamp

smp_ig.def.h smp_ig.decl.h: smp_ig.ci.stamp

histo_g.def.h histo_g.decl.h: histo_g.ci.stamp

histo_s.def.h histo_s.decl.h: histo_s.ci.stamp

histo.ci.stamp: histo.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $<
	touch $@

smp_ig.ci.stamp: smp_ig.ci
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

