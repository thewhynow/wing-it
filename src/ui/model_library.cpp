// Model library UI: grouped list, file dialog / drag-and-drop import, "Upload from phone" QR window.
#include "model_library.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <raylib.h>
#include "file_dialog.hpp"
#include "models.hpp"
#include "qrcodegen.hpp"
#include "server.hpp"
#include "ui.hpp"

namespace {

struct ImportResult { std::string id, err; };

// shared with the import worker threads. leaked on purpose: a detached worker (e.g. a file dialog still
// open) may outlive main, so this must never be destroyed
struct Shared {
    std::mutex mu;
    std::vector<ImportResult> results;
    std::atomic<int> jobs{0};
    std::atomic<bool> dialogOpen{false};
};
Shared& S = *new Shared;

const ImVec4 kRed(1.0f, 0.38f, 0.38f, 1.0f), kGreen(0.45f, 0.95f, 0.55f, 1.0f), kAccent(0.45f, 0.75f, 1.0f, 1.0f);
constexpr const char* kPhoneTitle = "Upload from phone";

std::string pendingSelect;     // newest arrival / "Use this model" click, consumed by modelLibraryControls
std::string scrollTarget;      // list scrolls this ID into view once
std::string status;            // last import result
bool statusErr = false;
double statusTime = -1e9;
bool phoneOpen = false, phoneOpenRequest = false;
std::set<std::string> fromPhone;  // IDs received from phones this session
int controlsFrame = -10;          // last frame modelLibraryControls ran (the phone window only shows with it)
bool lastAllowSelect = true;
std::string lastSelected;

void setStatus(const std::string& s, bool err) { status = s, statusErr = err, statusTime = GetTime(); }

// copy + validate files on a worker thread so a big mesh doesn't freeze the UI
void startImports(std::vector<std::string> paths) {
    if (paths.empty()) return;
    S.jobs++;
    std::thread([paths = std::move(paths)] {
        for (const auto& p : paths) {
            ImportResult r;
            r.id = modelsImportFile(p, r.err);
            std::lock_guard<std::mutex> l(S.mu);
            S.results.push_back(std::move(r));
        }
        S.jobs--;
    }).detach();
}

void openFileDialog() {
    if (S.dialogOpen.exchange(true)) return;
    if (!fileDialogAvailable()) {
        S.dialogOpen = false;
        setStatus("No file dialog available on this system - drag the file onto the window instead.", true);
        return;
    }
    S.jobs++;
    std::thread([] {
        std::vector<std::string> files = fileDialogOpenModels(); // blocks until the dialog closes
        S.dialogOpen = false;
        for (const auto& p : files) {
            ImportResult r;
            r.id = modelsImportFile(p, r.err);
            std::lock_guard<std::mutex> l(S.mu);
            S.results.push_back(std::move(r));
        }
        S.jobs--;
    }).detach();
}

// --- QR code (cached per URL) ---
std::string qrFor;
std::vector<bool> qrModules;
int qrSize = 0;

void drawQr(const std::string& text, float maxPx) {
    if (text != qrFor) {
        qrFor = text;
        qrModules.clear();
        qrSize = 0;
        try {
            auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            qrSize = qr.getSize();
            qrModules.resize((size_t)qrSize * qrSize);
            for (int y = 0; y < qrSize; ++y)
                for (int x = 0; x < qrSize; ++x) qrModules[(size_t)y * qrSize + x] = qr.getModule(x, y);
        } catch (...) {}
    }
    if (qrSize == 0) { ImGui::TextColored(kRed, "Couldn't make a QR code."); return; }
    const int quiet = 4; // the spec's quiet zone, scanners need it
    float px = std::floor(maxPx / (qrSize + 2 * quiet)); // whole pixels per module keep it crisp
    if (px < 1.0f) px = 1.0f;
    float total = px * (qrSize + 2 * quiet);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + total, o.y + total), IM_COL32(255, 255, 255, 255), 8.0f);
    for (int y = 0; y < qrSize; ++y)
        for (int x = 0; x < qrSize; ++x)
            if (qrModules[(size_t)y * qrSize + x]) {
                ImVec2 a(o.x + (x + quiet) * px, o.y + (y + quiet) * px);
                dl->AddRectFilled(a, ImVec2(a.x + px, a.y + px), IM_COL32(0, 0, 0, 255));
            }
    ImGui::Dummy(ImVec2(total, total));
}

