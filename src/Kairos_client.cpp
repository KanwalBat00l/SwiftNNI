#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <sstream>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include "ConfigManager.hpp"
#include "StringUtils.hpp"

struct ClientConfig {
    std::string snni_dir;
    std::string client_cmd_template;
};

ClientConfig loadClientConfig(std::string path) {
    std::ifstream file(path);
    ClientConfig cfg;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ConfigManager::trim(line.substr(0, eq));
        std::string val = ConfigManager::trim(line.substr(eq + 1));
        if (key == "SNNI_DIR") cfg.snni_dir = val;
        else if (key == "CLIENT_CMD_TEMPLATE") cfg.client_cmd_template = val;
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cerr << "Usage: ./Kairos_client <Srv_IP> <Srv_Port> <Model> <Batch> <SLO_ms>\n";
        return 1;
    }

    std::string srv_ip = argv[1];
    int srv_port = std::stoi(argv[2]);
    std::string model = argv[3];
    int batch = std::stoi(argv[4]);
    long slo_ms = std::stol(argv[5]);

    ClientConfig cfg = loadClientConfig("client.cfg");

    // 1. Connection
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)srv_port);
    inet_pton(AF_INET, srv_ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connect failed");
        return 1;
    }

    // 2. Protocol Handshake
    std::string req = "r_" + model + "_" + std::to_string(batch) + "_" + std::to_string(slo_ms);
    send(sock, req.c_str(), req.length(), 0);

    char buf[1024] = {0};
    int n = read(sock, buf, sizeof(buf)-1);
    if (n <= 0) return 1;

    // Q-2: TTFR Handshake — respond to PING probe if received
    std::string resp(buf, n);
    if (resp.find("TTFR_PING") != std::string::npos) {
        send(sock, "TTFR_PONG", 9, 0);
        // Read the actual admission response
        memset(buf, 0, sizeof(buf));
        n = read(sock, buf, sizeof(buf)-1);
        if (n <= 0) return 1;
        resp = std::string(buf, n);
    }

    // Q-3: Handle negotiation proposal if received
    if (resp.find("NEGOTIATE:") != std::string::npos) {
        std::cout << "[Client] Server proposes alternative: " << resp << std::endl;
        send(sock, "ACCEPT", 6, 0);
        memset(buf, 0, sizeof(buf));
        n = read(sock, buf, sizeof(buf)-1);
        if (n <= 0) return 1;
        resp = std::string(buf, n);
    }

    if (resp.find("ACCEPTED") == std::string::npos) {
        std::cout << "[Client] Rejected: " << resp << std::endl;
        return 1;
    }

    // 3. Wait for Dispatch
    std::cout << "[Client] Waiting for dispatch..." << std::endl;
    memset(buf, 0, sizeof(buf));
    n = read(sock, buf, sizeof(buf)-1);
    if (n <= 0) return 1;
    std::string dispatch = buf;
    close(sock);

    // 4. Parse & Command Prep
    // Dispatch format: "START_INF:<IP>:<PORT>:<MODEL_BATCH>:<FILE_PREFIX>"
    std::stringstream ss(dispatch);
    std::string item;
    std::vector<std::string> p;
    while (std::getline(ss, item, ':')) p.push_back(item);

    if (p.size() < 5) {
        std::cerr << "[Client] Bad dispatch: " << dispatch << std::endl;
        return 1;
    }

    // p[0]=START_INF, p[1]=IP, p[2]=PORT, p[3]=model_batch, p[4]=file_prefix
    // Extract model and batch from key (e.g., "simc1_1" → model="simc1", batch=1)
    std::string key = p[3];
    auto uscore = key.rfind('_');
    std::string d_model = key.substr(0, uscore);
    int d_batch = std::stoi(key.substr(uscore + 1));

    std::string cmd = StringUtils::buildCommand(cfg.client_cmd_template, cfg.snni_dir,
                                               d_model, d_batch, std::stoi(p[2]), p[4], p[1]);

    // 5. Execution with Zombie Guard
    // Guard timeout = SLO + 30s buffer
    int zombie_guard_sec = static_cast<int>((slo_ms / 1000) + 30);
    
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    } else {
        auto start = std::chrono::steady_clock::now();
        int status;
        while (true) {
            pid_t res = waitpid(pid, &status, WNOHANG);
            if (res == pid) {
                std::cout << "[Client] Finished. RC: " << WEXITSTATUS(status) << std::endl;
                break;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > zombie_guard_sec) {
                std::cerr << "[Client] Safety timeout hit. Killing stuck process." << std::endl;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return 0;
}