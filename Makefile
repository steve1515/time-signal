SRC_DIR   := .
INC_DIR   := $(SRC_DIR)
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
TARGET    := $(BUILD_DIR)/time-signal

CC        := gcc
CPPFLAGS  := -I$(INC_DIR) -MMD -MP
CFLAGS    := -Wall -O2
LDFLAGS   := -s -no-pie
LDLIBS    := 

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

clean:
	@$(RM) -rv $(OBJ_DIR)

cleanall: clean
	@$(RM) -rv $(BUILD_DIR)

-include $(OBJ:.o=.d)
