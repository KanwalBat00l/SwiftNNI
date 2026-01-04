CXX = g++
CXXFLAGS = -std=c++17 -I./include -Wall -Wextra -O3
LDFLAGS = -lpthread -lstdc++fs

# Directories
SRC_DIR = src
OBJ_DIR = obj

# Common Objects shared by both Server and Client
COMMON_OBJS = $(OBJ_DIR)/ConfigManager.o $(OBJ_DIR)/StringUtils.o

# Server specific objects
SERVER_OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/SAPPISServer.o $(OBJ_DIR)/ResourceManager.o \
              $(OBJ_DIR)/Logger.o $(OBJ_DIR)/ProcessLauncher.o $(OBJ_DIR)/SystemMonitor.o \
              $(OBJ_DIR)/SJFScheduler.o $(OBJ_DIR)/FileManager.o

# Client specific objects
CLIENT_OBJS = $(OBJ_DIR)/sappis_client.o

all: sappis_server sappis_client

# Build Server
sappis_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Build Client
sappis_client: $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Pattern rule for objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) sappis_server sappis_client sappis_profiler test_hw