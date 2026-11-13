CC ?= clang
MACOSX_DEPLOYMENT_TARGET ?= 14.0
PROJECT_ROOT := $(abspath ../..)
BUILD_DIR ?= $(CURDIR)/build
TARGET ?= $(BUILD_DIR)/metalsharp-backend
INSTALL_TARGET ?= $(TARGET)
VERSION ?= 1.1.0
# regular: the x86_64 Wine runtime through Rosetta; fex: MetalSharp Wine (arm64, FEX)
VARIANT ?= regular
JSON_TEST ?= $(BUILD_DIR)/json-test
MIGRATION_TEST ?= $(BUILD_DIR)/migration-test
VCPP_TEST ?= $(BUILD_DIR)/setup-vcpp-test
RUNTIME_READY_SIM ?= $(BUILD_DIR)/migration-runtime-ready-sim
HTTP_SERVER_TEST ?= $(BUILD_DIR)/http-server-test
GAME_EXE_TEST ?= $(BUILD_DIR)/game-executable-test
WINE_RUNTIME_TEST ?= $(BUILD_DIR)/wine-runtime-test
MTSP_RULES_TEST ?= $(BUILD_DIR)/mtsp-rules-test
SETUP_VARIANT_TEST ?= $(BUILD_DIR)/setup-variant-test
STORE_LAUNCHERS_TEST ?= $(BUILD_DIR)/store-launchers-test
LIBRARY_LOCATIONS_TEST ?= $(BUILD_DIR)/library-locations-test
LAUNCHER_UNINSTALL_TEST ?= $(BUILD_DIR)/launcher-uninstall-test
LINUX_STEAM_TEST ?= $(BUILD_DIR)/linux-steam-test
# A MetalSharp-Wine.app for the fex facade test; without one it checks only the mapping.
METALSHARP_WINE_APP ?=

CFLAGS ?= -std=c11 -O2 -g -D_POSIX_C_SOURCE=200809L
CFLAGS += -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong
ifeq ($(shell uname -s),Darwin)
CFLAGS += -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
LDFLAGS += -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
endif
CPPFLAGS += -I$(CURDIR)/include -DMS_BACKEND_VERSION=\"$(VERSION)\" -DMS_BACKEND_VARIANT=\"$(VARIANT)\"
LDFLAGS ?=

SOURCES := \
	runtime/backend.c \
	runtime/config.c \
	runtime/cache.c \
	runtime/logs.c \
	runtime/metalfx.c \
	runtime/frame_rate.c \
	runtime/mtsp.c \
	runtime/mtsp_catalog.c \
	runtime/setup.c \
	runtime/scan.c \
	runtime/steam.c \
	runtime/game_executable.c \
	runtime/updater.c \
	runtime/wine_runtime.c \
	runtime/rosetta.c \
	runtime/kernel_handles.c \
	runtime/kernel_apc.c \
	runtime/kernel_integrity.c \
	runtime/kernel_driver.c \
	runtime/process.c \
	runtime/steam_actions.c \
	runtime/bottles.c \
	runtime/mono.c \
	runtime/sharp.c \
	runtime/epic.c \
	runtime/gamejolt.c \
	runtime/gog.c \
	runtime/steamcmd.c \
	runtime/ubisoft.c \
	runtime/store_launcher.c \
	runtime/library_locations.c \
	runtime/launcher_uninstall.c \
	runtime/linux_steam.c \
	runtime/rockstar.c \
	runtime/battlenet.c \
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
	runtime/streaming.c \
	runtime/http_server.c \
	runtime/local_socket.c \
	runtime/json.c \
	runtime/json_writer.c \
	runtime/main.c
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/c-backend/%.o)

.PHONY: all clean install test test-regular test-fex asan-test

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

$(BUILD_DIR)/runtime-routes-test: tests/runtime_routes_test.c runtime/steam_actions.c runtime/epic.c $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/runtime_routes_test.c $(filter-out $(BUILD_DIR)/c-backend/runtime/main.o $(BUILD_DIR)/c-backend/runtime/steam_actions.o $(BUILD_DIR)/c-backend/runtime/epic.o $(BUILD_DIR)/c-backend/runtime/ubisoft.o,$(OBJECTS))

