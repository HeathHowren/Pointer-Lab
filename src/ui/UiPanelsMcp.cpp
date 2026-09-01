// The MCP server panel: start it, hand a client the address and token, and watch
// what the agent does.
//
// The request log is not decoration. Every other panel shows the *state* of the
// session, which after an agent has been working is the result of a hundred
// steps nobody watched. This is the only place that says what those steps were,
// and it is the reason someone can hand the session over and still know what
// happened to it.

#include "ui/UiApp.h"
#include "ui/UiInternal.h"

namespace ire::ui {

void UiApp::renderMcpPanel() {
    ImGui::Begin("MCP Server", &showMcp_);

    const bool running = mcpServer_.running();
    statusPill(running ? "LISTENING" : "OFF",
               running ? colorFromBytes(30, 111, 96) : colorFromBytes(63, 75, 88));
    ImGui::SameLine();
    if (running) {
        ImGui::TextDisabled("%s, %llu request(s)", mcpServer_.url().c_str(),
                            static_cast<unsigned long long>(mcpServer_.requestCount()));
    } else {
        ImGui::TextDisabled("%zu tools ready", mcpServer_.tools().tools().size());
    }

    ImGui::Separator();

    if (!running) {
        ImGui::TextWrapped(
            "Lets an AI agent drive this session over the Model Context Protocol: the same attached "
            "process, the same scan and the same address list you are looking at. It is off until you "
            "start it.");
        ImGui::Spacing();
        // Said plainly and before the button rather than in a tooltip after it.
        // Every other destructive action in Pointer Lab asks first; this one asks
        // once, here, and then does not ask again -- so this sentence is doing
        // the work that a confirmation dialog does everywhere else.
        ImGui::PushStyleColor(ImGuiCol_Text, colorFromBytes(224, 170, 80));
        ImGui::TextWrapped(
            "Starting the server is the only thing you will be asked. While it is running, a client "
            "holding the token below can read and write the target's memory, patch its code, set "
            "breakpoints, allocate, inject a DLL and start threads in it, without confirming any of "
            "them. Stop it when you are done.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(scaled(90.0f));
        ImGui::InputInt("##mcpport", &mcpPort_, 0, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        helpMarker("The port on 127.0.0.1 to listen on. 0 lets Windows choose a free one, which is "
                   "the easiest thing to do when 8722 is already taken.\n\n"
                   "The server never binds anything but the loopback address, so nothing outside "
                   "this machine can reach it.");

        ImGui::Spacing();
        if (ImGui::Button("Start server")) {
            if (mcpPort_ < 0 || mcpPort_ > 65535) {
                notifyError("A port has to be between 0 and 65535.");
            } else if (auto started = mcpServer_.start(static_cast<std::uint16_t>(mcpPort_)); !started) {
                notifyError(started.error());
            } else {
                mcpPort_ = static_cast<int>(mcpServer_.port());
                notifyInfo("MCP server listening on " + mcpServer_.url() +
                           ". Anything holding the token can now change this process's target.");
            }
        }
        ImGui::End();
        return;
    }

    // ---------------------------------------------------------------------
    // Running
    // ---------------------------------------------------------------------

    const auto url = mcpServer_.url();
    const auto token = mcpServer_.token();

    // Wide enough for the whole value rather than a fixed width picked by eye. A
    // token clipped to thirty of its thirty-two characters looks like a complete
    // token, and someone reading it off the screen gets a working-looking string
    // that is refused every time.
    const auto fieldWidth = [](const std::string& text) {
        return ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f + scaled(8.0f);
    };

    ImGui::TextUnformatted("Address");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldWidth(url));
    // Read-only rather than a label, so it can be selected and copied. A token
    // that has to be retyped by hand is one that gets shortened.
    std::string urlBuffer = url;
    ImGui::InputText("##mcpurl", urlBuffer.data(), urlBuffer.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##url")) {
        ImGui::SetClipboardText(url.c_str());
        notifyInfo("Copied the server address.");
    }

    ImGui::TextUnformatted("Token  ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldWidth(token));
    std::string tokenBuffer = token;
    ImGui::InputText("##mcptoken", tokenBuffer.data(), tokenBuffer.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##token")) {
        ImGui::SetClipboardText(token.c_str());
        notifyInfo("Copied the session token.");
    }
    ImGui::SameLine();
    helpMarker("A fresh token every time the server starts. A request without it is refused before "
               "anything else is looked at.\n\n"
               "It is the only thing between another program on this machine and a memory-write API, "
               "so it is not written to disk and it does not survive a restart.");

    ImGui::Spacing();
    if (ImGui::SmallButton("Copy claude mcp add command")) {
        // The whole command rather than its parts, because assembling it by hand
        // is where the header quoting goes wrong.
        const auto command = "claude mcp add --transport http pointerlab " + url +
                             " --header \"Authorization: Bearer " + token + "\"";
        ImGui::SetClipboardText(command.c_str());
        notifyInfo("Copied the command. Run it where your agent can see it.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop server")) {
        mcpServer_.stop();
        notifyInfo("MCP server stopped.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // Drained rather than copied, so the server's own buffer cannot grow without
    // bound while this panel is closed -- the same arrangement the Lua console
    // uses for its output.
    for (auto& line : mcpServer_.takeLog()) {
        mcpLog_.push_back(std::move(line));
    }
    // Kept to roughly a screenful of history times ten. Older than that and it is
    // the log file's job.
    constexpr std::size_t maxLines = 500;
    if (mcpLog_.size() > maxLines) {
        mcpLog_.erase(mcpLog_.begin(),
                      mcpLog_.begin() + static_cast<std::ptrdiff_t>(mcpLog_.size() - maxLines));
    }

    ImGui::TextDisabled("Requests");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        mcpLog_.clear();
    }

    if (ImGui::BeginChild("##mcplog", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(mcpLog_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::TextUnformatted(mcpLog_[static_cast<std::size_t>(i)].c_str());
            }
        }
        // Pinned to the bottom while the view is already there, so a live session
        // follows itself without fighting a reader who has scrolled up.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace ire::ui
