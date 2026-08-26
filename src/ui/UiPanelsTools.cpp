// The panels that are their own tools rather than views of the target: the
// pointer scanner, injection, the Lua scanner and console, and the log.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

#include <shellapi.h>

namespace ire::ui {

void UiApp::renderPointerPanel() {
    ImGui::Begin("Pointer Scanner", &showPointerScanner_);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Target address", "0x7FF... or a symbol", pointerTarget_.data(), pointerTarget_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("Max depth", &pointerDepth_);
    pointerDepth_ = std::clamp(pointerDepth_, 1, 8);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Max offset", pointerMaxOffset_.data(), pointerMaxOffset_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(118.0f);
    const auto typeNames = valueTypeNames();
    ImGui::Combo("Value type", &pointerTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));

    // Fetched once: the table below needs it too, and copying every chain twice
    // a frame is not free once a scan has found thousands of them.
    const auto chains = services_.pointerScanJob().results();

    if (ImGui::Button("Start pointer scan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process first.");
        } else if (auto target = resolveAddress(pointerTarget_.data())) {
            engine_pointer::PointerScanOptions options;
            options.target = *target;
            options.maxDepth = static_cast<std::uint32_t>(pointerDepth_);
            options.maxOffset = static_cast<std::uint32_t>(parseAddress(pointerMaxOffset_.data()).value_or(0x1000));
            services_.pointerScanJob().start(options);
        } else {
            notifyError("Target address is not valid hexadecimal.");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(chains.empty() || services_.pointerScanJob().progress().running);
    if (ImGui::Button("Rescan")) {
        if (!services_.session().attached()) {
            notifyError("Attach to a process first.");
        } else if (auto target = resolveAddress(pointerTarget_.data())) {
            services_.pointerScanJob().filter(*target);
        } else {
            notifyError("Target address is not valid hexadecimal.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    helpMarker("Keeps only the chains that still resolve to the target address above, and discards the "
               "rest. A first scan finds thousands of chains that pointed the right way once. Restart "
               "the target, find the value's new address, put it in Target address and rescan: what "
               "survives is what actually tracks the value. Rescanning costs seconds, not minutes.");
    if (services_.pointerScanJob().progress().running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel pointer scan")) {
            services_.pointerScanJob().cancel();
        }
    }
    const auto progress = services_.pointerScanJob().progress();
    ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
    ImGui::TextDisabled(
        "Adding a chain tracks it as module+offset, so it re-resolves itself when the target restarts.");

    if (ImGui::BeginTable("pointer-results", 5, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Offsets", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Resolves to", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(chains.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& chain = chains[static_cast<std::size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(domain::narrow(chain.moduleName).c_str());
                ImGui::TableNextColumn();
                // Module-relative, because that is what actually survives a
                // restart. The absolute address is only true for this run.
                ImGui::Text("%s+%s", domain::narrow(chain.moduleName).c_str(),
                            domain::toHex(chain.moduleOffset).c_str());
                ImGui::TableNextColumn();
                std::ostringstream offsets;
                for (std::size_t i = 0; i < chain.offsets.size(); ++i) {
                    if (i != 0) {
                        offsets << ", ";
                    }
                    offsets << "0x" << std::hex << chain.offsets[i];
                }
                ImGui::TextUnformatted(offsets.str().c_str());
                ImGui::TableNextColumn();
                if (auto resolved = engine_pointer::resolveChain(services_.session(), chain)) {
                    ImGui::TextUnformatted(domain::toHex(resolved.value()).c_str());
                } else {
                    ImGui::TextDisabled("unresolved");
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Add")) {
                    const auto type = domain::valueTypes()[static_cast<std::size_t>(pointerTypeIndex_)];
                    services_.addressList().addChain(chain, type,
                                                     domain::narrow(chain.moduleName) + "+" +
                                                         domain::toHex(chain.moduleOffset),
                                                     "Pointers");
                    notifyInfo("Added the pointer chain to the address list.");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
void UiApp::renderInjectionPanel() {
    ImGui::Begin("Injection", &showInjection_);
    ImGui::TextDisabled("These actions run code inside the target process.");
    helpMarker("Injection can destabilise or crash the target, and anti-cheat software commonly "
               "treats it as an attack. Only use it on software you own or are authorised to modify.");
    ImGui::Separator();

    if (ImGui::BeginTabBar("inject-tabs")) {
        if (ImGui::BeginTabItem("Allocate")) {
            ImGui::InputText("Allocation size", allocSize_.data(), allocSize_.size());
            if (ImGui::Button("Remote allocate RWX")) {
                const auto size = static_cast<std::size_t>(parseAddress(allocSize_.data()).value_or(4096));
                confirmAction(
                    "Allocate executable memory in the target?",
                    "This reserves " + std::to_string(size) + " bytes of read/write/execute memory inside " +
                    domain::narrow(services_.session().processName()) +
                    ". Executable memory in another process is exactly what malware allocates, so security "
                    "software may react. Pointer Lab cannot free it automatically.",
                    "Allocate",
                    [this, size] {
                        auto result = services_.injector().allocate(size, PAGE_EXECUTE_READWRITE);
                        if (result) {
                            notifyInfo("Remote allocation at " + domain::toHex(result.value()) + ".");
                            copyText(threadStart_.data(), threadStart_.size(), domain::toHex(result.value()));
                        } else {
                            notifyError("Allocation failed: " + result.error());
                        }
                    });
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Remote Thread")) {
            ImGui::InputText("Thread start", threadStart_.data(), threadStart_.size());
            ImGui::InputText("Thread parameter", threadParameter_.data(), threadParameter_.size());
            if (ImGui::Button("Create remote thread")) {
                if (auto start = resolveAddress(threadStart_.data())) {
                    const auto param = parseAddress(threadParameter_.data()).value_or(0);
                    const auto startAddress = *start;
                    confirmAction(
                        "Run code in the target process?",
                        "This starts a thread at " + domain::toHex(startAddress) + " inside " +
                        domain::narrow(services_.session().processName()) +
                        ". If that address does not contain valid code the target will almost certainly crash.",
                        "Create thread",
                        [this, startAddress, param] {
                            auto result = services_.injector().createThread(startAddress, param);
                            if (result) {
                                notifyInfo("Remote thread finished with exit code " + std::to_string(result.value()) + ".");
                            } else {
                                notifyError("Remote thread failed: " + result.error());
                            }
                        });
                } else {
                    notifyError("Thread start address is not valid hexadecimal.");
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("LoadLibrary")) {
            ImGui::InputText("DLL path", dllPath_.data(), dllPath_.size());
            if (ImGui::Button("LoadLibraryW injection")) {
                const std::string path = dllPath_.data();
                if (path.empty()) {
                    notifyError("Enter the full path of the DLL to inject.");
                } else {
                    confirmAction(
                        "Inject a DLL into the target?",
                        "This loads\n\n" + path + "\n\ninto " +
                        domain::narrow(services_.session().processName()) +
                        ". The DLL runs with that process's privileges and cannot be unloaded by Pointer Lab.",
                        "Inject",
                        [this, path] {
                            auto result = services_.injector().loadLibrary(domain::widen(path));
                            if (result) {
                                notifyInfo("Injected. LoadLibraryW returned " + std::to_string(result.value()) + ".");
                            } else {
                                notifyError("LoadLibrary injection failed: " + result.error());
                            }
                        });
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void UiApp::renderLuaScannerPanel() {
    ImGui::Begin("Lua Scanner", &showLuaScanner_);
    const auto typeNames = valueTypeNames();

    ImGui::BeginChild("lua-scan-controls", ImVec2(0, 140.0f), true);
    statusPill(services_.luaScanJob().progress().running ? "RUNNING" : "READY",
        services_.luaScanJob().progress().running ? colorFromBytes(51, 94, 120) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    ImGui::TextDisabled("Return a Lua predicate: function(ctx) -> truthy to keep the address.");
    ImGui::SameLine();
    helpMarker("ctx fields: address, value, bytes, hex, type, region_base, region_size. Example: return function(ctx) return ctx.value and ctx.value % 16 == 0 end");
    ImGui::Separator();

    ImGui::SetNextItemWidth(118.0f);
    ImGui::Combo("Type", &luaScanTypeIndex_, typeNames.data(), static_cast<int>(typeNames.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("Stride", &luaScanStride_);
    luaScanStride_ = std::clamp(luaScanStride_, 0, 4096);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputInt("Max results", &luaScanMaxResults_);
    luaScanMaxResults_ = std::clamp(luaScanMaxResults_, 1, 1000000);

    ImGui::Checkbox("Writable only", &luaScanWritableOnly_);
    ImGui::SameLine();
    ImGui::Checkbox("Executable only", &luaScanExecutableOnly_);
    ImGui::SameLine();
    if (ImGui::Button("Start Lua scan")) {
        scripting::LuaScanOptions options;
        options.type = valueTypeFromIndex(luaScanTypeIndex_);
        options.script = luaScanScript_.data();
        options.stride = static_cast<std::size_t>(luaScanStride_);
        options.maxResults = static_cast<std::size_t>(luaScanMaxResults_);
        options.writableOnly = luaScanWritableOnly_;
        options.executableOnly = luaScanExecutableOnly_;
        services_.luaScanJob().start(std::move(options));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        services_.luaScanJob().cancel();
    }
    ImGui::EndChild();

    if (ImGui::BeginTabBar("lua-scanner-tabs")) {
        if (ImGui::BeginTabItem("Predicate")) {
            ImGui::InputTextMultiline("##lua-scan-script", luaScanScript_.data(), luaScanScript_.size(), ImVec2(-1, -1));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Results")) {
            const auto progress = services_.luaScanJob().progress();
            ImGui::ProgressBar(static_cast<float>(progress.fraction), ImVec2(-1, 0), progress.status.c_str());
            if (!progress.error.empty()) {
                ImGui::TextColored(colorFromBytes(235, 116, 91), "%s", progress.error.c_str());
            } else {
                ImGui::TextDisabled("%zu Lua match%s", progress.results, progress.results == 1 ? "" : "es");
            }

            const auto results = services_.luaScanJob().results();
            const auto type = services_.luaScanJob().valueType();
            if (ImGui::BeginTable("lua-scan-results", 4, denseTableFlags | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Bytes");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < results.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::toHex(results[i].address).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::formatValue(type, results[i].current).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(domain::bytesToHex(results[i].current).c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("Add")) {
                        services_.addressList().add(results[i].address, type, "Lua scan result", "Lua Scanner");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("View")) {
                        gotoMemory(results[i].address);
                        copyText(disasmAddress_.data(), disasmAddress_.size(), domain::toHex(results[i].address));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Examples")) {
            ImGui::TextWrapped("Lua Scanner scripts must return a predicate function. The predicate receives ctx and returns true to keep the address.");
            ImGui::Separator();
            ImGui::BulletText("i32 greater than 1000:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.value and ctx.value > 1000\nend");
            ImGui::BulletText("aligned addresses only:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.address % 16 == 0\nend");
            ImGui::BulletText("byte pattern check:");
            ImGui::TextUnformatted("return function(ctx)\n    return ctx.hex:sub(1, 4) == \"9090\"\nend");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void UiApp::renderLuaPanel() {
    ImGui::Begin("Lua Console", &showLuaConsole_);
    if (ImGui::CollapsingHeader("API quick reference")) {
        ImGui::TextWrapped(
            "Target:   processes(), attach(pid), detach(), modules(), regions()\n"
            "Memory:   read(addr [, type]), write(addr, type, value), read_u32(addr), write_u32(addr, value),\n"
            "          read_bytes(addr, n), write_bytes(addr, hex)\n"
            "Scanning: scan_exact(value [, type]), scan_unknown([type]), scan_next(mode [, value]),\n"
            "          scan_wait([ms]), scan_status(), scan_results([max]) -> table, total\n"
            "Pointers: resolve(module, offset, {offsets}) -> address\n"
            "Table:    add_address(addr [, type, description, group]) -> id\n"
            "Target code: alloc(size), thread(start [, param]), loadlibrary(path)\n"
            "\n"
            "Types are the same names the UI uses: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 bytes.\n"
            "Scan modes: exact, unknown, changed, unchanged, increased, decreased.\n"
            "io, package, require, dofile, loadfile and the destructive half of os are removed.\n"
            "Full reference with return values and error behaviour: docs/lua-api.md in the repository.");
    }

    const bool running = lua_.running();
    const float available = ImGui::GetContentRegionAvail().y;
    const float buttonsRowH = ImGui::GetFrameHeightWithSpacing();
    const float splitH = std::max(60.0f, (available - buttonsRowH) * 0.5f);

    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    ImGui::InputTextMultiline("Lua", luaInput_.data(), luaInput_.size(), ImVec2(-1, splitH));
    ImGui::PopFont();

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Run Lua")) {
        if (!lua_.submit(luaInput_.data())) {
            notifyError("A script is already running.");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // Scripts run on their own thread now, so an endless loop is something the
    // user can stop rather than a reason to kill the whole application.
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop")) {
        lua_.cancel();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear output")) {
        luaOutput_.clear();
    }
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("running...");
    }
    ImGui::BeginChild("lua-output", ImVec2(0, splitH), true);
    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    for (const auto& line : luaOutput_) {
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
void UiApp::renderLogPanel() {
    ImGui::Begin("Logs", &showLogs_);

    auto& logger = infra::Logger::instance();
    static const char* levelNames[] = {"trace", "info", "warn", "error"};
    int level = static_cast<int>(logger.minimumLevel());
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Level", &level, levelNames, IM_ARRAYSIZE(levelNames))) {
        logger.setMinimumLevel(static_cast<infra::LogLevel>(level));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##logfilter", "filter", logFilter_.data(), logFilter_.size());

    if (ImGui::Button("Clear")) {
        logger.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open log folder")) {
        const auto folder = logger.path().parent_path();
        // Surfacing the folder beats telling the user a path they then have to
        // type out by hand.
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", logger.path().string().c_str());

    const std::string filter = logFilter_.data();
    const auto records = logger.snapshot();

    ImGui::BeginChild("log-scroll", ImVec2(0, 0), true);
    ImGui::PushFont(monoFont_, monoFont_->LegacySize);
    for (const auto& record : records) {
        if (!filter.empty() && record.message.find(filter) == std::string::npos) {
            continue;
        }
        ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (record.level == infra::LogLevel::Error) {
            colour = colorFromBytes(235, 116, 91);
        } else if (record.level == infra::LogLevel::Warning) {
            colour = colorFromBytes(232, 184, 92);
        }
        ImGui::TextColored(colour, "[%5u] [%s] %s", record.threadId,
                           infra::Logger::levelName(record.level), record.message.c_str());
    }
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ire::ui
