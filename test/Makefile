.PHONY:	all ll regexp

all:	ll

ll:
	TEST=1 make -C ../bin clean all
	./extract ../src/main.c
	../bin/ll test.l >test.out
	cmp test.out golden.out

regexp:
	make -C ../bin all
	./extract ../src/regexp.c
	../bin/ll test.l >test.out
	cmp test.out golden.out
