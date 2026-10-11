#include "app_icon.hpp"

#include "raylib.h"

#include <cstring>

#if !defined(__APPLE__)
#include "logo_png.h"  // generated from packaging/logo.png by packaging.cmake
#endif

namespace wingit {

void UseAppFolder() {
#if defined(__APPLE__)
    const char* exeDir = GetApplicationDirectory();  // ".../Wing It.app/Contents/MacOS/"
    if (std::strstr(exeDir, ".app/Contents/MacOS") != nullptr) {
        ChangeDirectory(TextFormat("%s../Resources", exeDir));
    }
#endif
}

void SetWindowLogo() {
#if !defined(__APPLE__)
    Image logo = LoadImageFromMemory(".png", kLogoPng, static_cast<int>(sizeof(kLogoPng)));
    if (logo.data != nullptr) {
        ImageFormat(&logo, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);  // format SetWindowIcon expects
        SetWindowIcon(logo);
        UnloadImage(logo);
    }
#endif
}

}  // namespace wingit
