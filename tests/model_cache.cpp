#include "ModelCache.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <barrier>
#include <atomic>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace fs = std::filesystem;
using namespace iiSocietyContainer;

static void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv) {
    try {
        require(argc == 2, "fixture directory required");
        const auto root = fs::canonical(fs::path(argv[1])) / "fixture.societycontainer";
        fs::create_directory(root);
        bool valid = true;
        ModelCache cache(root, "container-a", "society.agent", "agent/models", [&] { return valid; });
        const ModelCacheKey first{"resource-a", "revision-a", "model-a"};
        auto empty = cache.primary();
        require(empty && !*empty, "new application has no primary model");
        require(bool(cache.select(first)), "select primary model");
        auto selected = cache.primary();
        require(selected && *selected && **selected == first, "selection survives a read");
        ModelCache other(root, "container-a", "other.app", "agent/models", [&] { return valid; });
        require(other.primary() && !*other.primary(), "applications have independent selections");
        ModelCache wrong(root, "container-b", "society.agent", "agent/models", [&] { return valid; });
        require(!wrong.primary(), "different containers cannot reuse selections");
        require(!wrong.select(first), "different containers cannot replace the primary selection");
        const ModelCacheKey bad{"resource", "revision", "../outside"};
        require(!cache.select(bad), "cache entries cannot escape the catalog");
        const ModelCacheKey mixedCase{"resource", "revision", "Primary-Model"};
        require(bool(cache.select(mixedCase)), "cache entry names accept uppercase ASCII");
        require(cache.primary() && **cache.primary() == mixedCase, "mixed-case selection survives a read");
        int builds = 0;
        const auto build = [&](const fs::path &path, std::stop_token) -> ModelCache::Result<void> {
            ++builds; std::ofstream(path / "model.bin", std::ios::binary) << "verified"; return {};
        };
        const auto verify = [](const fs::path &path) { return read(path / "model.bin") == "verified"; };
        auto created = cache.ensure(first, build, verify);
        require(created && !created->hit && builds == 1, "first request publishes a verified result");
        require(fs::is_regular_file(created->path / "model.bin"), "result exists in the model catalog");
        auto hit = cache.ensure(first, build, verify);
        require(hit && hit->hit && builds == 1 && hit->path == created->path, "cache hit does not rebuild");
        const ModelCacheKey distinct{"resource-b", "revision-a", "model-a"};
        require(bool(cache.select(distinct)), "a distinct resource may use identical cached bytes");
        require(cache.primary() && **cache.primary() == distinct, "resource identity remains distinct from content");
        std::ofstream(created->path / "model.bin", std::ios::trunc) << "changed";
        require(!cache.ensure(first, build, verify) && builds == 1, "a corrupt entry is preserved and rejected");
        const ModelCacheKey cancelled{"resource-c", "revision-c", "model-c"};
        std::stop_source stop; stop.request_stop();
        require(!cache.ensure(cancelled, build, verify, stop.get_token()), "cancelled preparation is rejected");
        require(!fs::exists(created->path.parent_path() / "model-c"), "cancelled data is not published");
        const ModelCacheKey failed{"resource-e", "revision-e", "model-e"};
        const auto failedBuild = [](const fs::path &path, std::stop_token) -> ModelCache::Result<void> {
            std::ofstream(path / "model.bin") << "partial"; return std::unexpected("fixture failure");
        };
        require(!cache.ensure(failed, failedBuild, verify), "builder failure is reported");
        require(!fs::exists(created->path.parent_path() / "model-e"), "partial data is never published");
        const ModelCacheKey concurrent{"resource-f", "revision-f", "model-f"};
        std::barrier rendezvous(2);
        std::atomic<int> winners = 0, hits = 0, errors = 0;
        const auto concurrentBuild = [&](const fs::path &path, std::stop_token) -> ModelCache::Result<void> {
            std::ofstream(path / "model.bin") << "verified";
            rendezvous.arrive_and_wait(); return {};
        };
        auto produce = [&] {
            const auto value=cache.ensure(concurrent,concurrentBuild,verify);
            if (!value) ++errors; else if (value->hit) ++hits; else ++winners;
        };
        std::jthread left(produce), right(produce); left.join(); right.join();
        require(errors == 0 && winners == 1 && hits == 1, "concurrent publication must retain one verified entry");
        const auto outside = fs::canonical(fs::path(argv[1]));
        const auto redirectedRoot = outside / "redirected.societycontainer";
        fs::create_directory(redirectedRoot);
        std::error_code linkError;
        fs::create_directory_symlink(outside, redirectedRoot / ".society-runtime", linkError);
        if (!linkError) {
            ModelCache redirected(redirectedRoot, "container-a", "society.agent", "agent/models", [] { return true; });
            require(!redirected.catalog(), "redirected cache ancestry is rejected");
        }
        const ModelCacheKey changed{"resource-d", "revision-d", "model-d"};
        const auto changedBuild = [&](const fs::path &path, std::stop_token) -> ModelCache::Result<void> {
            std::ofstream(path / "model.bin") << "verified"; valid = false; return {};
        };
        require(!cache.ensure(changed, changedBuild, verify), "replacement of the selected container aborts publication");
        require(!fs::exists(created->path.parent_path() / "model-d"), "identity failure publishes nothing");
        std::cout << "Model cache contract passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
