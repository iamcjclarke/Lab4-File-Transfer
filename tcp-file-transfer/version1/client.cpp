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

#define PORT 9000
#define BUFFER_SIZE 4096

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

static std::string hex_digest(const unsigned char* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out.push_back(h[(d[i] >> 4) & 0xF]);
        out.push_back(h[d[i] & 0xF]);
    }
    return out;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./client <filename>\n";
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        die("socket() failed");
        return 1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1) {
        die("inet_pton() failed");
        close(sock);
        return 1;
    }

    if (connect(sock, (sockaddr*)&server, sizeof(server)) < 0) {
        die("connect() failed");
        close(sock);
        return 1;
    }

    std::string filename = argv[1];

    // send [filename_length][filename]
    uint32_t name_len_net = htonl(static_cast<uint32_t>(filename.size()));
    if (send_all(sock, &name_len_net, sizeof(name_len_net)) < 0) {
        die("send filename_length failed");
        close(sock);
        return 1;
    }
    if (send_all(sock, filename.c_str(), filename.size()) < 0) {
        die("send filename failed");
        close(sock);
        return 1;
    }

    // recv [file_size]
    uint32_t file_size_net = 0;
    if (recv_all(sock, &file_size_net, sizeof(file_size_net)) <= 0) {
        die("recv file_size failed");
        close(sock);
        return 1;
    }
    uint32_t file_size = ntohl(file_size_net);

    if (file_size == 0) {
        std::cout << "File not found on server.\n";
        close(sock);
        return 1;
    }

    std::ofstream output("received_" + filename, std::ios::binary);
    if (!output) {
        std::cerr << "[ERROR] could not open output file.\n";
        close(sock);
        return 1;
    }

    // receive file data, compute SHA256 as we write
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buffer[BUFFER_SIZE];
    uint32_t received = 0;

    while (received < file_size) {
        uint32_t remaining = file_size - received;
        size_t chunk = remaining < BUFFER_SIZE ? remaining : BUFFER_SIZE;

        ssize_t n = recv(sock, buffer, chunk, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("recv file data failed");
            close(sock);
            return 1;
        }
        if (n == 0) {
            std::cerr << "[ERROR] server closed early.\n";
            close(sock);
            return 1;
        }

        output.write(buffer, n);
        SHA256_Update(&ctx, buffer, static_cast<size_t>(n));
        received += static_cast<uint32_t>(n);
    }

    unsigned char local_digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(local_digest, &ctx);

    // receive checksum (32 bytes)
    unsigned char server_digest[SHA256_DIGEST_LENGTH];
    if (recv_all(sock, server_digest, SHA256_DIGEST_LENGTH) <= 0) {
        die("recv checksum failed");
        close(sock);
        return 1;
    }

    output.close();
    close(sock);

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::string local_hex = hex_digest(local_digest, SHA256_DIGEST_LENGTH);
    std::string server_hex = hex_digest(server_digest, SHA256_DIGEST_LENGTH);

    std::cout << "File received successfully.\n";
    std::cout << "Transfer time: " << ms << " ms\n";
    std::cout << "SHA-256 (client): " << local_hex << "\n";
    std::cout << "SHA-256 (server): " << server_hex << "\n";

    if (local_hex == server_hex) {
        std::cout << "[OK] Checksum match.\n";
        return 0;
    } else {
        std::cout << "[FAIL] Checksum mismatch.\n";
        return 2;
    }
}
