#include "DirectoryStorage.h"
#include "native_symlink.h"
#include <fstream>
#include <iostream>
#include <chrono>

int main(int argc, char** argv)
{
    namespace fs = std::filesystem;
    using iiSocietyContainer::DirectoryStorage;
    if (argc != 2) return 1;
    const auto root = fs::canonical(argv[1]) / ("directory-storage-native-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (fs::exists(root)) return 2;
    fs::create_directory(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    const auto check = [](bool valid) { if (!valid) throw std::runtime_error("Directory storage assertion failed"); };
    try {
        check(!DirectoryStorage::prepare("relative"));
        auto result = DirectoryStorage::prepare(root);
        check(result && result->created && result->path == root / "Society.societycontainer");
        check(!fs::exists(root / "Society") && !fs::exists(result->path / "bands"));
        check(DirectoryStorage::isPackageDirectory(result->path));
        check(!DirectoryStorage::isPackageDirectory(root));
        std::ofstream(result->path / ".society-drive.json") << "adapter manifest";
        auto reopened = DirectoryStorage::prepare(result->path);
        check(reopened && !reopened->created && reopened->path == result->path);
        fs::create_directory(result->path / "Files");
        check(!DirectoryStorage::prepare(result->path / "Files"));
        check(!fs::exists(result->path / "Files/Society.societycontainer"));
        fs::create_directory(root / "conflict");
        std::ofstream(root / "conflict/Society.societycontainer") << "preserve";
        check(!DirectoryStorage::prepare(root / "conflict"));
        fs::create_directory(root / "redirect");
        test_support::create_symlink(root / "conflict", root / "redirect/Society.societycontainer", true);
        check(!DirectoryStorage::prepare(root / "redirect"));
        test_support::remove_symlink(root / "redirect/Society.societycontainer", true);
        fs::create_directory(root / "legacy");
        fs::create_directory(root / "legacy/bands");
        std::ofstream(root / "legacy/Info.plist") << "disk";
        check(!DirectoryStorage::prepare(root / "legacy"));
        fs::create_directory(root / "Old.societycontainer");
        fs::create_directory(root / "Old.societycontainer/bands");
        std::ofstream(root / "Old.societycontainer/Info.plist") << "old image";
        check(!DirectoryStorage::isPackageDirectory(root / "Old.societycontainer"));
        check(!DirectoryStorage::prepare(root / "Old.societycontainer"));
        fs::create_directory(root / "Bare");
        std::ofstream(root / "Bare/.society-drive.json") << "existing bare source";
        check(!DirectoryStorage::prepare(root / "Bare"));
        check(!fs::exists(root / "Bare/Society.societycontainer"));
        fs::create_directory(result->path / "Nested.societycontainer");
        check(!DirectoryStorage::prepare(result->path / "Nested.societycontainer"));
        std::cout << "Native directory preparation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 3; }
    return 0;
}
