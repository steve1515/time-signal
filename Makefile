EXE_NAME    := time-signal
SRC_DIR     := .
INC_DIR     := $(SRC_DIR)
BUILD_DIR   := build
OBJ_DIR     := $(BUILD_DIR)/obj
TARGET      := $(BUILD_DIR)/$(EXE_NAME)
INSTALL_DIR := /usr/local/bin

CC          := gcc
CPPFLAGS    := -I$(INC_DIR) -MMD -MP
CFLAGS      := -Wall -O2
LDFLAGS     := -s -no-pie
LDLIBS      := 

SRC := $(wildcard $(SRC_DIR)/*.c)
OBJ := $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean cleanall

all: $(TARGET)

$(TARGET): $(OBJ) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

install:
	@install -m755 -v $(TARGET) $(INSTALL_DIR)/$(EXE_NAME)
	@install -m644 -v $(SRC_DIR)/$(EXE_NAME).service /etc/systemd/system
	@systemctl --no-ask-password daemon-reload
	@systemctl --no-ask-password enable $(EXE_NAME).service
	@systemctl --no-ask-password start $(EXE_NAME).service

uninstall:
	@systemctl --no-ask-password stop $(EXE_NAME).service
	@systemctl --no-ask-password disable $(EXE_NAME).service
	@$(RM) -v /etc/systemd/system/$(EXE_NAME).service
	@systemctl --no-ask-password daemon-reload
	@systemctl --no-ask-password reset-failed
	@$(RM) -v $(INSTALL_DIR)/$(EXE_NAME)

clean:
	@$(RM) -rv $(OBJ_DIR)

cleanall: clean
	@$(RM) -rv $(BUILD_DIR)

-include $(OBJ:.o=.d)
