#include "file_dialog.hpp"
#include <portable-file-dialogs.h>

bool fileDialogAvailable() { return pfd::settings::available(); }

std::vector<std::string> fileDialogOpenModels() {
    if (!pfd::settings::available()) return {};
    // no "*" entry: on macOS pfd drops all filtering as soon as any pattern is "*"
    return pfd::open_file("Add a model file (.obj, .stl, .ply)", "",
                          {"3D models (.obj .stl .ply)", "*.obj *.stl *.ply"},
                          pfd::opt::multiselect)
        .result();
}
