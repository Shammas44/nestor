# --- Expected project structure ---
# .
# ├── bin
# │   ├── test_runner
# │   ├── main
# │   ├── lib
# │   │   ├── libx.a
# │   │   └── libx.so
# │   └── obj
# │       └── x.o
# ├── dev
# ├── lib
# ├── main.c
# ├── Makefile
# ├── src
# │   ├── x.c
# │   └── include
# │       └── x.h
# └── tests
#     └── x.test.c

# --- Project Configuration ---
-include .env
# Available env variable are:
# - PROJECT_NAME= name
# - USER_SHARED_LIBS= lib1 lib2 lib3

# --- Build Tools ---
CC := gcc
AR := ar
# Check the operating system to set correct linker flags for shared libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin)
    # macOS linker flags
    SHARED_LDFLAGS := -Wl,-install_name,/usr/local/lib/lib$(PROJECT_NAME).so
else
    # Linux linker flags
    SHARED_LDFLAGS := -Wl,-soname,lib$(PROJECT_NAME).so
endif

# --- Build Options ---
BASE_CFLAGS := -Wall -Wextra -Werror -fvisibility=hidden -include src/include/jsonv_compat.h
ifeq ($(OPTION), prod)
  CFLAGS := $(BASE_CFLAGS) -O2
else ifeq ($(OPTION), dev)
  CFLAGS := $(BASE_CFLAGS) -g
else ifeq ($(OPTION), test)
  CFLAGS := $(BASE_CFLAGS) -g -Wno-implicit-function-declaration -fPIC
