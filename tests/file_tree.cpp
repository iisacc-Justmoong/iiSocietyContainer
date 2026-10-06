#include <FileTree.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using iiSocietyContainer::FileTree;
namespace fs = std::filesystem;

static void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto fixture = fs::canonical(argv[1]) / ("file-tree-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(fixture);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{fixture};
    try {
        const auto root = fixture / "root";
        const auto outside = fixture / "root-other";
        fs::create_directories(root / "projects" / "empty");
        fs::create_directory(outside);
        const auto unicode = fs::path(u8"자료 Space.txt");
        { std::ofstream file(root / "projects" / unicode); file << "metadata only"; }
        { std::ofstream file(root / "z.txt"); file << "z"; }
        { std::ofstream file(root / ".hidden"); file << "hidden"; }
        { std::ofstream file(outside / "keep.txt"); file << "keep"; }
        const FileTree tree(root);
        check(tree.isValid(), "valid tree root");
        auto snapshot = tree.snapshot();
        check(bool(snapshot), "recursive snapshot");
        check(snapshot.value->relativePath == "." && snapshot.value->parentPath.empty(), "root paths");
        check(snapshot.value->childrenLoaded && snapshot.value->children.size() == 2, "visible children");
        check(snapshot.value->children.front().relativePath == "projects", "directory-first ordering");
        const auto* nested = snapshot.value->find(fs::path("projects") / unicode);
        check(nested && nested->kind == FileTree::Kind::File && nested->size == 13, "unicode file metadata");
        check(nested->parentPath == "projects" && nested->path == root / "projects" / unicode, "child relationships");
        check(nested->lastModified == fs::last_write_time(nested->path), "modification time");
        check(nested->permissions == fs::status(nested->path).permissions(), "permissions");
        const auto* empty = snapshot.value->find("projects/empty");
        check(empty && empty->childrenLoaded && empty->children.empty(), "empty directory is loaded");
        check(!snapshot.value->find(".hidden"), "hidden default");
        auto hidden = tree.children({}, true);
        check(bool(hidden) && hidden.value->size() == 3, "hidden opt-in");
        auto shallow = tree.snapshot({}, {false, 1, 100});
        check(bool(shallow) && !shallow.value->find("projects")->childrenLoaded, "depth cutoff stays explicit");
        check(!shallow.value->find(fs::path("projects") / unicode), "no fake descendants at cutoff");
        auto rootOnly = tree.snapshot({}, {false, 0, 1});
        check(bool(rootOnly) && !rootOnly.value->childrenLoaded, "root-only snapshot");
        check(!tree.snapshot({}, {false, 64, 2}), "entry limit rejects incomplete snapshots");
        check(!tree.snapshot({}, {false, 257, 100}), "unsafe recursion bound rejected");
        check(!tree.snapshot({}, {false, 1, 0}), "zero entry budget rejected");
        check(!tree.children("z.txt") && !tree.entry("missing"), "type and missing-entry errors");
        for (const auto& path : {fs::path("../root-other/keep.txt"), outside, fs::path("projects/../z.txt"),
                                 fs::path("projects/./empty"), fs::path("projects//empty"),
                                 fs::path("scheme:name")})
            check(!tree.entry(path), "strict relative boundary");
#ifdef _WIN32
        check(bool(tree.entry(fs::path(L"projects\\empty"))), "native Windows child separators");
        check(!tree.entry(fs::path(L"projects\\..\\z.txt")), "native Windows traversal rejected");
#else
        check(!tree.entry(fs::path("projects\\empty")), "foreign separator rejected");
#endif
        check(!tree.entry(fs::path(std::string("z.txt\0extra", 11))), "null path rejected");
        // Snapshot ownership and fresh calls must distinguish removed/added items.
        fs::remove(root / "z.txt");
        { std::ofstream file(root / "new.txt"); file << "new"; }
        check(snapshot.value->find("z.txt") != nullptr, "old snapshot remains a value");
        auto refreshed = tree.snapshot();
        check(bool(refreshed) && !refreshed.value->find("z.txt") && refreshed.value->find("new.txt"), "live refresh");
        fs::create_directory(root / fs::path(u8"\u110C\u1161\u1105\u116D Folder"));
        const auto composed = fs::path(u8"자료 Folder");
        if (fs::exists(root / composed)) {
            const auto typed = tree.entry(composed);
            check(bool(typed), "native Unicode spelling lookup");
            const auto names = tree.snapshot();
            check(bool(names) && names.value->find(typed.value->relativePath), "canonical Unicode path matches snapshot");
        }
        std::error_code linkError;
        fs::create_directory_symlink(outside, root / "escape", linkError);
        if (!linkError) {
            fs::create_directory_symlink(root, root / "cycle");
            fs::create_symlink(root / "missing", root / "broken");
            check(!tree.entry("escape") && !tree.entry("escape/keep.txt"), "redirected ancestor rejected");
            auto linked = tree.snapshot({}, {true, 64, 100});
            check(bool(linked) && !linked.value->find("escape") && !linked.value->find("cycle")
                  && !linked.value->find("broken"), "links are excluded and never followed");
            fs::rename(root, fixture / "original");
            fs::create_directory_symlink(outside, root);
            check(!tree.isValid() && !tree.snapshot(), "redirected pinned root rejected");
            fs::remove(root);
        } else {
            fs::remove_all(root);
        }
        { std::ofstream file(root); file << "root replaced by file"; }
        check(!tree.isValid(), "root replaced by regular file rejected");
        check(!FileTree(fixture / "missing").isValid() && !FileTree(root).isValid()
              && !FileTree({}).isValid(), "invalid roots");
        std::cout << "FileTree metadata, hierarchy, limits, refresh, and boundaries passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
