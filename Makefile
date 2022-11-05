CHARMC=../../../bin/charmc $(OPTS)
NONSMP_CHARMC=../../../bin/charmc $(OPTS)

all: libhtram.a libtramnonsmp.a

libtramnonsmp.a : tramnonsmp.o
	$(NONSMP_CHARMC) tramnonsmp.o -o libtramnonsmp.a -language charm++

tramnonsmp.o : tramNonSmp.C tramNonSmp.def.h tramNonSmp.decl.h
	$(NONSMP_CHARMC) -c tramNonSmp.C -o tramnonsmp.o -g

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

