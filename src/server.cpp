#include "server.hpp"
#include "models.hpp"
#include "ai.hpp"
#include "httplib.h"
#include "qrcodegen.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
#include <string>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

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

static std::string get_local_ip() {
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

static void print_qr(const QrCode& qr) {
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

namespace {
std::mutex gMu;                       // guards everything below
std::string gUrl, gError;
bool gHasLan = false;
std::vector<std::string> gNewUploads; // drained by serverTakeUploads()
std::string gPhoto;                   // newest phone photo, drained by serverTakePhoto()
bool gHasPhoto = false;
constexpr size_t kMaxPhotoBytes = 12u << 20;
std::atomic<bool> gListening{false}, gStopping{false};

void setError(const std::string& e) { std::lock_guard<std::mutex> l(gMu); gError = e; }

std::string readAsset(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
} // namespace

std::string serverUrl() { std::lock_guard<std::mutex> l(gMu); return gUrl; }
bool serverHasLan() { std::lock_guard<std::mutex> l(gMu); return gHasLan; }
bool serverListening() { return gListening; }
std::string serverError() { std::lock_guard<std::mutex> l(gMu); return gError; }
std::vector<std::string> serverTakeUploads() {
    std::lock_guard<std::mutex> l(gMu);
    std::vector<std::string> out;
    out.swap(gNewUploads);
    return out;
}

bool serverTakePhoto(std::string& jpeg) {
    std::lock_guard<std::mutex> l(gMu);
    if (!gHasPhoto) return false;
    jpeg = std::move(gPhoto);
    gPhoto.clear();
    gHasPhoto = false;
    return true;
}

void start_server_thread() {
    std::string ip = get_local_ip();
    std::string url = "http://" + ip + ":" + std::to_string(kUploadPort);
    {
        std::lock_guard<std::mutex> l(gMu);
        gUrl = url;
        gHasLan = ip != "127.0.0.1";
    }
    std::error_code ec;
    std::filesystem::create_directories(kUploadDir, ec);

    try {
        QrCode qr = QrCode::encodeText(url.c_str(), QrCode::Ecc::LOW);
        std::cout << "Scan this QR code from your mobile device to upload files (" << url << "):\n\n";
        print_qr(qr);
    } catch (const std::exception& e) {
        std::cout << "Failed to generate QR Code: " << e.what() << "\n";
    }

    svr.set_payload_max_length(kMaxModelBytes + (1u << 20)); // + room for the multipart framing

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readAsset("assets/index.html");
        if (!content.empty()) {
            res.set_header("Cache-Control", "no-store");
            res.set_content(content, "text/html; charset=utf-8");
        } else {
            res.status = 404;
            res.set_content("index.html not found (Make sure it is in the assets/ directory)", "text/plain");
        }
    });

    // phone camera page -> photo for the AI model generator
    svr.Get("/camera", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readAsset("assets/camera.html");
        if (content.empty()) {
            res.status = 404;
            res.set_content("camera.html not found (Make sure it is in the assets/ directory)", "text/plain");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(content, "text/html; charset=utf-8");
    });

    svr.Post("/photo", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.form.has_file("photo")) {
            res.status = 400;
            res.set_content("No photo was sent.", "text/plain");
            return;
        }
        const auto& file = req.form.get_file("photo");
        const std::string& d = file.content;
        if (d.size() < 4 || (unsigned char)d[0] != 0xFF || (unsigned char)d[1] != 0xD8) {
            res.status = 415;
            res.set_content("That doesn't look like a JPEG photo.", "text/plain");
            return;
        }
        if (d.size() > kMaxPhotoBytes) {
            res.status = 413;
            res.set_content("The photo is too large (max " + std::to_string(kMaxPhotoBytes >> 20) + " MB).", "text/plain");
            return;
        }
        {
            std::lock_guard<std::mutex> l(gMu);
            gPhoto = d;
            gHasPhoto = true;
        }
        std::cout << "Received photo (" << d.size() << " bytes)\n";
        res.set_content("ok", "text/plain");
    });

    // the phone page polls this to show how the computer is getting on
    svr.Get("/ai-status", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(aiStatusJson(), "application/json");
    });

    svr.Post("/upload", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.form.has_file("scan_file")) {
            res.status = 400;
            res.set_content("No file was sent.", "text/plain");
            return;
        }
        const auto& file = req.form.get_file("scan_file");
        std::string err;
        // sanitizes the name (no path traversal), checks size + extension, validates the mesh, never overwrites
        std::string id = modelsImportBytes(file.filename, file.content.data(), file.content.size(), err);
        if (id.empty()) {
            std::cout << "Rejected upload '" << file.filename << "': " << err << "\n";
            res.status = 422;
            res.set_content(err, "text/plain");
            return;
        }
        std::cout << "Received model: " << id << " (" << file.content.size() << " bytes)\n";
        {
            std::lock_guard<std::mutex> l(gMu);
            gNewUploads.push_back(id);
        }
        res.status = 200;
        res.set_content(modelLabel(id), "text/plain");
    });

    // httplib answers oversized bodies with 413 before our handler runs - give it a readable message
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (!res.body.empty()) return;
        if (res.status == 413)
            res.set_content("The file is too large (max " + std::to_string(kMaxModelBytes >> 20) + " MB).", "text/plain");
        else
            res.set_content("Error " + std::to_string(res.status), "text/plain");
    });

    server_thread = std::thread([]() {
        if (!svr.bind_to_port("0.0.0.0", kUploadPort)) {
            setError("Couldn't open port " + std::to_string(kUploadPort) + " (is another copy of the app running?)");
            std::cout << "Upload server: couldn't bind port " << kUploadPort << "\n";
            return;
        }
        gListening = true;
        if (!gStopping) svr.listen_after_bind();
        gListening = false;
    });
}

void stop_server_thread() {
    gStopping = true;
    svr.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}
