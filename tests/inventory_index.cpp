#include "InventoryIndex.h"
#include <FileTree.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace iiSocietyContainer;

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    try {
        require(argc == 2, "fixture directory required");
        const auto root = fs::path(argv[1]) / "inventory.societycontainer";
        fs::create_directories(root / ".society-runtime/monitor/v1");
        fs::create_directories(root / "Files/nested");
        fs::create_directories(root / "Models");
        std::ofstream(root / "Files/nested/example.txt") << "first";
        std::ofstream(root / "Models/weights.safetensors") << "model";
        std::ofstream(root / "Files/.hidden") << "private";
        const std::string id = "11111111-2222-4333-8444-555555555555";
        auto snapshot = [&](const char *key, const char *directory, std::uint64_t fingerprint) {
            auto tree = FileTree(root / directory).snapshot({}, {true, 64, 100000});
            require(bool(tree), "fixture tree must be readable");
            return InventorySection{key, directory, fingerprint, true, std::move(*tree.value)};
        };
        std::vector<InventorySection> sections;
        sections.push_back(snapshot("files", "Files", 1));
        sections.push_back(snapshot("models", "Models", 2));
        // A stopped first run may leave an empty SQLite file before schema commit.
        std::ofstream(InventoryIndex::path(root), std::ios::binary).close();
        auto published = InventoryIndex::publish(root, id, sections);
        require(bool(published) && *published, "first snapshot must recover an uninitialized cache and publish");
        auto folder = InventoryIndex::children(root, id, "Files/nested");
        require(bool(folder) && folder->size() == 1, "nested folder must be indexed");
        const auto original = folder->front();
        require(original.path == "Files/nested/example.txt" && original.size == 5,
                "cached entry must identify the original file");
        require(original.key.size() == 64 && original.revision.size() == 64,
                "lookup and metadata revision keys must be SHA-256 values");
        const auto found = InventoryIndex::find(root, id, original.key);
        require(found && *found && (*found)->path == original.path, "cached files must be addressable by key");
        auto unchanged = InventoryIndex::publish(root, id, sections);
        require(bool(unchanged) && !*unchanged, "unchanged sections must not be rewritten");
        auto incomplete = sections;
        incomplete[0].fingerprint = 7;
        for (auto &child : incomplete[0].tree.children)
            if (child.relativePath == "nested") child.childrenLoaded = false;
        require(!InventoryIndex::publish(root, id, incomplete),
                "a truncated child must not be published as a complete scan");
        require(InventoryIndex::children(root, id, "Files/nested")->size() == 1,
                "a rejected scan must leave the previous snapshot intact");
        auto unsupported = sections;
        unsupported[0].fingerprint = 9;
        FileTree::Entry invalid;
        invalid.relativePath = "unsupported:name";
        invalid.path = root / "Files" / invalid.relativePath;
        invalid.lastModified = fs::file_time_type::clock::now();
        unsupported[0].tree.children.push_back(std::move(invalid));
        require(!InventoryIndex::publish(root, id, unsupported),
                "an unsupported path must not silently produce a complete section");
        require(InventoryIndex::children(root, id, "Files/nested")->size() == 1,
                "a rejected path must not replace the previous snapshot");
        auto cancelled = sections;
        cancelled[0].fingerprint = 8;
        for (int i = 0; i < 514; ++i) {
            FileTree::Entry item;
            item.relativePath = "virtual-" + std::to_string(i) + ".dat";
            item.path = root / "Files" / item.relativePath;
            item.kind = FileTree::Kind::File;
            item.lastModified = fs::file_time_type::clock::now();
            cancelled[0].tree.children.push_back(std::move(item));
        }
        int validations = 0;
        require(!InventoryIndex::publish(root, id, cancelled, [&] { return ++validations < 3; }) && validations == 3,
                "a large section must notice cancellation during publication");
        require(InventoryIndex::children(root, id, "Files/nested")->size() == 1,
                "cancelled publication must roll back its partial changes");
        require(!InventoryIndex::children(root, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "Files"),
                "another container identity must not read this cache");
        require(!InventoryIndex::children(root, id, "../Models"), "traversal must be rejected");
        require(!InventoryIndex::publish(root, id, sections, [] { return false; }),
                "cancelled publications must not replace cached state");
        auto top = InventoryIndex::children(root, id, "Files");
        require(bool(top) && top->size() == 2, "hidden user files and folders must be indexed");
        std::ofstream(root / "Files/nested/example.txt", std::ios::app) << " changed";
        sections[0] = snapshot("files", "Files", 3);
        require(InventoryIndex::publish(root, id, sections).value(), "changed section must publish");
        folder = InventoryIndex::children(root, id, "Files/nested");
        require(folder && folder->size() == 1 && folder->front().key == original.key
                    && folder->front().revision != original.revision,
                "content metadata changes must retain the path key and change revision");
        fs::remove(root / "Files/nested/example.txt");
        sections[0] = snapshot("files", "Files", 4);
        require(InventoryIndex::publish(root, id, sections).value(), "removal must publish");
        folder = InventoryIndex::children(root, id, "Files/nested");
        require(folder && folder->empty(), "removed entries must disappear");
        const auto deleted = InventoryIndex::find(root, id, original.key);
        require(deleted && !*deleted, "removed keys must not resolve");
        sections[0].complete = false;
        require(bool(InventoryIndex::publish(root, id, sections)), "partial status must publish");
        require(!InventoryIndex::children(root, id, "Files"), "partial scans must not masquerade as complete");
        require(bool(InventoryIndex::children(root, id, "Models")), "other complete sections must remain readable");
#ifndef _WIN32
        const auto outside = fs::path(argv[1]) / "outside";
        fs::create_directories(outside);
        fs::remove_all(root / ".society-runtime/monitor/v1");
        fs::create_directory_symlink(outside, root / ".society-runtime/monitor/v1");
        require(!InventoryIndex::publish(root, id, sections), "redirected runtime must not be written");
        require(!fs::exists(outside / "inventory.sqlite"), "redirected destination must remain untouched");
#endif
        std::cout << "Inventory index contract passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
