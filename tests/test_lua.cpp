// Lua console tests.
//
// The console used to run scripts inline on the UI thread with no interrupt, so
// "while true do end" wedged the application permanently, and the standard
// library was wide open: a pasted script could read and write files or run
// programs on the machine.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "scripting/LuaConsole.h"
#include "services/RuntimeServices.h"

#include <chrono>
#include <string>
#include <thread>

using namespace ire;
using testsupport::HelperProcess;
using testsupport::needleValue;

namespace {

// Runs a script to completion and returns everything it printed.
std::vector<std::string> run(scripting::LuaConsole& console, const std::string& code,
                             std::chrono::seconds timeout = std::chrono::seconds(30)) {
    std::vector<std::string> output;
    REQUIRE(console.submit(code));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (console.running() && std::chrono::steady_clock::now() < deadline) {
        for (auto& line : console.takeOutput()) {
            output.push_back(std::move(line));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(console.running());
    for (auto& line : console.takeOutput()) {
        output.push_back(std::move(line));
    }
    return output;
}

bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [&needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

// A console needs a full service stack, which owns its own session.
struct Console {
    services::RuntimeServices services;
    scripting::LuaConsole console{services};
};

} // namespace

TEST_CASE("A script runs and returns its output", "[lua]") {
    Console fixture;
    const auto output = run(fixture.console, "print('hello', 42)");
    REQUIRE(output.size() == 1);
    CHECK(output[0] == "hello\t42");
}

// The headline fix: this used to hang the application with no way out.
TEST_CASE("An infinite loop can be cancelled", "[lua]") {
    Console fixture;
    REQUIRE(fixture.console.submit("while true do end"));

    // It must actually still be running, or this proves nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(fixture.console.running());

    fixture.console.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.console.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(fixture.console.running());
    CHECK(anyContains(fixture.console.takeOutput(), "cancelled"));
}

TEST_CASE("The console is usable again after a cancelled script", "[lua]") {
    Console fixture;
    REQUIRE(fixture.console.submit("while true do end"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fixture.console.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.console.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(fixture.console.running());
    (void)fixture.console.takeOutput();

    const auto output = run(fixture.console, "print('still here')");
    REQUIRE(output.size() == 1);
    CHECK(output[0] == "still here");
}

namespace {

// Cancels a running script and waits for it to actually stop.
bool cancelAndWait(scripting::LuaConsole& console, std::chrono::seconds timeout = std::chrono::seconds(5)) {
    console.cancel();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (console.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !console.running();
}

} // namespace

// Cancelling used to raise a Lua error, which pcall catches like any other. The
// flag stayed set, so a loop that caught it just kept erroring every 10 000
// instructions and never stopped -- and a runaway loop is exactly what the Stop
// button is for. Each of these swallows errors as hard as it can.
TEST_CASE("A script cannot pcall its way out of being cancelled", "[lua][cancel]") {
    Console fixture;

    SECTION("a loop that catches every error and carries on") {
        REQUIRE(fixture.console.submit("while true do pcall(function() error('nope') end) end"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(fixture.console.running());
        CHECK(cancelAndWait(fixture.console));
    }
    SECTION("a bare pcall around an infinite loop") {
        REQUIRE(fixture.console.submit("while true do pcall(function() while true do end end) end"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(fixture.console.running());
        CHECK(cancelAndWait(fixture.console));
    }
    SECTION("xpcall with a handler that ignores everything") {
        REQUIRE(fixture.console.submit(
            "while true do xpcall(function() while true do end end, function() return 'ignored' end) end"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(fixture.console.running());
        CHECK(cancelAndWait(fixture.console));
    }

    // And the console still works afterwards, so a hard stop does not cost the
    // user their session.
    (void)fixture.console.takeOutput();
    const auto output = run(fixture.console, "print('still here')");
    REQUIRE(output.size() == 1);
    CHECK(output[0] == "still here");
}

TEST_CASE("A script can see that it has been cancelled", "[lua][cancel]") {
    Console fixture;

    SECTION("cancelled() is false while the script is running normally") {
        const auto output = run(fixture.console, "print(tostring(cancelled()))");
        REQUIRE(output.size() == 1);
        CHECK(output[0] == "false");
    }
    SECTION("check_cancel stops a loop that would otherwise never end") {
        // The loop only leaves through check_cancel: there is no other exit.
        REQUIRE(fixture.console.submit("while true do check_cancel() end"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(fixture.console.running());
        CHECK(cancelAndWait(fixture.console));
    }
    SECTION("check_cancel cannot be caught either") {
        REQUIRE(fixture.console.submit("while true do pcall(check_cancel) end"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(fixture.console.running());
        CHECK(cancelAndWait(fixture.console));
    }
}

// Stop used to stop the script but not the scan it had set going, so the work
// carried on in the background and the next script inherited its results.
TEST_CASE("Cancelling a script also cancels the scan it started", "[lua][cancel][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());

    Console fixture;
    // The script parks in a loop of its own rather than in scan_wait. An
    // unknown-value scan of the helper takes a few milliseconds in a Release
    // build, so a script that scanned, waited and printed had already run to
    // completion before a fixed sleep expired: the cancel then arrived with
    // nothing left to cancel, and the test said so only in Release.
    const auto script = "attach(" + std::to_string(helper.pid()) +
                        ")\n"
                        "scan_unknown('i32')\n"
                        "while true do end\n";
    REQUIRE(fixture.console.submit(script));

    // Wait for the scan to actually be in flight instead of assuming it is.
    const auto inFlight = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!fixture.services.scanJob().progress().running && std::chrono::steady_clock::now() < inFlight) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(fixture.services.scanJob().progress().running);
    REQUIRE(fixture.console.running());

    // The loop has no exit, so only the cancel can end the script.
    REQUIRE(cancelAndWait(fixture.console, std::chrono::seconds(20)));

    // Stopped, not merely orphaned. ScanJob::cancel joins its worker, so the
    // scan is already over by the time cancel returns either way -- the status
    // is what says which of the two ways it ended.
    const auto progress = fixture.services.scanJob().progress();
    CHECK_FALSE(progress.running);
    CHECK(progress.status.find("cancelled") != std::string::npos);
}

// loadlibrary used to return the remote thread's exit code, which is only the
// low 32 bits of the module handle: a number that looks like an address on a
// 64-bit target but is not one.
TEST_CASE("loadlibrary reports the module's real base, not a truncated handle", "[lua][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());

    Console fixture;
    const auto script = "attach(" + std::to_string(helper.pid()) +
                        ")\n"
                        "local ok, base = loadlibrary('winmm.dll')\n"
                        "if not ok then print('LOADFAIL ' .. tostring(base)) return end\n"
                        "local listed = nil\n"
                        "for _, m in ipairs(modules()) do\n"
                        "  if m.name:lower() == 'winmm.dll' then listed = m.base end\n"
                        "end\n"
                        "if listed == nil then print('NOTLISTED')\n"
                        "elseif listed == base then print('MATCH ' .. string.format('%X', base))\n"
                        "else print(string.format('MISMATCH %X vs %X', base, listed)) end\n";

    const auto output = run(fixture.console, script);
    REQUIRE_FALSE(output.empty());
    INFO("script said: " << output.front());

    if (anyContains(output, "LOADFAIL")) {
        SKIP("winmm.dll could not be injected on this machine.");
    }
    // The base loadlibrary hands back is the one the module list reports, which
    // is the whole point: it is usable as an address.
    CHECK(anyContains(output, "MATCH"));
}

TEST_CASE("Only one script runs at a time", "[lua]") {
    Console fixture;
    REQUIRE(fixture.console.submit("while true do end"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(fixture.console.submit("print('nope')"));
    fixture.console.cancel();
}

TEST_CASE("Errors come back with a traceback, not just a message", "[lua]") {
    Console fixture;
    const auto output = run(fixture.console, "local function inner() error('boom') end\ninner()");
    REQUIRE_FALSE(output.empty());
    CHECK(anyContains(output, "boom"));
    CHECK(anyContains(output, "stack traceback"));
}

TEST_CASE("A syntax error is reported rather than swallowed", "[lua]") {
    Console fixture;
    const auto output = run(fixture.console, "this is not lua");
    REQUIRE_FALSE(output.empty());
    CHECK(anyContains(output, "syntax error"));
}

// A script is something a user pastes from the internet. It has no business
// touching the file system or starting programs.
TEST_CASE("The dangerous standard library is removed", "[lua]") {
    Console fixture;
    const auto output = run(fixture.console,
                            "print('io', io)\n"
                            "print('package', package)\n"
                            "print('require', require)\n"
                            "print('dofile', dofile)\n"
                            "print('loadfile', loadfile)\n"
                            "print('os.execute', os.execute)\n"
                            "print('os.remove', os.remove)\n"
                            "print('os.getenv', os.getenv)\n"
                            "print('os.exit', os.exit)\n"
                            "print('os.time', type(os.time))");
    REQUIRE(output.size() == 10);
    CHECK(output[0] == "io\tnil");
    CHECK(output[1] == "package\tnil");
    CHECK(output[2] == "require\tnil");
    CHECK(output[3] == "dofile\tnil");
    CHECK(output[4] == "loadfile\tnil");
    CHECK(output[5] == "os.execute\tnil");
    CHECK(output[6] == "os.remove\tnil");
    CHECK(output[7] == "os.getenv\tnil");
    CHECK(output[8] == "os.exit\tnil");
    // Harmless parts of os survive, so scripts can still time themselves.
    CHECK(output[9] == "os.time\tfunction");
}

TEST_CASE("Typed reads and writes work against a live target", "[lua][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());
    Console fixture;

    const auto address = std::to_string(helper.address());
    const auto output = run(fixture.console,
                            "assert(attach(" + std::to_string(helper.pid()) + "))\n"
                            "print(read(" + address + ", 'i32'))\n"
                            "assert(write(" + address + ", 'i32', 4321))\n"
                            "print(read(" + address + ", 'i32'))\n"
                            "assert(write(" + address + ", 'f32', 1.5))\n"
                            "print(read(" + address + ", 'f32'))\n");

    REQUIRE(output.size() == 3);
    CHECK(output[0] == std::to_string(needleValue));
    CHECK(output[1] == "4321");
    CHECK(output[2] == "1.5");
    // The last write was a float, so the target now holds the bit pattern of
    // 1.5f. Reading it back as an integer proves the write really was 4 bytes
    // of float and not a coerced integer.
    CHECK(helper.get() == 0x3FC00000);
}

TEST_CASE("A script can scan, read the results and edit one", "[lua][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());
    Console fixture;

    // Exactly the workflow that was impossible before: the scan functions were
    // fire-and-forget with no way to see what they found.
    const auto output = run(fixture.console,
                            "assert(attach(" + std::to_string(helper.pid()) + "))\n"
                            "scan_exact(" + std::to_string(needleValue) + ", 'i32')\n"
                            "assert(scan_wait(30000))\n"
                            "local results, total = scan_results(50)\n"
                            "print('found', total)\n"
                            "for _, r in ipairs(results) do\n"
                            "  if r.address == " + std::to_string(helper.address()) + " then\n"
                            "    assert(write(r.address, 'i32', 24680))\n"
                            "    print('edited', r.value)\n"
                            "  end\n"
                            "end\n");

    REQUIRE(output.size() >= 2);
    CHECK(anyContains(output, "found"));
    CHECK(anyContains(output, "edited"));
    CHECK(helper.get() == 24680);
}

TEST_CASE("scan_next narrows a previous scan", "[lua][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());
    Console fixture;

    REQUIRE(fixture.console.submit(
        "assert(attach(" + std::to_string(helper.pid()) + "))\n"
        "scan_exact(" + std::to_string(needleValue) + ", 'i32')\n"
        "assert(scan_wait(30000))\n"
        "local _, first = scan_results(1)\n"
        "print('first', first)\n"));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while (fixture.console.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(fixture.console.running());
    CHECK(anyContains(fixture.console.takeOutput(), "first"));

    // Change it out from under the scan, then filter for exactly that.
    REQUIRE(helper.set(555));
    const auto output = run(fixture.console,
                            "assert(scan_next('exact', 555))\n"
                            "assert(scan_wait(30000))\n"
                            "local results, total = scan_results(50)\n"
                            "print('narrowed', total)\n"
                            "for _, r in ipairs(results) do print(r.address, r.value, r.hex) end\n");
    CHECK(anyContains(output, "narrowed"));
    CHECK(anyContains(output, std::to_string(helper.address())));
}

TEST_CASE("A script can list modules and resolve a pointer chain", "[lua][integration]") {
    HelperProcess helper;
    REQUIRE(helper.ready());
    Console fixture;

    const auto output = run(fixture.console,
                            "assert(attach(" + std::to_string(helper.pid()) + "))\n"
                            "local list = modules()\n"
                            "print('modules', #list > 0)\n"
                            "local found = false\n"
                            "for _, m in ipairs(list) do\n"
                            "  if m.name:lower():find('helper') then found = true end\n"
                            "end\n"
                            "print('helper', found)\n"
                            "print('regions', #regions() > 0)\n"
                            "local addr, err = resolve('nosuch.dll', 0, {0})\n"
                            "print('resolve', addr, err ~= nil)\n");

    CHECK(anyContains(output, "modules\ttrue"));
    CHECK(anyContains(output, "helper\ttrue"));
    CHECK(anyContains(output, "regions\ttrue"));
    CHECK(anyContains(output, "resolve\tnil\ttrue"));
}

TEST_CASE("Unknown types and scan modes are rejected clearly", "[lua]") {
    Console fixture;
    const auto output = run(fixture.console,
                            "local ok, err = pcall(function() return read(0x1000, 'not_a_type') end)\n"
                            "print(ok, err)\n"
                            "local ok2, err2 = pcall(function() return scan_next('sideways') end)\n"
                            "print(ok2, err2)\n");
    REQUIRE(output.size() == 2);
    CHECK(anyContains(output, "not_a_type"));
    CHECK(anyContains(output, "sideways"));
}

TEST_CASE("add_address refuses an unknown type instead of guessing i32", "[lua]") {
    // Quietly substituting i32 would give the entry the wrong width, so it
    // would read and write the wrong number of bytes at that address forever
    // after, with nothing anywhere saying why.
    Console fixture;
    const auto output = run(fixture.console,
                            "local ok, err = pcall(function() return add_address(0x1000, 'nonsense') end)\n"
                            "print(ok, err)\n");
    REQUIRE(output.size() == 1);
    CHECK(anyContains(output, "false"));
    CHECK(anyContains(output, "nonsense"));
}

TEST_CASE("Every scan mode the parser accepts is named in its error message", "[lua]") {
    // The message used to omit "unknown", which the parser has always accepted,
    // so a script author following the error had no way to discover it.
    Console fixture;
    const auto output = run(fixture.console,
                            "local ok, err = pcall(function() return scan_next('sideways') end)\n"
                            "print(err)\n");
    REQUIRE(output.size() == 1);
    for (const auto* mode : {"exact", "unknown", "changed", "unchanged", "increased", "decreased"}) {
        INFO("mode missing from the message: " << mode);
        CHECK(anyContains(output, mode));
    }
}
