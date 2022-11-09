CHARMPATH=/project/projectdirs/m4167/finegrain/charm
CHARMBUILD=mpi-crayxc
CHARMC=$(CHARMPATH)/$(CHARMBUILD)-smp/bin/charmc $(OPTS)
NONSMP_CHARMC=$(CHARMPATH)/$(CHARMBUILD)/bin/charmc $(OPTS)

all: libhtram.a libtramnonsmp.a

libtramnonsmp.a : tramNonSmp.o
	$(NONSMP_CHARMC) tramNonSmp.o -o libtramnonsmp.a -language charm++

tramNonSmp.o : tramNonSmp.C tramNonSmp.def.h tramNonSmp.decl.h
	$(NONSMP_CHARMC) -c tramNonSmp.C -o tramNonSmp.o -g

tramNonSmp.def.h tramNonSmp.decl.h : tramNonSmp.ci
	$(NONSMP_CHARMC) tramNonSmp.ci

libhtram.a: htram.o
	$(CHARMC) htram.o -o libhtram.a -language charm++ 

htram.o : htram.C htram.def.h htram.decl.h
	$(CHARMC) -c htram.C

htram.def.h htram.decl.h : htram.ci
	$(CHARMC) htram.ci

test: all
	./charmrun +p16 ./driver  ++local +setcpuaffinity ++ppn 4

clean:
	rm -f *.def.h *.decl.h
	rm -f *.log.gz *.projrc *.topo *.sts *.sum
	rm libhtram.a