else
  CFLAGS := $(BASE_CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fPIC
endif

# --- Directories ---
SRC_DIR := src
BIN_DIR := bin
OBJ_DIR := $(BIN_DIR)/obj
LIB_DIR := $(BIN_DIR)/lib
TEST_DIR := tests

PREFIX := /usr/local
INSTALL_LIB_DIR := $(PREFIX)/lib
INSTALL_INCLUDE_DIR := $(PREFIX)/include/$(PROJECT_NAME)

# --- Source Files and Objects ---
SRC_FILES := $(shell find $(SRC_DIR) -type f -name "*.c")
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))
TEST_SRC_FILES := $(wildcard $(TEST_DIR)/*.c)
TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test_%.o,$(TEST_SRC_FILES))

# Split into Unit and E2E sources & objects
UNIT_TEST_SRC_FILES := $(filter-out $(TEST_DIR)/examples.test.c, $(TEST_SRC_FILES))
E2E_TEST_SRC_FILES := $(TEST_DIR)/examples.test.c $(TEST_DIR)/testutils.c
UNIT_TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test_%.o,$(UNIT_TEST_SRC_FILES))
E2E_TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test_%.o,$(E2E_TEST_SRC_FILES))


# --- Libraries ---
TEST_LIBS := criterion
LINK_USER_SHARED_LIBS := $(patsubst %, -l%, $(USER_SHARED_LIBS))
LINK_TEST_LIBS := $(patsubst %, -l%, $(TEST_LIBS))
# Corrected find commands to search within the lib directory
STATIC_LIB_BIN_PATHS := $(shell find lib -maxdepth 2 -type d -name "bin")
STATIC_LIB_INCLUDE_PATHS := $(shell find lib -maxdepth 2 -type d -name "include")
LDFLAGS := -L $(LIB_DIR) -L/usr/local/lib $(patsubst %, -L%, $(STATIC_LIB_BIN_PATHS)) -Wl,-rpath,/usr/local/lib
LDLIBS := -l$(PROJECT_NAME) $(LINK_USER_SHARED_LIBS)

ifneq ($(NESTOR_DIR),)
    LDFLAGS += -L$(NESTOR_DIR)/bin/lib
endif

SRC_INCLUDE_PATHS := $(shell find $(SRC_DIR) -type d)
INC_FLAGS := $(patsubst %, -I%, $(SRC_INCLUDE_PATHS)) -I/usr/local/include $(patsubst %, -I%, $(STATIC_LIB_INCLUDE_PATHS))

ifneq ($(NESTOR_DIR),)
    NESTOR_SRC_INCLUDE_PATHS := $(shell find $(NESTOR_DIR)/src -type d)
    INC_FLAGS += $(patsubst %, -I%, $(NESTOR_SRC_INCLUDE_PATHS))
endif

MAIN_APP_STATIC := $(BIN_DIR)/main
MAIN_APP_DYNAMIC := $(BIN_DIR)/main_d
TEST_APP_UNIT := $(BIN_DIR)/test_runner_unit
TEST_APP_E2E := $(BIN_DIR)/test_runner_e2e

# --- Phony Targets ---
.PHONY: all static shared test test_unit test_e2e main_d run run_test run_test_unit run_test_e2e clean install uninstall bear dirs main run_d inspect

# --- Main Targets ---
all: static

bear: clean dirs
	@echo "Generating compile_commands.json..."
	@bear -- $(MAKE) all
	@echo "compile_commands.json generated."

# --- Directories ---
dirs:
	@mkdir -p $(OBJ_DIR) $(LIB_DIR) $(BIN_DIR)

# --- Build Static Library ---
static: $(LIB_DIR)/lib$(PROJECT_NAME).a
$(LIB_DIR)/lib$(PROJECT_NAME).a: $(OBJS) | dirs
	@echo "[AR] $@"
	@$(AR) rcs $@ $^

# --- Build Shared Library ---
shared: $(LIB_DIR)/lib$(PROJECT_NAME).so
$(LIB_DIR)/lib$(PROJECT_NAME).so: $(OBJS) | dirs
	@echo "[CC-shared] $@"
	@$(CC) -shared $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) $(SHARED_LDFLAGS)

# --- Main Executable (Dynamic Link) ---
main_d: $(MAIN_APP_DYNAMIC)
$(MAIN_APP_DYNAMIC): $(OBJ_DIR)/main.o $(LIB_DIR)/lib$(PROJECT_NAME).so | dirs
	@echo "[CC] Linking DYNAMIC$@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) -Wl,-rpath,$(INSTALL_LIB_DIR)

# --- Main Executable (Static Link) ---
main: static $(MAIN_APP_STATIC) # Ensure the static library is built first
$(MAIN_APP_STATIC): $(OBJ_DIR)/main.o $(LIB_DIR)/lib$(PROJECT_NAME).a | dirs
	@echo "[CC] Linking STATIC $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LINK_USER_SHARED_LIBS)

# --- Test Executables ---
test_unit: static plugins/mock_plugin $(TEST_APP_UNIT)
$(TEST_APP_UNIT): $(UNIT_TEST_OBJS) $(LIB_DIR)/lib$(PROJECT_NAME).a | dirs
	@echo "[CC] Linking $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LINK_TEST_LIBS) $(LINK_USER_SHARED_LIBS)

test_e2e: static plugins/mock_plugin $(TEST_APP_E2E)
$(TEST_APP_E2E): $(E2E_TEST_OBJS) $(LIB_DIR)/lib$(PROJECT_NAME).a | dirs
	@echo "[CC] Linking $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LINK_TEST_LIBS) $(LINK_USER_SHARED_LIBS)

plugins/mock_plugin: tests/fixtures/mock_plugin.c
	@mkdir -p plugins
	@$(CC) -O2 $< -o $@

test: test_unit test_e2e

# --- Compile Rules ---
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) $(INC_FLAGS) -c $< -o $@

$(OBJ_DIR)/main.o: main.c | dirs
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) $(INC_FLAGS) -c $< -o $@

$(OBJ_DIR)/test_%.o: $(TEST_DIR)/%.c | dirs
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) $(INC_FLAGS) -c $< -o $@

# --- Install/Uninstall ---
install: static shared
	@mkdir -p $(INSTALL_LIB_DIR) $(INSTALL_INCLUDE_DIR)
	@cp $(LIB_DIR)/*.a $(LIB_DIR)/*.so $(INSTALL_LIB_DIR)/
	@cp -R $(SRC_DIR)/include/* $(INSTALL_INCLUDE_DIR)/ | true
	@echo "Installed to $(PREFIX)"

uninstall:
	@rm -f $(INSTALL_LIB_DIR)/lib$(PROJECT_NAME).a
	@rm -f $(INSTALL_LIB_DIR)/lib$(PROJECT_NAME).so
	@rm -rf $(INSTALL_INCLUDE_DIR)
	@echo "Uninstalled from $(PREFIX)"

# --- Run Targets ---
run: $(MAIN_APP_STATIC)
	@MallocNanoZone=0 $(MAIN_APP_STATIC) $(ARGS)

run_d: $(MAIN_APP_DYNAMIC)
	@MallocNanoZone=0 $(MAIN_APP_DYNAMIC)

run_test_unit: $(TEST_APP_UNIT)
	@MallocNanoZone=0 $(TEST_APP_UNIT) || true

run_test_e2e: $(TEST_APP_E2E)
	@MallocNanoZone=0 $(TEST_APP_E2E) || true

run_test: run_test_unit run_test_e2e

inspect:
	@echo "Inspect exposed symbols"
	@nm -gU $(LIB_DIR)/lib$(PROJECT_NAME).so

# --- Clean Targets ---
clean:
	@echo "Clean targets"
	@rm -rf $(BIN_DIR)
	@rm -f plugins/mock_plugin
