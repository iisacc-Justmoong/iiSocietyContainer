#include "StorageMaintenance.h"
#include <TemporaryLease.h>
#include "native_symlink.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using iiSocietyContainer::StorageMaintenance;
using iiFileProvider::TemporaryLease;
constexpr auto identity = "ae1b5aa2-6219-45b5-b63a-70532d790027";
void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
void write(const fs::path& path, std::string_view bytes = "temporary") {
    fs::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary);
    file << bytes; check(bool(file), "fixture write failed");
}
void manifest(const fs::path& root) {
    write(root / ".society-drive.json", R"({"type":"SocietyDrive","schemaVersion":1,"identifier":"ae1b5aa2-6219-45b5-b63a-70532d790027","sections":[]})");
}
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const auto fixture = fs::canonical(argv[1]) / ("maintenance-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(fixture);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{fixture};
    try {
        const auto root = fixture / "Society.societycontainer"; manifest(root);
        const auto bare = fixture / "Society"; manifest(bare); write(bare / ".society-write-preserve");
        check(!StorageMaintenance::runOnce(bare, identity).errors.empty() && fs::exists(bare / ".society-write-preserve"), "ordinary directories are never managed");
        for (auto name : {"Asset Library", "Deleted", "Files", "Forked", "Generation History", "Models", "Photos", "Published", "Thinking Space"}) fs::create_directory(root / name);
        auto report = StorageMaintenance::runOnce(root, identity);
        check(report.errors.empty() && report.tempRemoved == 0 && report.cacheRemoved == 0, "empty scan succeeds");
        check(!fs::exists(root / ".iisacc-temporary.lock") && !fs::exists(root / ".society-maintenance"), "empty scan creates no state or lock");
        const auto runtimeTemporary = root / ".society-runtime/tmp";
        fs::create_directories(runtimeTemporary);
        const auto job = runtimeTemporary / (std::string(".society-model-package-") + identity);
        {
            TemporaryLease writer(runtimeTemporary, TemporaryLease::Mode::Shared);
            write(job / "nested/output.safetensors", "staged package");
            check(StorageMaintenance::runOnce(root, identity).skippedBusy == 1 && fs::exists(job / "nested/output.safetensors"), "active private job is preserved");
        }
        check(StorageMaintenance::runOnce(root, identity).tempRemoved == 1 && !fs::exists(job), "orphan private job removed immediately");
        const auto merge = root / "Models/.iild-merge-ab12_cd3";
        write(merge / "model/weights", "merge staging");
        write(root / "Models/.iild-merge-user-document/keep", "user file");
        check(StorageMaintenance::runOnce(root, identity).tempRemoved == 1 && !fs::exists(merge), "owned merge staging removed");
        check(fs::exists(root / "Models/.iild-merge-user-document/keep"), "unknown merge name retained");
        write(root / ".society-write-test"); write(root / "Files/deep/.society-import-test");
        write(root / "Models/nested/.iifile-stage-test"); write(root / "Files/.society-install-test");
        write(root / "Generation History/.society-extract-test");
        write(root / "Files/document.tmp", "user temporary"); write(root / "Files/activity.log", "user log");
        write(root / ".society-objects/objects.sqlite-wal", "required WAL");
        write(root / ".society-objects/staging/snapshot-123-1/payload");
        write(root / ".society-objects/staging/snapshot-unknown/unknown", "preserve unknown");
        write(root / ".society-sync/transfers/" / (std::string(64, 'a') + ".part"));
        write(root / ".society-sync/transfers/provider-ae1b5aa2-6219-45b5-b63a-70532d790027.part");
        write(root / ".society-sync/uploads/ae1b5aa2-6219-45b5-b63a-70532d790027");
        write(root / ".society-sync/capability-ae1b5aa2-6219-45b5-b63a-70532d790027");
        write(root / ".society-sync/capability-ae1b5aa2-6219-45b5-b63a-70532d790027.link");
        write(root / ".society-sync/capability-unknown", "preserve unowned");
        write(root / ".society-sync/capability-ae1b5aa2-6219-45b5-b63a-70532d790027.link.extra", "preserve unowned suffix");
        const std::string finderHeader("\0\0\0\1Bud1", 8);
        write(root / ".DS_Store", finderHeader + "root-cache");
        write(root / "Models/.DS_Store", finderHeader + "model-cache");
        write(root / "Files/.DS_Store", finderHeader + "public-cache");
        write(root / "Generation History/.DS_Store", finderHeader + "other-cache");
        write(root / "Models/nested/.DS_Store", finderHeader + "nested-cache");
        write(root / ".society-sync/transfers/custom.part", "preserve unowned");
        const auto logs = root / ".society-maintenance/logs";
        write(logs / "old.log", "old"); write(logs / "a.log", "a"); write(logs / "z.log", "z");
        write(logs / "notes.txt", "preserve");
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(logs / "old.log", now - std::chrono::hours(1));
        fs::last_write_time(logs / "a.log", now); fs::last_write_time(logs / "z.log", now);
        report = StorageMaintenance::runOnce(root, identity);
        check(report.errors.empty() && report.tempRemoved == 11 && report.logsRemoved == 2 && report.cacheRemoved == 2 && report.bytesRemoved == 140, "owned temporaries, old logs, and recognized root Finder caches removed");
        check(!fs::exists(root / ".DS_Store") && !fs::exists(root / "Models/.DS_Store"), "root and Models Finder caches removed");
        check(fs::exists(root / "Files/.DS_Store") && fs::exists(root / "Generation History/.DS_Store") && fs::exists(root / "Models/nested/.DS_Store"), "public, other-section, and nested Finder caches preserved");
        write(root / ".DS_Store", "user text named DS_Store");
        write(root / "Models/.DS_Store", std::string("\0\0\0\2Bud1", 8));
        check(StorageMaintenance::runOnce(root, identity).cacheRemoved == 0 && fs::exists(root / ".DS_Store") && fs::exists(root / "Models/.DS_Store"), "non-Finder namesakes and wrong header versions preserved");
        {
            TemporaryLease writer(root / "Models", TemporaryLease::Mode::Shared);
            write(root / "Models/.DS_Store", finderHeader);
            report = StorageMaintenance::runOnce(root, identity);
            check(report.cacheRemoved == 0 && report.skippedBusy == 1 && fs::exists(root / "Models/.DS_Store"), "active Models lease protects its cache");
        }
        report = StorageMaintenance::runOnce(root, identity);
        check(report.cacheRemoved == 1 && report.tempRemoved == 0 && report.bytesRemoved == 8, "cache cleanup has a separate count after lease release");
        check(fs::exists(logs / "z.log") && !fs::exists(logs / "a.log") && fs::exists(logs / "notes.txt"), "one deterministic newest log retained");
        check(fs::exists(root / "Files/document.tmp") && fs::exists(root / "Files/activity.log") && fs::exists(root / ".society-objects/objects.sqlite-wal"), "public files and SQLite WAL retained");
        check(fs::exists(root / ".society-objects/staging/snapshot-unknown/unknown") && fs::exists(root / ".society-sync/transfers/custom.part"), "unknown namespace contents retained");
        check(fs::exists(root / ".society-sync/capability-unknown") && fs::exists(root / ".society-sync/capability-ae1b5aa2-6219-45b5-b63a-70532d790027.link.extra"), "capability cleanup requires exact UUID and optional link suffix");
        check(!fs::exists(root / ".society-objects/staging/snapshot-123-1"), "abandoned snapshot directory removed");
        {
            TemporaryLease writer(root / ".society-objects/staging", TemporaryLease::Mode::Shared);
            write(root / ".society-objects/staging/snapshot-124-1/payload");
            report = StorageMaintenance::runOnce(root, identity);
            check(fs::exists(root / ".society-objects/staging/snapshot-124-1/payload") && report.skippedBusy == 1, "active object snapshot protected");
        }
        check(StorageMaintenance::runOnce(root, identity).tempRemoved == 1, "released object snapshot removed immediately");
        {
            TemporaryLease writer(logs, TemporaryLease::Mode::Shared);
            write(logs / "active.log");
            report = StorageMaintenance::runOnce(root, identity);
            check(report.logsRemoved == 0 && report.skippedBusy == 1 && fs::exists(logs / "z.log"), "active log parent defers retention");
        }
        check(StorageMaintenance::runOnce(root, identity).logsRemoved == 1 && fs::exists(logs / "active.log"), "released log parent keeps newest log");
        {
            TemporaryLease writer(root / "Files", TemporaryLease::Mode::Shared);
            check(writer.locked(), "writer lease acquired"); write(root / "Files/.society-write-active");
            report = StorageMaintenance::runOnce(root, identity);
            check(fs::exists(root / "Files/.society-write-active") && report.skippedBusy == 1, "active writer protected");
        }
        report = StorageMaintenance::runOnce(root, identity);
        check(!fs::exists(root / "Files/.society-write-active") && report.tempRemoved == 1, "no age delay after lease release");
        {
            TemporaryLease writer(root / ".society-sync/transfers", TemporaryLease::Mode::Shared);
            write(root / ".society-sync/transfers/" / (std::string(64, 'b') + ".part"));
            check(StorageMaintenance::runOnce(root, identity).skippedBusy == 1, "resumable active transfer protected");
        }
        check(StorageMaintenance::runOnce(root, identity).tempRemoved == 1, "unleased transfer removed immediately");
        {
            TemporaryLease writer(root / ".society-sync", TemporaryLease::Mode::Shared);
            write(root / ".society-sync/capability-be1b5aa2-6219-45b5-b63a-70532d790027.link");
            check(StorageMaintenance::runOnce(root, identity).skippedBusy == 1, "active capability probe protected");
        }
        check(StorageMaintenance::runOnce(root, identity).tempRemoved == 1, "unleased capability probe removed immediately");
        const auto outside = fixture / "outside"; write(outside / ".society-write-keep");
        write(outside / "finder-cache", finderHeader);
        fs::remove(root / ".DS_Store");
        test_support::create_symlink(outside / "finder-cache", root / ".DS_Store");
        test_support::create_symlink(outside, root / "Files/redirect", true);
        test_support::create_symlink(outside / ".society-write-keep", root / "Files/.society-write-link");
        check(StorageMaintenance::runOnce(root, identity).errors.empty(), "links skipped without following them");
        check(fs::exists(outside / ".society-write-keep") && fs::is_symlink(fs::symlink_status(root / "Files/.society-write-link")), "link targets preserved");
        check(fs::exists(outside / "finder-cache") && fs::is_symlink(fs::symlink_status(root / ".DS_Store")), "Finder-named link and its target preserved");
        test_support::create_symlink(root, fixture / "redirect-root", true);
        check(!StorageMaintenance::runOnce(fixture / "redirect-root", identity).errors.empty(), "redirected root rejected");
        write(root / ".society-write-wrong-identity");
        check(!StorageMaintenance::runOnce(root, "be1b5aa2-6219-45b5-b63a-70532d790027").errors.empty() && fs::exists(root / ".society-write-wrong-identity"), "changed identity rejected");
        write(root / ".society-disk.plist", "legacy");
        check(!StorageMaintenance::runOnce(root, identity).errors.empty(), "legacy image rejected"); fs::remove(root / ".society-disk.plist");
        const auto validManifest = root / ".society-drive.json";
        write(validManifest, R"({"type":"SocietyDrive","schemaVersion":1,"identifier":"ae1b5aa2-6219-45b5-b63a-70532d790027","identifier":"ae1b5aa2-6219-45b5-b63a-70532d790027"})");
        check(!StorageMaintenance::runOnce(root, identity).errors.empty(), "duplicate identity keys rejected"); manifest(root);
        StorageMaintenance::monitor(root, identity, std::chrono::seconds(1));
        StorageMaintenance::monitor(root, identity, std::chrono::seconds(1));
        for (int i = 0; i != 60 && fs::exists(root / ".society-write-wrong-identity"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!fs::exists(root / ".society-write-wrong-identity"), "monitor initial scan runs");
        write(root / "Files/.society-write-next-period");
        for (int i = 0; i != 60 && fs::exists(root / "Files/.society-write-next-period"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!fs::exists(root / "Files/.society-write-next-period"), "monitor subsequent scan runs");
        std::cout << "Identity, leases, minimal storage, logs, and periodic maintenance passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 3; }
    return 0;
}
