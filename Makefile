CHARMC=../../../bin/charmc $(OPTS)

all: libhtram.a

libhtram.a: htram.o
	$(CHARMC) htram.o -o libhtram.a -language charm++ 

htram.o : htram.C htram.def.h htram.decl.h
	$(CHARMC) -c htram.C -g

htram.def.h htram.decl.h : htram.ci
	$(CHARMC) htram.ci

test: all
	./charmrun +p16 ./driver  ++local +setcpuaffinity ++ppn 4

clean:
	rm -f *.def.h *.decl.h
	rm -f *.log.gz *.projrc *.topo *.sts *.sum
	rm libhtram.a

