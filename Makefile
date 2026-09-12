include Makefile.common

CHARMCFLAGS = $(OPTS) -O3

BINARY=histo_smp
all: $(BINARY)

include Makefile_htram

.PHONY: histo_smp

histo_smp: histo.C histo.ci histo.decl.h histo.def.h libhtram_group_histo.a
	$(CHARMC_SMP) histo.ci -DTRAM_SMP -DGROUPBY
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_group_histo.a -language charm++ -o $@ $< -std=c++1z -DTRAM_SMP -DGROUPBY -DHISTO

ig_smp: smp_ig.C smp_ig.ci smp_ig.decl.h smp_ig.def.h libhtram_group_ig.a
	$(CHARMC_SMP) smp_ig.ci -DTRAM_SMP -DGROUPBY
	$(CHARMC_SMP) $(CHARMCFLAGS) libhtram_group_ig.a -language charm++ -o $@ $< -std=c++1z -DTRAM_SMP -DGROUPBY -DIG

.SECONDARY: $(patsubst %.C,%.decl.h,$(wildcard *.C))
.SECONDARY: $(patsubst %.C,%.def.h,$(wildcard *.C))

histo.def.h histo.decl.h: histo.ci.stamp

smp_ig.def.h smp_ig.decl.h: smp_ig.ci.stamp

histo.ci.stamp: histo.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $<
	touch $@

smp_ig.ci.stamp: smp_ig.ci
	$(CHARMC_SMP) $(CHARMCFLAGS) $<
	touch $@

histo.o: histo.C histo.decl.h histo.def.h
	$(CHARMC_SMP) $(CHARMCFLAGS) -c histo.C

smp-run: histo_smp
	./histo_smp -n 1000000 -T 10000 +setcpuaffinity ++ppn 32 +pemap 0-31 +commap 34

clean:
	$(MAKE) clean-libs
	rm -f *.o *.decl.h *.def.h $(BINARY) charmrun* *.stamp