# rockstar.c and battlenet.c are #included for their static helpers, so their objects are left out.
$(STORE_LAUNCHERS_TEST): tests/store_launchers_test.c runtime/rockstar.c runtime/battlenet.c $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/store_launchers_test.c $(filter-out $(BUILD_DIR)/c-backend/runtime/main.o $(BUILD_DIR)/c-backend/runtime/rockstar.o $(BUILD_DIR)/c-backend/runtime/battlenet.o,$(OBJECTS))

# The backend whole (without main), on a fake home with a script standing in for Linux Steam's metalsharp-steam.
$(LINUX_STEAM_TEST): tests/linux_steam_test.c $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/linux_steam_test.c $(filter-out $(BUILD_DIR)/c-backend/runtime/main.o,$(OBJECTS))

# The backend whole (without main), on fake homes, prefixes and volumes: no Wine is started.
$(LAUNCHER_UNINSTALL_TEST): tests/launcher_uninstall_test.c $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/launcher_uninstall_test.c $(filter-out $(BUILD_DIR)/c-backend/runtime/main.o,$(OBJECTS))

test: test-$(VARIANT)

# The FEX build's suite (make VARIANT=fex BUILD_DIR=... test): its backend and the tests that hold for
# MetalSharp Wine. The route, smoke, VC++ seeding, runtime-ready and update-download tests describe
# the regular build's x86_64 runtime and run in test-regular.
test-fex: $(TARGET) $(JSON_TEST) $(MIGRATION_TEST) $(HTTP_SERVER_TEST) $(GAME_EXE_TEST) $(WINE_RUNTIME_TEST) $(MTSP_RULES_TEST) $(SETUP_VARIANT_TEST) $(STORE_LAUNCHERS_TEST) $(LIBRARY_LOCATIONS_TEST) $(LAUNCHER_UNINSTALL_TEST) $(LINUX_STEAM_TEST)
	@"$(SETUP_VARIANT_TEST)"
	@home="$$(mktemp -d)"; "$(STORE_LAUNCHERS_TEST)" "$$home"; rc=$$?; rm -rf "$$home"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LIBRARY_LOCATIONS_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LAUNCHER_UNINSTALL_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LINUX_STEAM_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@"$(JSON_TEST)"
	@cd "$(PROJECT_ROOT)" && METALSHARP_HOME= "$(abspath $(MTSP_RULES_TEST))"
	@"$(MIGRATION_TEST)"
	@"$(HTTP_SERVER_TEST)"
	@"$(GAME_EXE_TEST)"
	@ms_home="$$(mktemp -d)"; "$(WINE_RUNTIME_TEST)" "$$ms_home" $(METALSHARP_WINE_APP); rc=$$?; rm -rf "$$ms_home"; exit $$rc
	@bash "$(PROJECT_ROOT)/app/updater/tests/legacy_shutdown_timeout_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/install_progress_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/build_switch_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/stopped_status_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/linux_steam_stop_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@python3 "$(CURDIR)/tests/steam_process_test.py" "$(TARGET)"

