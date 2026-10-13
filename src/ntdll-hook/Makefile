CC = x86_64-w64-mingw32-gcc
CC_I386 = i686-w64-mingw32-gcc
WINE_ROOT = $(HOME)/.metalsharp/runtime/wine
WINE_LIBS = $(WINE_ROOT)/lib/wine/x86_64-windows
WINE_LIBS_I386 = $(WINE_ROOT)/lib/wine/i386-windows
WINEBUILD = $(WINE_ROOT)/bin/winebuild
CFLAGS = -Wall -Wextra -O2 -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE
LDFLAGS = -shared -static-libgcc -static-libstdc++ \
	-L$(WINE_LIBS)
LDFLAGS_I386 = -shared -static-libgcc -static-libstdc++ \
	-L$(WINE_LIBS_I386)
TARGET = metalsharp_ntdll_hook.dll
TARGET_RAW = metalsharp_ntdll_hook.dll.raw
I386_DIR = build-i386
I386_TARGET = $(I386_DIR)/metalsharp_ntdll_hook.dll
I386_TARGET_RAW = $(I386_DIR)/metalsharp_ntdll_hook.dll.raw

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
I386_OBJS = $(patsubst src/%.c,$(I386_DIR)/src/%.o,$(SRCS))

all: $(TARGET) i386

$(TARGET_RAW): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lkernel32

$(TARGET): $(TARGET_RAW)
	cp $< $@
	$(WINEBUILD) --builtin $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 32-bit build: hosts the MetalFX live-toggle poller for 32-bit games.
# Same source as x64: DllMain runs the metalfx poller.
i386: $(I386_TARGET)

$(I386_TARGET_RAW): $(I386_OBJS)
	@mkdir -p $(I386_DIR)
	$(CC_I386) $(LDFLAGS_I386) -o $@ $^ -lkernel32

$(I386_TARGET): $(I386_TARGET_RAW)
	cp $< $@
	$(WINEBUILD) --builtin $@

$(I386_DIR)/src/%.o: src/%.c
	@mkdir -p $(I386_DIR)/src
	$(CC_I386) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(TARGET_RAW) src/*.o
	rm -rf $(I386_DIR)

.PHONY: all clean i386
