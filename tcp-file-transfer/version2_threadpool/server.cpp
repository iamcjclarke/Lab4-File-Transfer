#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>

#include <openssl/sha.h>

#include "threadpool.h"

#define PORT 9000
#define BUFFER_SIZE 4096
#define THREADS 4

static void die(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << " | errno=" << errno << " (" << strerror(errno) << ")\n";
}

static ssize_t send_all(int sock, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

static ssize_t recv_all(int sock, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

static std::string peer_ip_str(int client_socket) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(client_socket, (sockaddr*)&peer, &peer_len) != 0) return "UNKNOWN";
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) return "UNKNOWN";
    return std::string(ip);
}

// IMPORTANT: threadpool.cpp declares: extern void send_file(int);
void send_file(int client_socket) {
    auto t0 = std::chrono::steady_clock::now();

    uint32_t name_len_net = 0;
    if (recv_all(client_socket, &name_len_net, sizeof(name_len_net)) <= 0) {
        die("recv filename_length failed");
        return;
    }
    uint32_t name_len = ntohl(name_len_net);

    if (name_len == 0 || name_len >= 256) {
        std::cerr << "[WARN] Bad filename length: " << name_len << "\n";
        uint32_t size0 = htonl(0);
        send_all(client_socket, &size0, sizeof(size0));
        return;
    }

    char filename[256]{0};
    if (recv_all(client_socket, filename, name_len) <= 0) {
        die("recv filename failed");
        return;
    }
    filename[name_len] = '\0';

    std::string client_ip = peer_ip_str(client_socket);
    std::cout << "[LOG] Client IP: " << client_ip << "\n";
    std::cout << "[LOG] File requested: " << filename << "\n";

    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cout << "[LOG] File not found.\n";
        uint32_t size0 = htonl(0);
        send_all(client_socket, &size0, sizeof(size0));
        return;
    }

    file.seekg(0, std::ios::end);
    std::streamoff sz = file.tellg();
    file.seekg(0, std::ios::beg);

    if (sz < 0 || sz > static_cast<std::streamoff>(0xFFFFFFFFu)) {
        std::cerr << "[WARN] File too large for uint32_t protocol.\n";
        uint32_t size0 = htonl(0);
        send_all(client_socket, &size0, sizeof(size0));
        return;
    }

    uint32_t file_size = static_cast<uint32_t>(sz);
    std::cout << "[LOG] File size: " << file_size << " bytes\n";

    uint32_t file_size_net = htonl(file_size);
    if (send_all(client_socket, &file_size_net, sizeof(file_size_net)) < 0) {
        die("send file_size failed");
        return;
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buffer[BUFFER_SIZE];
    uint32_t sent_total = 0;

    while (file && sent_total < file_size) {
        file.read(buffer, BUFFER_SIZE);
        std::streamsize got = file.gcount();
        if (got <= 0) break;

        SHA256_Update(&ctx, buffer, static_cast<size_t>(got));

        if (send_all(client_socket, buffer, static_cast<size_t>(got)) < 0) {
            die("send file data failed");
            return;
        }
        sent_total += static_cast<uint32_t>(got);
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);

    if (send_all(client_socket, digest, SHA256_DIGEST_LENGTH) < 0) {
        die("send checksum failed");
        return;
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "[LOG] Transfer time: " << ms << " ms\n";
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        die("socket() failed");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        die("setsockopt(SO_REUSEADDR) failed");
        close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        die("bind() failed");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        die("listen() failed");
        close(server_fd);
        return 1;
    }

    ThreadPool pool(THREADS);

    std::cout << "Thread pool server running...\n";

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            die("accept() failed");
            continue;
        }
        pool.enqueue(client_socket);
    }

    close(server_fd);
    return 0;
}
