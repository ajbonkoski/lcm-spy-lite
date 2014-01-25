
CFLAGS_LCM  := `pkg-config --cflags lcm`
LDFLAGS_LCM := `pkg-config --libs lcm`

CFLAGS := -std=gnu99 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE\
          -D_REENTRANT -Wall -Wno-unused-parameter -Wno-format-zero-length -pthread\
          $(CFLAGS_LCM)

LDFLAGS := $(LDFLAGS_LCM) -ldl

CC := gcc

H_FILES := $(shell ls *.h)
C_FILES := $(shell ls *.c)
O_FILES := $(patsubst %.c,../obj/%.o,$(C_FILES))

BIN := ../bin/lcm-spy-lite
ALL := $(O_FILES) $(BIN)

all: $(ALL)

$(BIN): $(O_FILES) $(H_FILES)
	$(CC) -o $@ $^ $(LDFLAGS)

../obj/%.o: %.c $(H_FILES)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ALL)