test-regular: $(TARGET) $(JSON_TEST) $(MIGRATION_TEST) $(HTTP_SERVER_TEST) $(VCPP_TEST) $(RUNTIME_READY_SIM) $(GAME_EXE_TEST) $(WINE_RUNTIME_TEST) $(MTSP_RULES_TEST) $(SETUP_VARIANT_TEST) $(BUILD_DIR)/runtime-routes-test $(STORE_LAUNCHERS_TEST) $(LIBRARY_LOCATIONS_TEST) $(LAUNCHER_UNINSTALL_TEST) $(LINUX_STEAM_TEST)
	@"$(SETUP_VARIANT_TEST)"
	@home="$$(mktemp -d)"; "$(STORE_LAUNCHERS_TEST)" "$$home"; rc=$$?; rm -rf "$$home"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LIBRARY_LOCATIONS_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LAUNCHER_UNINSTALL_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@dir="$$(mktemp -d)"; "$(LINUX_STEAM_TEST)" "$$dir"; rc=$$?; rm -rf "$$dir"; exit $$rc
	@cd "$(PROJECT_ROOT)" && METALSHARP_HOME= "$(abspath $(MTSP_RULES_TEST))"
	@home="$$(mktemp -d)"; "$(BUILD_DIR)/runtime-routes-test" "$$home"; rc=$$?; rm -rf "$$home"; exit $$rc
	@ms_home="$$(mktemp -d)"; test_home="$$(mktemp -d)"; HOME="$$test_home" METALSHARP_HOME="$$ms_home" "$(CURDIR)/tests/smoke.sh" "$(TARGET)"; rc=$$?; rm -rf "$$test_home" "$$ms_home"; exit $$rc
	@"$(JSON_TEST)"
	@"$(MIGRATION_TEST)"
	@bash "$(CURDIR)/tests/runtime-ready-sim.sh" "$(RUNTIME_READY_SIM)"; rc=$$?; if [ $$rc -eq 97 ]; then echo "runtime-ready sim skipped"; else exit $$rc; fi
	@ms_home="$$(mktemp -d)"; "$(VCPP_TEST)" $$ms_home; rc=$$?; rm -rf "$$ms_home"; exit $$rc
	@"$(HTTP_SERVER_TEST)"
	@"$(GAME_EXE_TEST)"
	@ms_home="$$(mktemp -d)"; "$(WINE_RUNTIME_TEST)" "$$ms_home" $(METALSHARP_WINE_APP); rc=$$?; rm -rf "$$ms_home"; exit $$rc
	@python3 "$(CURDIR)/tests/updater_test.py" "$(TARGET)"
	@bash "$(PROJECT_ROOT)/app/updater/tests/legacy_shutdown_timeout_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/install_progress_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/build_switch_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/stopped_status_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@bash "$(PROJECT_ROOT)/app/updater/tests/linux_steam_stop_test.sh" "$(PROJECT_ROOT)/app/updater/update.sh"
	@python3 "$(CURDIR)/tests/steam_process_test.py" "$(TARGET)"

asan-test:
	@set -eu; python3 "$(CURDIR)/tests/migration_hash_contract_test.py"; \
	trap 'rm -rf "$(CURDIR)/build-asan"' EXIT INT TERM; \
	$(MAKE) BUILD_DIR="$(CURDIR)/build-asan" TARGET="$(CURDIR)/build-asan/metalsharp-backend" \
		JSON_TEST="$(CURDIR)/build-asan/json-test" \
		CFLAGS='-std=c11 -O1 -g -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer -fsanitize=address,undefined' all; \
	attempt=1; while [ $$attempt -le 3 ]; do \
		ms_home="$$(mktemp -d)"; test_home="$$(mktemp -d)"; \
		if ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 HOME="$$test_home" \
			METALSHARP_HOME="$$ms_home" \
			"$(CURDIR)/tests/smoke.sh" "$(CURDIR)/build-asan/metalsharp-backend"; then \
			ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 "$(CURDIR)/tests/updater_test.py" \
				"$(CURDIR)/build-asan/metalsharp-backend" && \
			ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 "$(CURDIR)/tests/steam_process_test.py" \
				"$(CURDIR)/build-asan/metalsharp-backend" && exit 0; \
		fi; \
		rm -rf "$$test_home" "$$ms_home"; \
		[ $$attempt -eq 3 ] && exit 1; attempt=$$((attempt + 1)); \
	done

# Volumes come from a folder standing in for /Volumes, and HOME from the test's own folder.
$(LIBRARY_LOCATIONS_TEST): runtime/library_locations.c runtime/json.c runtime/json_writer.c tests/library_locations_test.c include/metalsharp_backend/library_locations.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/library_locations.c runtime/json.c runtime/json_writer.c tests/library_locations_test.c

