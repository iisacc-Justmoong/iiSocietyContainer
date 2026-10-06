#include "DirectoryStorage.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    namespace fs = std::filesystem;
    using iiSocietyContainer::DirectoryStorage;
    if (argc != 2) return 1;
    const auto root = fs::canonical(argv[1]) / "directory-storage-native-fixture";
    if (fs::exists(root)) return 2;
    fs::create_directory(root);
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
    const auto check = [](bool valid) { if (!valid) throw std::runtime_error("Directory storage assertion failed"); };
    try {
        check(!DirectoryStorage::prepare("relative"));
        auto result = DirectoryStorage::prepare(root);
        check(result && result->created && result->path == root / "Society");
        check(!fs::exists(root / "Society.societycontainer") && !fs::exists(result->path / "bands"));
        std::ofstream(result->path / ".society-drive.json") << "adapter manifest";
        auto reopened = DirectoryStorage::prepare(result->path);
        check(reopened && !reopened->created && reopened->path == result->path);
        fs::create_directory(result->path / "Files");
        check(!DirectoryStorage::prepare(result->path / "Files"));
        check(!fs::exists(result->path / "Files/Society"));
        fs::create_directory(root / "conflict");
        std::ofstream(root / "conflict/Society") << "preserve";
        check(!DirectoryStorage::prepare(root / "conflict"));
        fs::create_directory(root / "redirect");
        fs::create_directory_symlink(root / "conflict", root / "redirect/Society");
        check(!DirectoryStorage::prepare(root / "redirect"));
        fs::create_directory(root / "legacy");
        fs::create_directory(root / "legacy/bands");
        std::ofstream(root / "legacy/Info.plist") << "disk";
        check(!DirectoryStorage::prepare(root / "legacy"));
        std::cout << "Native directory preparation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 3; }
    return 0;
}
