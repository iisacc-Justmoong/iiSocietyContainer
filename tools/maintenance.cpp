#include <StorageMaintenance.h>
#include <charconv>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::string escaped(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output = "\"";
    for (unsigned char c : input) {
        if (c == '\\' || c == '"') { output.push_back('\\'); output.push_back(static_cast<char>(c)); }
        else if (c < 32) { output += "\\u00"; output += hex[c >> 4]; output += hex[c & 15]; }
        else output.push_back(static_cast<char>(c));
    }
    output += '"'; return output;
}
void output(const iiSocietyContainer::StorageMaintenance::Report& report) {
    std::cout << "{\"tempRemoved\":" << report.tempRemoved << ",\"logsRemoved\":" << report.logsRemoved
        << ",\"cacheRemoved\":" << report.cacheRemoved << ",\"bytesRemoved\":" << report.bytesRemoved
        << ",\"skippedBusy\":" << report.skippedBusy << ",\"errors\":[";
    bool first = true;
    for (const auto& error : report.errors) { if (!first) std::cout << ','; first = false; std::cout << escaped(error); }
    std::cout << "]}\n" << std::flush;
}
}
int main(int argc, char** argv) {
    std::filesystem::path root;
    std::string identifier;
    bool once = false, watch = false;
    int interval = 300;
    const auto usage = [] { std::cerr << "Usage: iiSocietyContainerMaintenanceTool --root <absolute directory> --identifier <UUID> --once|--watch [--interval-seconds <1..86400>]\n"; return 2; };
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--once") { if (once) return usage(); once = true; }
        else if (argument == "--watch") { if (watch) return usage(); watch = true; }
        else if ((argument == "--root" || argument == "--identifier" || argument == "--interval-seconds") && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            if (argument == "--root") root = std::filesystem::path(std::u8string(value.begin(), value.end()));
            else if (argument == "--identifier") identifier = value;
            else {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), interval);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || interval < 1 || interval > 86400) return usage();
            }
        } else return usage();
    }
    if (!root.is_absolute() || identifier.empty() || once == watch) return usage();
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    do {
        const auto report = iiSocietyContainer::StorageMaintenance::runOnce(root, identifier);
        output(report);
        if (once || !report.errors.empty()) return report.errors.empty() ? 0 : 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(interval);
        while (!stopped && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (!stopped);
    return 0;
}
