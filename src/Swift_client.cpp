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
#include "StringUtils.hpp"
#include "ConfigManager.hpp"

struct ClientConfig {
    std::string snni_dir;
    std::string client_cmd_template;
};

ClientConfig loadClientConfig() {
    std::ifstream file("client.cfg");
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
    if (argc < 5) {
        std::cerr << "Usage: ./Swift_client <Srv_IP> <Srv_Port> <Model> <Batch>\n";
        return 1;
    }

    std::string srv_ip = argv[1];
    int srv_port = std::stoi(argv[2]);
    std::string model = argv[3];
    int batch = std::stoi(argv[4]);

    ClientConfig cfg = loadClientConfig();

    // 1. Connection
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)srv_port);
    inet_pton(AF_INET, srv_ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connect failed");
        return 1;
    }

    // 2. Simple Request: r_model_batch
    std::string req = "r_" + model + "_" + std::to_string(batch);
    send(sock, req.c_str(), req.length(), 0);

    // 3. Wait for Admission (ACCEPTED)
    char buf[256] = {0};
    int n = read(sock, buf, 255);
    if (n <= 0 || std::string(buf).find("ACCEPTED") == std::string::npos) {
        std::cerr << "[Client] Request Rejected by Server" << std::endl;
        close(sock);
        return 1;
    }

    // 4. Wait for Dispatch (PORT:x;THREADS:y)
    std::cout << "[Client] Queued. Waiting for resources..." << std::endl;
    memset(buf, 0, sizeof(buf));
    n = read(sock, buf, 255);
    if (n <= 0) return 1;
    std::string dispatch(buf);
    close(sock);

    // Parse PORT:47101;THREADS:4
    int port = 0, threads = 1;
    size_t p_pos = dispatch.find("PORT:");
    size_t t_pos = dispatch.find(";THREADS:");
    if (p_pos != std::string::npos && t_pos != std::string::npos) {
        port = std::stoi(dispatch.substr(p_pos + 5, t_pos - (p_pos + 5)));
        threads = std::stoi(dispatch.substr(t_pos + 9));
    }

    // 5. Build command and execute
    // Template: ./benchmark-{MODEL} 1 {SERVER_IP} {PORT} {BATCH}
    // (Note: Mode 1 clients usually don't need the filename, but we provide it for safety)
    std::string cmd = StringUtils::buildCommand(cfg.client_cmd_template, cfg.snni_dir, 
                                               model, batch, port, "", srv_ip, threads);

    std::cout << "[Client] Executing Mode 1 Connection..." << std::endl;
    
    setenv("OMP_NUM_THREADS", std::to_string(threads).c_str(), 1);
    
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        std::cout << "[Client] Inference Session Closed. RC: " << WEXITSTATUS(status) << std::endl;
    }

    return 0;
}