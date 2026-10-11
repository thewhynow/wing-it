#pragma once

// Small helpers for the packaged game. The matching app_icon.cpp is added to the
// build automatically by packaging/packaging.cmake.
//
// Usage in main():
//
//     int main() {
//         wingit::UseAppFolder();      // first line, before any files are loaded
//         ...
//         InitWindow(...);
//         wingit::SetWindowLogo();     // right after InitWindow()
//         ...
//     }

namespace wingit {

// macOS app only: when the game is opened from Finder, its working folder is "/",
// so relative paths like "assets/plane.obj" would not be found. This switches to
// the app's Resources folder, where the package puts the game's data folders.
// Does nothing on Windows, on Linux, or when run from the terminal during development.
void UseAppFolder();

// Sets the logo as the window and taskbar icon (Windows and Linux). On macOS the
// Dock icon comes from the .app bundle instead, so this does nothing there.
void SetWindowLogo();

}  // namespace wingit