void drawPhoneWindow(const UiState& ui) {
    if (phoneOpenRequest) {
        phoneOpenRequest = false;
        ImGui::OpenPopup(kPhoneTitle);
    }
    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPhoneTitle, &phoneOpen, ImGuiWindowFlags_NoSavedSettings)) {
        phoneOpen = false;
        return;
    }
    std::string url = serverUrl();

    ImGui::BeginGroup();
    if (url.empty()) ImGui::TextColored(kRed, "Upload server isn't running.");
    else drawQr(url, 230.0f);
    ImGui::EndGroup();
    ImGui::SameLine(0, 20);

    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
    ImGui::TextColored(kAccent, "Send a model from your phone");
    ImGui::Spacing();
    ImGui::TextWrapped("1. Connect your phone to the same Wi-Fi as this computer.");
    ImGui::TextWrapped("2. Scan the code with the camera app.");
    ImGui::TextWrapped("3. Choose a %s file and tap Upload.", kModelExtsHint);
    ImGui::Spacing();
    ImGui::TextDisabled("Or open this address in a browser:");
    ImGui::Text("%s", url.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(url.c_str());
    ImGui::Spacing();
    std::string err = serverError();
    if (!err.empty()) ImGui::TextColored(kRed, "%s", err.c_str());
    else if (!serverHasLan()) ImGui::TextColored(kRed, "No network found - phones can't reach this computer right now.");
    else if (serverListening()) {
        int dots = (int)(GetTime() * 2.0) % 4;
        ImGui::TextDisabled("Waiting for uploads%.*s", dots, "...");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::SeparatorText("My models");
    if (ImGui::BeginChild("##phonefiles", ImVec2(-FLT_MIN, 170), ImGuiChildFlags_Borders)) {
        bool any = false;
        for (int i = 0; i < (int)ui.models.size(); ++i) {
            const std::string& id = ui.models[i];
            if (!modelIsCustom(id)) continue;
            any = true;
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(modelLabel(id).c_str());
            if (fromPhone.count(id)) {
                ImGui::SameLine();
                ImGui::TextColored(kGreen, "from phone");
            }
            float bw = 140.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - bw));
            if (id == lastSelected) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kGreen, "Selected");
            } else {
                ImGui::BeginDisabled(!lastAllowSelect);
                if (ImGui::Button("Use this model", ImVec2(bw, 0))) {
                    pendingSelect = id;
                    phoneOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("Nothing here yet - files you upload will appear here.");
    }
    ImGui::EndChild();

    if (!status.empty() && GetTime() - statusTime < 15.0)
        ImGui::TextColored(statusErr ? kRed : kGreen, "%s", status.c_str());
    if (ImGui::Button("Done", ImVec2(120, 0))) {
        phoneOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

bool modelLibraryModalOpen() { return phoneOpen; }

int modelIndexOf(const UiState& ui, const std::string& id) {
    for (int i = 0; i < (int)ui.models.size(); ++i)
        if (ui.models[i] == id) return i;
    return -1;
}

void modelLibraryRefresh(UiState& ui) {
    std::string keep = ui.model >= 0 && ui.model < (int)ui.models.size() ? ui.models[ui.model] : "";
    ui.models.clear();
    for (const auto& m : modelsList()) ui.models.push_back(m.id);
    int i = modelIndexOf(ui, keep);
    ui.model = i >= 0 ? i : std::min(std::max(ui.model, 0), std::max(0, (int)ui.models.size() - 1));
}

void modelLibraryFrame(UiState& ui, bool acceptDrops) {
    // raylib keeps dropped files queued until unloaded - drain them every frame from here
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        std::vector<std::string> paths;
        for (unsigned i = 0; i < dropped.count; ++i) paths.emplace_back(dropped.paths[i]);
        UnloadDroppedFiles(dropped);
        if (acceptDrops) {
            setStatus("Importing " + std::to_string(paths.size()) + (paths.size() == 1 ? " file..." : " files..."), false);
            startImports(std::move(paths));
        }
    }

    std::vector<ImportResult> results;
    {
        std::lock_guard<std::mutex> l(S.mu);
        results.swap(S.results);
    }
    std::vector<std::string> phone = serverTakeUploads();
    if (!results.empty() || !phone.empty()) {
        modelsRefresh();
        modelLibraryRefresh(ui);
        int added = 0;
        std::string lastErr, lastAdded;
        for (const auto& r : results) {
            if (r.id.empty()) { lastErr = r.err; continue; }
            ++added, lastAdded = r.id, pendingSelect = r.id;
        }
        for (const auto& id : phone) {
            fromPhone.insert(id);
            pendingSelect = id;
        }
        if (!phone.empty()) setStatus("Received " + modelLabel(phone.back()) + " from your phone", false);
        else if (!lastErr.empty()) setStatus(added ? lastErr + " (" + std::to_string(added) + " other file(s) added)" : lastErr, true);
        else setStatus(added == 1 ? "Added " + modelLabel(lastAdded) : "Added " + std::to_string(added) + " models", false);
    }

    // the phone window belongs to the select / pick screens, which call modelLibraryControls every frame
    if (ImGui::GetFrameCount() - controlsFrame > 2) phoneOpen = phoneOpenRequest = false;
    if (phoneOpen) drawPhoneWindow(ui);
}

bool modelLibraryList(const char* strId, const UiState& ui, std::string& selectedId, ImVec2 size, bool* activated) {
    bool changed = false;
    if (!ImGui::BeginListBox(strId, size)) return false;
    for (int pass = 0; pass < 2; ++pass) {
        bool custom = pass == 1, any = false;
        ImGui::SeparatorText(custom ? "My models" : "Sample models");
        for (int i = 0; i < (int)ui.models.size(); ++i) {
            const std::string& id = ui.models[i];
            if (modelIsCustom(id) != custom) continue;
            any = true;
            ImGui::PushID(i); // a sample and an upload can share a filename
            bool sel = id == selectedId;
            if (ImGui::Selectable(modelLabel(id).c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (!sel) selectedId = id, changed = true;
                if (activated && ImGui::IsMouseDoubleClicked(0)) *activated = true;
            }
            if (id == scrollTarget) { ImGui::SetScrollHereY(); scrollTarget.clear(); }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled(custom ? "  none yet - add one below" : "  none found");
    }
    ImGui::EndListBox();
    return changed;
}

bool modelLibraryControls(const UiState& ui, std::string& selectedId, bool allowSelect) {
    controlsFrame = ImGui::GetFrameCount();
    lastAllowSelect = allowSelect;
    bool changed = false;
    if (!pendingSelect.empty()) {
        if (allowSelect && modelIndexOf(ui, pendingSelect) >= 0 && pendingSelect != selectedId) {
            selectedId = pendingSelect;
            scrollTarget = pendingSelect;
            changed = true;
        }
        pendingSelect.clear();
    }
    lastSelected = selectedId;

    ImGui::BeginDisabled(S.dialogOpen);
    if (ImGui::Button(S.dialogOpen ? "Choose a file..." : "Add model file..."))
        openFileDialog();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Upload from phone"))
        phoneOpen = phoneOpenRequest = true;

    if (S.jobs > 0 && !S.dialogOpen)
        ImGui::TextDisabled("Importing...");
    else
        ImGui::TextDisabled("Tip: drop %s files onto the window", kModelExtsHint);
    if (!status.empty() && GetTime() - statusTime < 15.0) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
        ImGui::TextColored(statusErr ? kRed : kGreen, "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    return changed;
}
