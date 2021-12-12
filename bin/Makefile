include ../shared/shared.mk

BIN	:= ll

all: $(BIN)

$(BIN): $(SRC)/main.c $(SRC)/ll.h
	gcc -rdynamic $(CFLAGS) $(SRC)/main.c -lreadline -lz -ldl -lm -o $(BIN)

clean:
	rm -fr $(BIN) check.out


