#include "ResourceManager.hpp"


ResourceManager::ResourceManager(int total_cores, int port_base, int port_range, int max_preproc)
    : total_cores(total_cores), used_cores(0), port_base(port_base), 
      port_range(port_range), max_preproc(max_preproc), active_preproc(0) {
    for (int i = 0; i < port_range; ++i) {
        available_ports.insert(port_base + i);
    }
}


bool ResourceManager::tryAcquireCores(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    if (used_cores + count <= total_cores) {
        used_cores += count;
        return true;
    }
    return false;
}

void ResourceManager::releaseCores(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    used_cores = (used_cores >= count) ? used_cores - count : 0;
}

int ResourceManager::getFreeCores() const {
    std::lock_guard<std::mutex> lock(mtx);
    return total_cores - used_cores;
}

int ResourceManager::acquirePort() {
    std::lock_guard<std::mutex> lock(mtx);
    if (available_ports.empty()) return -1;

    auto it = available_ports.begin();
    int port = *it;
    available_ports.erase(it);
    busy_ports.insert(port);
    return port;
}

void ResourceManager::releasePort(int port) {
    std::lock_guard<std::mutex> lock(mtx);
    busy_ports.erase(port);
    available_ports.insert(port);
}




bool ResourceManager::tryAcquirePreproc() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc < max_preproc) {
        active_preproc++;
        return true;
    }
    return false;
}

void ResourceManager::releasePreproc() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc > 0) active_preproc--;
}