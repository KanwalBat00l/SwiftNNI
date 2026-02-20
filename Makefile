# ==============================================
# SwiftNNI Final Makefile
# ==============================================

CXX = g++
CXXFLAGS = -std=c++17 -I./include -Wall -Wextra -O3
LDFLAGS = -lpthread -lstdc++fs

SRC_DIR = src
OBJ_DIR = obj

# Common Logic
COMMON_OBJS = $(OBJ_DIR)/ConfigManager.o

# Server engine components
SERVER_OBJS = $(OBJ_DIR)/main.o \
              $(OBJ_DIR)/SwiftServer.o \
              $(OBJ_DIR)/ResourceManager.o \
              $(OBJ_DIR)/Logger.o \
              $(OBJ_DIR)/ProcessRunner.o \
              $(OBJ_DIR)/FileManager.o

# Client application
CLIENT_OBJS = $(OBJ_DIR)/SwiftClient.o

# Profiling tool
PROFILER_OBJS = $(OBJ_DIR)/profiler.o

all: Swift_server Swift_client #profiler

# Link Server
Swift_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Link Client
Swift_client: $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Link Profiler
profiler: $(PROFILER_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Compile C++ files to Objects
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	@echo "Cleaning SwiftNNI build artifacts..."
	rm -rf $(OBJ_DIR) Swift_server Swift_client profiler scheduler_log.csv

.PHONY: all clean