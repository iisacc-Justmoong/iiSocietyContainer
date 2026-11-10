#include "MobileStorage.h"
#include <iiJson/Json.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
using ii::json::Value;
using iiSocietyContainer::MobileContainer;
using iiSocietyContainer::MobileStorage;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
int publish(Value::Object fields) {
    auto output = ii::json::stringify(Value(std::move(fields)));
    if (!output) { std::cerr << "Cannot encode the storage result\n"; return 1; }
    std::cout << *output << '\n';
    return 0;
}
int run(const std::vector<std::filesystem::path>& arguments) {
    try {
        if (arguments.size() == 2 && arguments[0] == "prepare") {
            const auto result = MobileStorage::prepare(arguments[1]);
            if (!result) { std::cerr << result.error() << '\n'; return 1; }
            return publish({{"root", Value(utf8(result->root))}, {"identifier", Value(result->identifier)}});
        }
        if (arguments.size() == 4 && arguments[0] == "migrate") {
            const auto result = MobileStorage::migrate(MobileContainer{arguments[1], utf8(arguments[2])}, arguments[3]);
            if (!result) { std::cerr << result.error() << '\n'; return 1; }
            return publish({{"copied", Value(result->copied)}, {"already_present", Value(result->already_present)},
                            {"conflicts", Value(result->conflicts)}});
        }
        std::cerr << "Usage: iiSocietyContainerMobileStorageTool prepare APP_DIR | migrate SOURCE ID DESTINATION\n";
        return 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::vector<std::filesystem::path> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    return run(arguments);
}
