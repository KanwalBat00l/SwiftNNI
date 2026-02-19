CXX = g++
CXXFLAGS = -std=c++17 -I./include -Wall -Wextra -O3
LDFLAGS = -lpthread -lstdc++fs

SRC_DIR = src
OBJ_DIR = obj

# Common logic used by both Server and Client
COMMON_OBJS = $(OBJ_DIR)/ConfigManager.o $(OBJ_DIR)/StringUtils.o

# Server engine components (Schedulers are header-only)
SERVER_OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/KairosServer.o $(OBJ_DIR)/ResourceManager.o \
              $(OBJ_DIR)/Logger.o $(OBJ_DIR)/ProcessLauncher.o $(OBJ_DIR)/SystemMonitor.o \
              $(OBJ_DIR)/FileManager.o

# Client application
CLIENT_OBJS = $(OBJ_DIR)/Kairos_client.o

all: Kairos_server Kairos_client

# Link Server
Kairos_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Link Client
Kairos_client: $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Compile C++ files to Objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Test Binary
TEST_BIN = test_schedulers
TEST_SRCS = $(SRC_DIR)/test_schedulers.cpp $(SRC_DIR)/ConfigManager.cpp \
            $(SRC_DIR)/StringUtils.cpp $(SRC_DIR)/SystemMonitor.cpp \
            $(SRC_DIR)/FileManager.cpp $(SRC_DIR)/ResourceManager.cpp \
            $(SRC_DIR)/Logger.cpp

test: $(TEST_BIN)
	@echo "Running test suite..."
	@./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) Kairos_server Kairos_client $(TEST_BIN)