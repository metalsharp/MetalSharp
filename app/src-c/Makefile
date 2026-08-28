CC ?= clang
PROJECT_ROOT := $(abspath ../..)
BUILD_DIR ?= $(CURDIR)/build
TARGET ?= $(BUILD_DIR)/metalsharp-backend
INSTALL_TARGET ?= $(TARGET)
VERSION ?= 0.60.0
JSON_TEST ?= $(BUILD_DIR)/json-test
MIGRATION_TEST ?= $(BUILD_DIR)/migration-test
HTTP_SERVER_TEST ?= $(BUILD_DIR)/http-server-test

CFLAGS ?= -std=c11 -O2 -g -D_POSIX_C_SOURCE=200809L
CFLAGS += -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong
CPPFLAGS += -I$(CURDIR)/include -DMS_BACKEND_VERSION=\"$(VERSION)\"
LDFLAGS ?=

SOURCES := \
	runtime/backend.c \
	runtime/config.c \
	runtime/cache.c \
	runtime/logs.c \
	runtime/metalfx.c \
	runtime/mtsp.c \
	runtime/mtsp_catalog.c \
	runtime/setup.c \
	runtime/scan.c \
	runtime/steam.c \
	runtime/updater.c \
	runtime/kernel_handles.c \
	runtime/kernel_apc.c \
	runtime/kernel_integrity.c \
	runtime/kernel_driver.c \
	runtime/process.c \
	runtime/steam_actions.c \
	runtime/bottles.c \
	runtime/mono.c \
	runtime/emulators.c \
	runtime/pcsx2.c \
	runtime/rpcs3.c \
	runtime/shadps4.c \
	runtime/sharpemu.c \
	runtime/sharp.c \
	runtime/epic.c \
	runtime/gamejolt.c \
	runtime/gog.c \
	runtime/game.c \
	runtime/diagnostics.c \
	runtime/antidebug.c \
	runtime/kernel_extra.c \
	runtime/bottle_actions.c \
	runtime/ob_callbacks.c \
	runtime/es_bridge.c \
	runtime/thread.c \
	runtime/integration.c \
	runtime/d3dmetal.c \
	runtime/migration.c \
	runtime/http_server.c \
	runtime/json.c \
	runtime/json_writer.c \
	runtime/main.c
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/c-backend/%.o)

.PHONY: all clean install test asan-test

all: $(TARGET)

install: $(TARGET)
	@if [ "$(TARGET)" != "$(INSTALL_TARGET)" ]; then \
		mkdir -p "$(dir $(INSTALL_TARGET))"; \
		cp "$(TARGET)" "$(INSTALL_TARGET)"; \
	fi
	chmod 755 "$(INSTALL_TARGET)"

$(TARGET): $(OBJECTS)
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $(OBJECTS)
	@chmod 755 "$@"

$(BUILD_DIR)/c-backend/%.o: %.c
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"

-include $(OBJECTS:.o=.d)

test: $(TARGET) $(JSON_TEST) $(MIGRATION_TEST) $(HTTP_SERVER_TEST)
	@ms_home="$$(mktemp -d)"; test_home="$$(mktemp -d)"; test_port=$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'); HOME="$$test_home" METALSHARP_PORT="$$test_port" METALSHARP_HOME="$$ms_home" METALSHARP_RPCS3_RELEASE_JSON="$(CURDIR)/tests/rpcs3_release.json" METALSHARP_RPCS3_DOWNLOAD_FILE="$(CURDIR)/tests/rpcs3_bad_archive.7z" METALSHARP_SHADPS4_RELEASE_JSON="$(CURDIR)/tests/shadps4_release.json" METALSHARP_SHADPS4_DOWNLOAD_FILE="$(CURDIR)/tests/shadps4_bad_archive.zip" METALSHARP_SHADPS4_HOST_ARCH=arm64 METALSHARP_SHADPS4_HOST_MACOS=27 METALSHARP_SHADPS4_ROSETTA=1 METALSHARP_PCSX2_RELEASE_JSON="$(CURDIR)/tests/pcsx2_release.json" METALSHARP_PCSX2_DOWNLOAD_FILE="$(CURDIR)/tests/pcsx2_bad_archive.tar.xz" METALSHARP_PCSX2_HOST_ARCH=arm64 METALSHARP_PCSX2_HOST_MACOS=27 METALSHARP_PCSX2_ROSETTA=1 METALSHARP_PCSX2_SKIP_SIGNATURE_FOR_TESTS=1 METALSHARP_SHARPEMU_HOST_ARCH=arm64 METALSHARP_SHARPEMU_HOST_MACOS=27 METALSHARP_SHARPEMU_ROSETTA=1 METALSHARP_SHARPEMU_SANDBOX=1 "$(CURDIR)/tests/smoke.sh" "$(TARGET)"; rc=$$?; rm -rf "$$test_home" "$$ms_home"; exit $$rc
	@"$(JSON_TEST)"
	@"$(MIGRATION_TEST)"
	@"$(HTTP_SERVER_TEST)"
	@python3 "$(CURDIR)/tests/updater_test.py" "$(TARGET)"
	@python3 "$(CURDIR)/tests/shadps4_update_test.py" "$(TARGET)"
	@python3 "$(CURDIR)/tests/pcsx2_update_test.py" "$(TARGET)"
	@python3 "$(CURDIR)/tests/sharpemu_update_test.py" "$(TARGET)"

