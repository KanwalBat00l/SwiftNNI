#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <sstream>
#include <sys/wait.h>
#include <algorithm>
#include "ConfigManager.hpp"
#include "StringUtils.hpp"

struct ClientConfig {
    std::string snni_dir;
    std::string client_cmd_template;
    int timeout = 300;
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
        else if (key == "INFERENCE_TIMEOUT") cfg.timeout = std::stoi(val);
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cerr << "Usage: ./sappis_client <Srv_IP> <Srv_Port> <Model> <Batch> <SLO_ms>\n";
        return 1;
    }

    std::string srv_ip = argv[1];
    int srv_port = std::stoi(argv[2]);
    std::string model = argv[3];
    std::string batch = argv[4];
    std::string slo = argv[5];

    ClientConfig cfg = loadClientConfig("client.cfg");

    // --- 1. Connect ---
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

    // --- 2. Request ---
    std::string req = "r_" + model + "_" + batch + "_" + slo;
    send(sock, req.c_str(), req.length(), 0);

    // --- 3. Admission ---
    char buf[2048]; 
    memset(buf, 0, sizeof(buf));
    int n1 = read(sock, buf, sizeof(buf)-1);
    if (n1 <= 0) return 1;
    buf[n1] = '\0';
    std::string resp = ConfigManager::trim(buf);
    std::cout << "[SAPPIS] Admission: " << resp << std::endl;

    if (resp != "ACCEPTED") { close(sock); return 1; }

    // --- 4. Wait for Dispatch ---
    std::cout << "[SAPPIS] Waiting for ready file..." << std::endl;
    memset(buf, 0, sizeof(buf));
    int n2 = read(sock, buf, sizeof(buf)-1);
    if (n2 <= 0) return 1;
    buf[n2] = '\0';
    
    std::string dispatch = ConfigManager::trim(buf);
    std::cout << "[SAPPIS] Dispatch: " << dispatch << std::endl;
    close(sock);

    // --- 5. Parse Dispatch ---
    // Expected: START_INF:IP:PORT:MODEL:BATCH:FILE
    std::stringstream ss(dispatch);
    std::string item;
    std::vector<std::string> p;
    while (std::getline(ss, item, ':')) {
        p.push_back(ConfigManager::trim(item));
    }

    if (p.size() < 6) {
        std::cerr << "Malformed dispatch msg\n";
        return 1;
    }

    // --- 6. Execution ---
    // p[1]=IP, p[2]=Port, p[3]=Model, p[4]=Batch, p[5]=File
    std::string cmd = StringUtils::buildCommand(cfg.client_cmd_template, cfg.snni_dir, 
                                               p[3], std::stoi(p[4]), std::stoi(p[2]), p[5], p[1]);

    std::cout << "[Executing] " << cmd << std::endl;

    pid_t pid = fork();
    if (pid == 0) {
        // Use a clean environment for the shell
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
        std::cout << "[Result] Exit Code: " << WEXITSTATUS(status) << std::endl;
    }

    return 0;
}