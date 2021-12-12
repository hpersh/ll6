include ../shared/shared.mk

#LIBS	:= os.so math.so datetime.so socket.so regexp.so serialize.so gtk.so gtk2.so
LIBS	:= os.so math.so datetime.so socket.so regexp.so

all: $(LIBS)

math.so: $(SRC)/math.c $(SRC)/ll.h
	gcc -c -fPIC $(CFLAGS) $(SRC)/math.c -o math.o
	gcc -fPIC -shared math.o -o math.so -lm
	rm -f math.o

gtk.so: $(SRC)/gtk.c $(SRC)/ll.h
	gcc -c -fPIC $(CFLAGS) `pkg-config --cflags gtk+-3.0` $(SRC)/gtk.c -o gtk.o
	gcc -fPIC -shared gtk.o `pkg-config --libs gtk+-3.0` -o gtk.so
	rm -f gtk.o

gtk2.so: $(SRC)/gtk2.c $(SRC)/ll.h
	gcc -c -fPIC $(CFLAGS) `pkg-config --cflags gtk+-3.0` $(SRC)/gtk2.c -o gtk2.o
	gcc -fPIC -shared gtk2.o `pkg-config --libs gtk+-3.0` -o gtk2.so
	rm -f gtk2.o

%.so: $(SRC)/%.c $(SRC)/ll.h
	gcc -c -fPIC $(CFLAGS) $(SRC)/$*.c -o $*.o
	gcc -fPIC -shared $*.o -o $*.so
	rm -f $*.o

clean:
	rm -fr *.o *.so
