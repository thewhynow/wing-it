#pragma once
// Native "open file" dialog for model files. BLOCKS until the user closes it - call it from a worker thread.
// Kept in its own translation unit (src/file_dialog.cpp) because portable-file-dialogs pulls in <windows.h>
// on Windows, which clashes with raylib.
#include <string>
#include <vector>

std::vector<std::string> fileDialogOpenModels(); // chosen paths, empty if cancelled / unavailable
bool fileDialogAvailable();                     // false if there's no backend (e.g. Linux without zenity/kdialog)
