CXX = g++
CXXFLAGS = -std=c++17 -I./include -Wall -Wextra -O3
LDFLAGS = -lpthread -lstdc++fs

SRC_DIR = src
OBJ_DIR = obj

# Common logic used by both Server and Client
COMMON_OBJS = $(OBJ_DIR)/ConfigManager.o $(OBJ_DIR)/StringUtils.o

# Server engine components (Schedulers are header-only, so they are NOT listed here)
SERVER_OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/SAPPISServer.o $(OBJ_DIR)/ResourceManager.o \
              $(OBJ_DIR)/Logger.o $(OBJ_DIR)/ProcessLauncher.o $(OBJ_DIR)/SystemMonitor.o \
              $(OBJ_DIR)/FileManager.o

# Client application
CLIENT_OBJS = $(OBJ_DIR)/sappis_client.o

all: sappis_server sappis_client

# Link Server
sappis_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Link Client
sappis_client: $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Compile C++ files to Objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) sappis_server sappis_client