$(JSON_TEST): runtime/json.c runtime/json_writer.c tests/json_test.c include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c tests/json_test.c

$(MIGRATION_TEST): runtime/json.c runtime/json_writer.c runtime/migration.c tests/migration_test.c include/metalsharp_backend/migration.h include/metalsharp_backend/steam_actions.h include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c runtime/logs.c runtime/wine_runtime.c tests/migration_test.c -lpthread

$(HTTP_SERVER_TEST): runtime/http_server.c runtime/local_socket.c tests/http_server_test.c include/metalsharp_backend/http_server.h include/metalsharp_backend/local_socket.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/http_server.c runtime/local_socket.c tests/http_server_test.c

$(GAME_EXE_TEST): runtime/game_executable.c tests/game_executable_test.c include/metalsharp_backend/game_executable.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/game_executable.c tests/game_executable_test.c

# setup.c and migration.c together, as this VARIANT builds them (both #included, like the vcpp test).
$(SETUP_VARIANT_TEST): runtime/setup.c runtime/migration.c runtime/json.c runtime/json_writer.c runtime/logs.c runtime/rosetta.c runtime/wine_runtime.c tests/setup_variant_test.c include/metalsharp_backend/setup.h include/metalsharp_backend/migration.h include/metalsharp_backend/wine_runtime.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c runtime/logs.c runtime/rosetta.c runtime/wine_runtime.c tests/setup_variant_test.c -lpthread

# This build's game rule set (configs/mtsp-rules.toml, or mtsp-rules-fex.toml with VARIANT=fex).
$(MTSP_RULES_TEST): runtime/mtsp.c runtime/mtsp_catalog.c runtime/json.c runtime/json_writer.c runtime/wine_runtime.c tests/mtsp_rules_test.c include/metalsharp_backend/mtsp.h $(PROJECT_ROOT)/configs/mtsp-rules.toml $(PROJECT_ROOT)/configs/mtsp-rules-fex.toml
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/mtsp.c runtime/mtsp_catalog.c runtime/json.c runtime/json_writer.c runtime/wine_runtime.c tests/mtsp_rules_test.c -lpthread

# The fex build's code, whatever VARIANT this build is.
$(WINE_RUNTIME_TEST): runtime/wine_runtime.c runtime/cache.c runtime/metalfx.c tests/wine_runtime_test.c include/metalsharp_backend/wine_runtime.h include/metalsharp_backend/cache.h include/metalsharp_backend/metalfx.h
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -DMS_BACKEND_VARIANT=%,$(CPPFLAGS)) -DMS_BACKEND_VARIANT=\"fex\" $(CFLAGS) -o "$@" runtime/wine_runtime.c runtime/cache.c runtime/metalfx.c runtime/json.c runtime/json_writer.c tests/wine_runtime_test.c

# The vcpp test #includes runtime/setup.c for the static progress helpers, so
# setup.c itself must not be linked into this binary.
$(VCPP_TEST): runtime/json.c runtime/json_writer.c runtime/logs.c runtime/rosetta.c runtime/wine_runtime.c tests/setup_vcpp_test.c runtime/setup.c include/metalsharp_backend/setup.h include/metalsharp_backend/rosetta.h include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c runtime/logs.c runtime/rosetta.c runtime/wine_runtime.c tests/setup_vcpp_test.c -lpthread

# The runtime-ready sim #includes runtime/migration.c for runtime_ready();
# migration.c must not be linked into this binary (same pattern as the vcpp
# test with setup.c).
$(RUNTIME_READY_SIM): runtime/json.c runtime/json_writer.c runtime/migration.c tests/migration_runtime_ready_sim.c include/metalsharp_backend/migration.h include/metalsharp_backend/steam_actions.h include/metalsharp_backend/json.h include/metalsharp_backend/json_writer.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" runtime/json.c runtime/json_writer.c runtime/logs.c runtime/wine_runtime.c tests/migration_runtime_ready_sim.c -lpthread

clean:
	rm -rf "$(BUILD_DIR)"
