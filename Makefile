CHARMC=../../../bin/charmc $(OPTS) -O3

all: libhtram.a libtramnonsmp.a

libtramnonsmp.a : tramnonsmp.o
	$(CHARMC) tramnonsmp.o -o libtramnonsmp.a -language charm++

tramnonsmp.o : tramNonSmp.C tramNonSmp.def.h tramNonSmp.decl.h
	$(CHARMC) -c tramnonsmp.o -g

tramNonSmp.def.h tramNonSmp.decl.h : tramNonSmp.ci
	$(CHARMC) tramNonSmp.ci

libhtram.a: htram.o
	$(CHARMC) htram.o -o libhtram.a -language charm++ 

htram.o : htram.C htram.def.h htram.decl.h
	$(CHARMC) -c htram.C

htram.def.h htram.decl.h : htram.ci
	$(CHARMC) htram.ci

libhtram_group.a: htram_group.o
	$(CHARMC) htram_group.o -o libhtram_group.a -language charm++

htram_group.o : htram_group.C htram_group.def.h htram_group.decl.h
	$(CHARMC) -c htram_group.C

htram_group.def.h htram_group.decl.h : htram_group.ci
	$(CHARMC) htram_group.ci

clean:
	rm -f *.def.h *.decl.h
	rm -f *.log.gz *.projrc *.topo *.sts *.sum
	rm libhtram.a

