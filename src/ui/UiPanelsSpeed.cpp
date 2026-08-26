// The speed hack, and exporting the address list as a trainer. Two features
// that share a panel because they are the two things people do once the finding
// is done: change how fast the game runs, and take the result away with them.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include "engine_export/TrainerExport.h"
#include "infra/Paths.h"

#include <shellapi.h>
#include <shlobj.h>

namespace ire::ui {

namespace {

// The presets, because a slider is a poor way to ask for exactly half speed and
// half speed is what people want. 0.1 is slow enough to read a frame; 5 is fast
// enough to skip a loading screen without the physics leaving the map.
struct Preset {
    const char* label;
    double scale;
};

constexpr Preset presets[]{
    {"0.1x", 0.1}, {"0.25x", 0.25}, {"0.5x", 0.5}, {"1x", 1.0},
    {"2x", 2.0},   {"3x", 3.0},     {"5x", 5.0},
};

} // namespace

void UiApp::renderSpeedPanel() {
    ImGui::Begin("Speed and Export", &showSpeed_);

    const bool attached = services_.session().attached();
    const auto status = services_.speed().status();

    statusPill(status.running ? "HOOKED" : (status.loaded ? "LOADED" : "OFF"),
               status.running ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    if (status.loaded) {
        ImGui::Text("%.2fx", status.applied);
        ImGui::SameLine();
        ImGui::TextDisabled("(%u import(s) redirected)", status.hookedImports);
    } else {
        ImGui::TextDisabled("The payload has not been loaded into this target.");
    }
    ImGui::SameLine();
    helpMarker(
        "A game does not measure time, it asks Windows what time it is and works out how much has passed "
        "since it last asked. Everything it does per frame -- how far to move, how much of a cooldown has "
        "elapsed, how far through an animation to be -- is that delta multiplied by something.\n\n"
        "So this is not a hack on the game. It is a hook on the four clocks the game can ask, returning "
        "one that runs at a different rate. It needs to know nothing about the game at all.\n\n"
        "The clock is rebased rather than multiplied, so it never jumps and never runs backwards when the "
        "rate changes. A game that sees time go backwards does not slow down; it divides by a negative "
        "delta and detonates.");

    if (status.loaded && status.hookedImports == 0 && status.running) {
        // The honest failure, and the one that would otherwise look like the
        // feature not working for no reason.
        ImGui::TextColored(colorFromBytes(214, 154, 70), "%s",
                           "The payload is running but found nothing to redirect. This target does not call "
                           "the timing functions through its import table -- it resolves them at runtime, or "
                           "it uses a clock this hook does not cover.");
    }

    ImGui::Separator();

    ImGui::BeginDisabled(!attached);
    for (const auto& preset : presets) {
        if (ImGui::SmallButton(preset.label)) {
            if (auto set = services_.speed().setScale(preset.scale); !set) {
                notifyError(set.error());
            } else {
                speedScale_ = static_cast<float>(preset.scale);
                notifyInfo(std::string("Speed set to ") + preset.label + ".");
            }
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::SliderFloat("##speed", &speedScale_, static_cast<float>(engine_speed::SpeedController::minScale),
                       static_cast<float>(engine_speed::SpeedController::maxScale), "%.2fx",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        if (auto set = services_.speed().setScale(static_cast<double>(speedScale_)); !set) {
            notifyError(set.error());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Normal")) {
        speedScale_ = 1.0f;
        if (auto set = services_.speed().setScale(1.0); !set) {
            notifyError(set.error());
        }
    }
    ImGui::EndDisabled();

    if (status.loaded) {
        if (ImGui::SmallButton("Remove the hook")) {
            confirmAction("Put the clocks back?",
                          "Every redirected import is restored and the payload stops working. It stays "
                          "loaded in the target: unloading a module while a thread might be executing "
                          "inside it is a crash, and there is no way to prove none is.",
                          "Remove", [this] {
                              if (auto reset = services_.speed().reset(); !reset) {
                                  notifyError(reset.error());
                              } else {
                                  speedScale_ = 1.0f;
                                  notifyInfo("The clocks are back to normal.");
                              }
                          });
        }
    }

    // -----------------------------------------------------------------------
    // Trainer export
    // -----------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Export as a trainer");
    ImGui::SameLine();
    helpMarker(
        "Writes a C++ project, not an executable.\n\n"
        "A generated binary is a black box: it works, and you have learned nothing about why. Generated "
        "source is the same trainer with its reasoning visible -- how a process is found by name, how a "
        "module base is looked up, how a pointer chain is walked one dereference at a time.\n\n"
        "It is also the difference between a program you have read and a program of unknown provenance "
        "you are about to point at your own machine.");

    const auto entries = services_.session().addressList().snapshot();
    std::size_t exportable = 0;
    for (const auto& entry : entries) {
        if (!entry.frozenValue.empty()) {
            ++exportable;
        }
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##trainer-name", "trainer name", trainerName_.data(), trainerName_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputTextWithHint("##trainer-dir", "output directory", trainerDirectory_.data(),
                             trainerDirectory_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(entries.empty());
    if (ImGui::Button("Export")) {
        exportTrainer();
    }
    ImGui::EndDisabled();

    if (entries.empty()) {
        ImGui::TextDisabled("The address list is empty, so there is nothing to export.");
    } else {
        ImGui::TextDisabled("%zu of %zu entr%s can be written; the rest have no frozen value.", exportable,
                            entries.size(), entries.size() == 1 ? "y" : "ies");
        if (exportable == 0) {
            ImGui::TextWrapped(
                "An entry's exported value is the one it is frozen at. Freeze what you want the trainer to "
                "write, then export -- otherwise the trainer knows the addresses and not what to put in "
                "them.");
        }
    }

    ImGui::End();
}

void UiApp::exportTrainer() {
    engine_export::TrainerOptions options;
    options.name = trainerName_.data();
    if (options.name.empty()) {
        options.name = "Trainer";
    }
    options.processName = services_.session().processName();
    if (options.processName.empty()) {
        options.processName = L"game.exe";
    }
    options.bitness = services_.session().bitness();
    options.entries = services_.session().addressList().snapshot();

    std::filesystem::path directory = trainerDirectory_.data();
    if (directory.empty()) {
        // Beside the project when there is one, because that is where the
        // person is already keeping this work.
        directory = projectPath_.empty() ? infra::Paths::appData() / "trainers"
                                         : projectPath_.parent_path();
        directory /= engine_export::TrainerExport::identifier(options.name);
    }

    auto written = engine_export::TrainerExport::write(directory, options);
    if (!written) {
        notifyError(written.error());
        return;
    }

    copyText(trainerDirectory_.data(), trainerDirectory_.size(), directory.string());
    notifyInfo("Wrote " + std::to_string(written.value()) + " file(s) to " + directory.string() +
               ". Read main.cpp, then build it with the two commands in README.md.");
    ShellExecuteW(nullptr, L"open", directory.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace ire::ui
