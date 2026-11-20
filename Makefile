CXX ?= c++
CPPFLAGS += -Isrc
CXXFLAGS ?= -O2 -g0
CXXFLAGS += -std=c++17 -fPIC -Wall -Wextra -Wpedantic -MMD -MP
BUILD := build
OBJECTS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(wildcard src/*.cpp))
MODULES := $(filter-out $(BUILD)/main.o,$(OBJECTS))

# Use the system Qt and QTermWidget by default.
# The unprivileged cloud installer records its local sysroot here.
-include $(BUILD)/sdk.mk
ifdef INTERACT_SDK
MULTIARCH := $(shell $(CXX) -print-multiarch)
SDK_LIB := $(INTERACT_SDK)/usr/lib/$(MULTIARCH)
QT_PKG := PKG_CONFIG_SYSROOT_DIR=$(INTERACT_SDK) PKG_CONFIG_LIBDIR=$(SDK_LIB)/pkgconfig pkg-config
CPPFLAGS += -I$(INTERACT_SDK)/usr/include $(shell $(QT_PKG) --cflags Qt5Widgets)
LDFLAGS += -L$(SDK_LIB) -Wl,--disable-new-dtags,-rpath,$(SDK_LIB)
LDLIBS += -lqtermwidget5 $(shell $(QT_PKG) --libs Qt5Widgets)
else
CPPFLAGS += $(shell pkg-config --cflags Qt5Widgets)
LDLIBS += -lqtermwidget5 $(shell pkg-config --libs Qt5Widgets)
endif

.PHONY: all check bench
all: $(BUILD)/interact-studio

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test-%.o: tests/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/interact-studio: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

TESTS := smoke editor_test terminal_test workspace_test customize_test buildrun_test definition_test
TEST_PROGRAMS := $(addprefix $(BUILD)/,$(TESTS))

$(BUILD)/smoke: $(MODULES)
$(BUILD)/editor_test $(BUILD)/editor_bench: $(BUILD)/editor.o
$(BUILD)/terminal_test: $(BUILD)/terminal.o
$(BUILD)/workspace_test: $(BUILD)/workspace.o $(BUILD)/quickopen.o
$(BUILD)/customize_test: $(BUILD)/customize.o
$(BUILD)/customize_test: LDLIBS += -lQt5Test
$(BUILD)/buildrun_test: $(BUILD)/buildrun.o
$(BUILD)/definition_test: $(BUILD)/definition.o

$(TEST_PROGRAMS) $(BUILD)/editor_bench: $(BUILD)/%: $(BUILD)/test-%.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

bench: $(BUILD)/editor_bench
	QT_QPA_PLATFORM=offscreen ./$(BUILD)/editor_bench

check: $(TEST_PROGRAMS)
	@set -e; for program in $(TEST_PROGRAMS); do QT_QPA_PLATFORM=offscreen ./$$program; done

-include $(wildcard $(BUILD)/*.d)
