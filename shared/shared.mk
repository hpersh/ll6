SRC	:= ../src
LIBS	:= ../libs
DOC	:= ../doc

CFLAGS_COMMON	:= -DCYGWIN -I $(SRC) -Wall -Werror
CFLAGS_DEBUG	:= $(CFLAGS_COMMON) -g3
CFLAGS_OPTIM	:= $(CFLAGS_COMMON) -O2 -fomit-frame-pointer -DNDEBUG

ifdef DEBUG
CFLAGS	:= $(CFLAGS_DEBUG)
else
CFLAGS	:= $(CFLAGS_OPTIM)
endif

ifdef TEST
CFLAGS	+= -DTEST
endif

.PHONY:	all clean check doc

