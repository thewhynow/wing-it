#include "server.hpp"
#include "httplib.h"
#include "qrcodegen.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
#include <string>
#include <cstring>

#ifdef _WIN32
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using namespace qrcodegen;

static std::thread server_thread;
static httplib::Server svr;

std::string get_local_ip() {
    std::string ip = "127.0.0.1";
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != -1) {
        struct sockaddr_in serv;
        memset(&serv, 0, sizeof(serv));
        serv.sin_family = AF_INET;
        serv.sin_addr.s_addr = inet_addr("8.8.8.8");
        serv.sin_port = htons(53);

        if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) == 0) {
            struct sockaddr_in name;
            socklen_t namelen = sizeof(name);
            if (getsockname(sock, (struct sockaddr*)&name, &namelen) == 0) {
                char buffer[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &name.sin_addr, buffer, sizeof(buffer))) {
                    ip = buffer;
                }
            }
        }
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return ip;
}

void print_qr(const QrCode& qr) {
    int border = 2;
    for (int y = -border; y < qr.getSize() + border; y += 2) {
        std::cout << "\033[47;30m";
        for (int x = -border; x < qr.getSize() + border; x++) {
            bool top = qr.getModule(x, y);
            bool bottom = qr.getModule(x, y + 1);

            if (top && bottom) {
                std::cout << "\xE2\x96\x88\xE2\x96\x88";
            } else if (top && !bottom) {
                std::cout << "\xE2\x96\x80\xE2\x96\x80";
            } else if (!top && bottom) {
                std::cout << "\xE2\x96\x84\xE2\x96\x84";
            } else {
                std::cout << "  ";
            }
        }
        std::cout << "\033[0m\n";
    }
}

void start_server_thread() {
    server_thread = std::thread([]() {
        std::string ip = get_local_ip();
        std::string url = "http://" + ip + ":8081";
        
        try {
            QrCode qr = QrCode::encodeText(url.c_str(), QrCode::Ecc::LOW);
            std::cout << "Scan this QR code from your mobile device to upload files:\n\n";
            print_qr(qr);
        } catch (const std::exception& e) {
            std::cout << "Failed to generate QR Code: " << e.what() << "\n";
        }

        svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
            std::ifstream file("assets/index.html");
            if (file.is_open()) {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                res.set_content(content, "text/html");
            } else {
                res.status = 404;
                res.set_content("index.html not found (Make sure it is in the assets/ directory)", "text/plain");
            }
        });

        svr.Post("/upload", [](const httplib::Request& req, httplib::Response& res) {
            if (req.form.has_file("scan_file")) {
                const auto& file = req.form.get_file("scan_file");
                
                std::string filename = "uploads/" + file.filename;
                std::ofstream ofs(filename, std::ios::binary);
                if (ofs.is_open()) {
                    ofs.write(file.content.data(), file.content.size());
                    ofs.close();
                    std::cout << "Successfully uploaded file: " << filename << " (" << file.content.size() << " bytes)\n";
                    res.status = 200;
                    res.set_content("Success", "text/plain");
                } else {
                    res.status = 500;
                    res.set_content("Failed to save file (Make sure uploads/ directory exists)", "text/plain");
                }
            } else {
                res.status = 400;
                res.set_content("No scan_file provided", "text/plain");
            }
        });

        svr.listen("0.0.0.0", 8081);
    });
}

void stop_server_thread() {
    svr.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}