asan-test:
	@set -eu; trap 'rm -rf "$(CURDIR)/build-asan"' EXIT INT TERM; \
	$(MAKE) BUILD_DIR="$(CURDIR)/build-asan" TARGET="$(CURDIR)/build-asan/metalsharp-backend" \
		JSON_TEST="$(CURDIR)/build-asan/json-test" \
		CFLAGS='-std=c11 -O1 -g -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer -fsanitize=address,undefined' all; \
	attempt=1; while [ $$attempt -le 3 ]; do \
		ms_home="$$(mktemp -d)"; test_home="$$(mktemp -d)"; \
		test_port=$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'); \
		if ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 HOME="$$test_home" METALSHARP_PORT="$$test_port" \
			METALSHARP_HOME="$$ms_home" METALSHARP_RPCS3_RELEASE_JSON="$(CURDIR)/tests/rpcs3_release.json" \
			METALSHARP_RPCS3_DOWNLOAD_FILE="$(CURDIR)/tests/rpcs3_bad_archive.7z" \
			METALSHARP_SHADPS4_RELEASE_JSON="$(CURDIR)/tests/shadps4_release.json" \
			METALSHARP_SHADPS4_DOWNLOAD_FILE="$(CURDIR)/tests/shadps4_bad_archive.zip" \
			METALSHARP_SHADPS4_HOST_ARCH=arm64 METALSHARP_SHADPS4_HOST_MACOS=27 METALSHARP_SHADPS4_ROSETTA=1 \
			METALSHARP_PCSX2_RELEASE_JSON="$(CURDIR)/tests/pcsx2_release.json" \
			METALSHARP_PCSX2_DOWNLOAD_FILE="$(CURDIR)/tests/pcsx2_bad_archive.tar.xz" \
			METALSHARP_PCSX2_HOST_ARCH=arm64 METALSHARP_PCSX2_HOST_MACOS=27 METALSHARP_PCSX2_ROSETTA=1 \
			METALSHARP_PCSX2_SKIP_SIGNATURE_FOR_TESTS=1 \
			METALSHARP_SHARPEMU_HOST_ARCH=arm64 METALSHARP_SHARPEMU_HOST_MACOS=27 \
			METALSHARP_SHARPEMU_ROSETTA=1 METALSHARP_SHARPEMU_SANDBOX=1 \
			"$(CURDIR)/tests/smoke.sh" "$(CURDIR)/build-asan/metalsharp-backend"; then \
			ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 "$(CURDIR)/tests/updater_test.py" \
				"$(CURDIR)/build-asan/metalsharp-backend" && \
			ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 "$(CURDIR)/tests/pcsx2_update_test.py" \
				"$(CURDIR)/build-asan/metalsharp-backend" && \
			ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 "$(CURDIR)/tests/sharpemu_update_test.py" \
				"$(CURDIR)/build-asan/metalsharp-backend" && exit 0; \
		fi; \
		rm -rf "$$test_home" "$$ms_home"; \
		[ $$attempt -eq 3 ] && exit 1; attempt=$$((attempt + 1)); \
	done

$(JSON_TEST): runtime/json.c runtime/json_writer.c tests/json_test.c include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c tests/json_test.c

$(MIGRATION_TEST): runtime/json.c runtime/json_writer.c runtime/migration.c tests/migration_test.c include/metalsharp_backend/migration.h include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c tests/migration_test.c -lpthread

$(HTTP_SERVER_TEST): runtime/http_server.c tests/http_server_test.c include/metalsharp_backend/http_server.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/http_server.c tests/http_server_test.c

clean:
	rm -rf "$(BUILD_DIR)"
