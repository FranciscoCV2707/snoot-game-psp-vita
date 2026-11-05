PROJECT_TITLE := Snoot Game
PROJECT_TITLEID := SNOOT001
PROJECT := snoot

CC := arm-vita-eabi-gcc
STRIP := arm-vita-eabi-strip

SRC := /src
OUT := /src/build/vita
SDL2_PC := $(OUT)/sdl2/lib/pkgconfig/sdl2.pc

SRCS := $(wildcard $(SRC)/engine/core/*.c) \
        $(wildcard $(SRC)/engine/app/*.c) \
        $(wildcard $(SRC)/engine/modules/*.c) \
        $(wildcard $(SRC)/games/snoot/*.c) \
        $(wildcard $(SRC)/platforms/sdl/*.c)

OBJS := $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(SRCS))

CFLAGS := -std=c99 -Wall -Wextra -Wno-unused-parameter -pedantic -O2 -Wl,-q
CFLAGS += -I$(SRC)/engine/core -I$(SRC)/engine/platform -I$(SRC)/engine/app \
          -I$(SRC)/engine/modules -I$(SRC)/games/snoot -I$(SRC)/platforms/sdl \
          -I$(OUT)/generated
CFLAGS += $(shell pkg-config $(SDL2_PC) --cflags)
# El perfil de la consola entra por aqui (platforms/vita/build.py --profile): view_flags + presentation_flags.
# Variable aparte porque pasar CFLAGS= en la linea de make anularia las de arriba.
CFLAGS += $(EXTRA_CFLAGS)

LDFLAGS := -Wl,-q -Wl,--defsym,__sce_headroom=0x10000
LIBS := $(shell pkg-config $(SDL2_PC) --libs) -lvorbisfile -lvorbis -logg -lm

ifeq ($(VN_TRACE),1)
CFLAGS += -DVN_TRACE
endif

.PHONY: all package clean
all: package

package: $(OUT)/snoot.vpk

$(OUT)/snoot.vpk: $(OUT)/eboot.bin $(OUT)/param.sfo
	vita-pack-vpk -s $(OUT)/param.sfo -b $(OUT)/eboot.bin \
		--add /usr/local/vitasdk/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys/icon0.png=sce_sys/icon0.png \
		--add /usr/local/vitasdk/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
		--add /usr/local/vitasdk/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
		--add /usr/local/vitasdk/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
		$(OUT)/snoot.vpk

$(OUT)/eboot.bin: $(OUT)/snoot.velf
	vita-make-fself $(OUT)/snoot.velf $(OUT)/eboot.bin

$(OUT)/param.sfo:
	vita-mksfoex -s TITLE_ID="$(PROJECT_TITLEID)" "$(PROJECT_TITLE)" $(OUT)/param.sfo

$(OUT)/snoot.velf: $(OUT)/snoot.elf
	$(STRIP) -g $(OUT)/snoot.elf
	vita-elf-create $(OUT)/snoot.elf $(OUT)/snoot.velf

$(OUT)/snoot.elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $(OUT)/snoot.elf

$(OUT)/obj/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OUT)/snoot.elf $(OUT)/snoot.velf $(OUT)/eboot.bin $(OUT)/param.sfo $(OUT)/snoot.vpk $(OBJS)
