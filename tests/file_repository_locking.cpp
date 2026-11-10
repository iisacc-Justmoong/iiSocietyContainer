#include "FileRepository.h"
#include <TemporaryLease.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <iiFileProviderStorage.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace iiSocietyContainer;
using namespace iiFileProvider;
using namespace std::chrono_literals;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) try {
    const auto root = fs::path(argc > 1 ? argv[1] : ".") / "repository-lock";
    native::createDirectories(root / "Files");
    const std::vector<FileRepositorySection> sections{{"files", root / "Files"}};
    const auto path = root / "Files" / "source.txt";
    { native::OutputFile file(path); file << "original"; }
    check(native::Presentation::waitForIdle(10s), "initial presentation did not settle");
    const auto unknown = FileRepository::packagePath(root, "unknown-fixture");
    native::createDirectories(unknown);
    { native::OutputFile file(unknown / "keep.txt"); file << "preserve"; }
    bool unknownRejected = false;
    try { FileRepository invalid(root, "unknown-fixture", sections); }
    catch (const std::exception &) { unknownRejected = true; }
    check(unknownRejected && fs::exists(unknown / "keep.txt")
              && std::distance(fs::directory_iterator(unknown), fs::directory_iterator{}) == 1,
          "an unknown nonempty package was changed or accepted");
    FileRepository repository(root, "lock-fixture", sections);
    const auto original = repository.observe("files/source.txt");
    const auto operations = FileRepository::packagePath(root, "lock-fixture") / "operations";
    // A foreground writer must be able to interrupt cooperative background
    // intake rather than waiting for a multi-gigabyte snapshot to finish.
    std::atomic_bool intakeStarted = false, intakeInterrupted = false, foregroundDone = false, intakeFailed = false;
    std::stop_source cancelIntake;
    std::jthread intake([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections);
            const auto completed = peer.tryBackgroundOperation([&](std::stop_token stop) {
                intakeStarted = true;
                while (!stop.stop_requested()) std::this_thread::sleep_for(5ms);
                intakeInterrupted = true;
            }, cancelIntake.get_token());
            if (completed) intakeFailed = true;
        } catch (const std::exception &) { intakeFailed = true; }
    });
    const auto intakeLimit = std::chrono::steady_clock::now() + 5s;
    while (!intakeStarted && std::chrono::steady_clock::now() < intakeLimit)
        std::this_thread::sleep_for(5ms);
    std::jthread foreground([&] {
        FileRepository peer(root, "lock-fixture", sections);
        peer.withExclusiveOperation([&] { foregroundDone = true; });
    });
    const auto foregroundLimit = std::chrono::steady_clock::now() + 5s;
    while (!foregroundDone && std::chrono::steady_clock::now() < foregroundLimit)
        std::this_thread::sleep_for(5ms);
    const bool responsive = foregroundDone;
    cancelIntake.request_stop();
    intake.join(); foreground.join();
    check(intakeStarted && intakeInterrupted && responsive && !intakeFailed,
          "foreground operation could not preempt background intake");
    std::atomic_bool scanActive = false, scanYielded = false, jobActive = false, jobYielded = false, editDone = false;
    std::stop_source stopLow, stopJob;
    std::jthread lowPriority([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections);
            (void)peer.tryBackgroundOperation([&](std::stop_token token) {
                scanActive = true;
                while (!token.stop_requested()) std::this_thread::sleep_for(5ms);
                scanYielded = true;
            }, stopLow.get_token());
        } catch (const std::exception &) {}
    });
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!scanActive && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    std::jthread userJob([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections, {}, FileRepository::Access::ReadWrite, {},
                                FileRepository::Intent::Background);
            (void)peer.tryUserJobOperation([&](std::stop_token token) {
                jobActive = true;
                while (!token.stop_requested()) std::this_thread::sleep_for(5ms);
                jobYielded = true;
            }, stopJob.get_token());
        } catch (const std::exception &) {}
    });
    deadline = std::chrono::steady_clock::now() + 5s;
    while (!jobActive && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    std::atomic_bool passiveOpened = false;
    std::jthread passive([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections, {}, FileRepository::Access::ReadWrite, {},
                                FileRepository::Intent::Background);
            passiveOpened = true;
        } catch (const std::exception &) {}
    });
    deadline = std::chrono::steady_clock::now() + 30s;
    while (!passiveOpened && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    const bool backgroundOpenRespectedJob = passiveOpened && !jobYielded;
    std::jthread edit([&] {
        FileRepository peer(root, "lock-fixture", sections);
        peer.withExclusiveOperation([&] { editDone = true; });
    });
    deadline = std::chrono::steady_clock::now() + 5s;
    while (!editDone && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    const bool threeTierResponsive = scanActive && scanYielded && jobActive && jobYielded && editDone;
    stopLow.request_stop(); stopJob.request_stop();
    lowPriority.join(); userJob.join(); edit.join(); passive.join();
    check(backgroundOpenRespectedJob, "opening a background repository interrupted a user job");
    check(threeTierResponsive, "user job did not preempt intake or yield to a foreground edit");
    std::stop_source cancelledBackground;
    cancelledBackground.request_stop();
    bool cancelledRejected = false, cancelledEntered = false;
    try {
        (void)repository.tryBackgroundOperation([&](std::stop_token) { cancelledEntered = true; },
                                                cancelledBackground.get_token());
    } catch (const std::exception &) { cancelledRejected = true; }
    check(cancelledRejected && !cancelledEntered, "pre-cancelled background operation ran");
    // Two independent intake callers must not scan the same container at the
    // same time. Waiting for the intake owner must not occupy the write lease.
    const auto intakeDirectory = FileRepository::packagePath(root, "lock-fixture") / "intake";
    std::optional<TemporaryLease> intakeOwner;
    intakeOwner.emplace(intakeDirectory, TemporaryLease::Mode::TryExclusive);
    check(intakeOwner->locked(), "could not own the intake fixture");
    std::stop_source stopWaitingScan;
    std::atomic_bool scanWaiting = false, waitingScanDone = false;
    std::jthread waitingScan([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections);
            scanWaiting = true;
            (void)peer.reconcile(stopWaitingScan.get_token());
        } catch (const std::exception &) {}
        waitingScanDone = true;
    });
    const auto waitingLimit = std::chrono::steady_clock::now() + 5s;
    while (!scanWaiting && std::chrono::steady_clock::now() < waitingLimit) std::this_thread::sleep_for(5ms);
    std::this_thread::sleep_for(100ms);
    const bool queued = !waitingScanDone;
    bool writerEntered = false;
    repository.withExclusiveOperation([&] { writerEntered = true; });
    stopWaitingScan.request_stop();
    waitingScan.join();
    intakeOwner.reset();
    check(scanWaiting && queued && writerEntered && waitingScanDone,
          "duplicate intake blocked foreground work or failed to cancel");
    const auto large = root / "Files" / "large.bin";
    {
        std::ofstream file(large, std::ios::binary);
        file.seekp(256 * 1024 * 1024 - 1);
        file.put('\0');
        check(bool(file), "could not prepare the large intake fixture");
    }
    const auto package = FileRepository::packagePath(root, "lock-fixture");
    std::stop_source stopScan;
    std::atomic_bool scanDone = false;
    std::jthread scanning([&] {
        try { FileRepository peer(root, "lock-fixture", sections); (void)peer.reconcile(stopScan.get_token()); }
        catch (const std::exception &) {}
        scanDone = true;
    });
    bool snapshotSeen = false;
    const auto snapshotLimit = std::chrono::steady_clock::now() + 30s;
    while (!scanDone && !snapshotSeen && std::chrono::steady_clock::now() < snapshotLimit) {
        for (const auto &entry : fs::directory_iterator(package))
            if (entry.is_directory() && entry.path().filename().string().starts_with("snapshot-")) snapshotSeen = true;
        if (!snapshotSeen) std::this_thread::sleep_for(2ms);
    }
    const auto renamed = root / "Files" / "foreground.txt";
    std::atomic_bool movedDuringScan = false;
    std::jthread rename([&] {
        FileRepository peer(root, "lock-fixture", sections);
        peer.withExclusiveOperation([&] {
            native::renameFile(path, renamed);
            peer.relocated("files/source.txt", "files/foreground.txt", false);
            movedDuringScan = true;
        });
    });
    const auto renameLimit = std::chrono::steady_clock::now() + 5s;
    while (!movedDuringScan && std::chrono::steady_clock::now() < renameLimit) std::this_thread::sleep_for(5ms);
    const bool renameResponsive = movedDuringScan;
    stopScan.request_stop();
    scanning.join(); rename.join();
    check(snapshotSeen, "large intake did not reach its snapshot checkpoint");
    check(renameResponsive, "large intake blocked a foreground model operation");
    const auto afterPreemption = repository.lookup("files/foreground.txt");
    check(afterPreemption && afterPreemption->key == original.key,
          "preemption lost the renamed resource identity");
    repository.withExclusiveOperation([&] {
        native::renameFile(renamed, path);
        repository.relocated("files/foreground.txt", "files/source.txt", false);
    });
    fs::remove(large);
    bool nested = false;
    repository.withExclusiveOperation([&] {
        TemporaryLease other(operations, TemporaryLease::Mode::TryExclusive);
        check(!other.locked(), "exclusive callback did not hold the repository lock");
        nested = repository.observe("files/source.txt").key == original.key;
    });
    check(nested, "nested repository operations did not retain the lock");
    bool rejected = false;
    try { repository.withExclusiveOperation([] { throw std::runtime_error("callback failure"); }); }
    catch (const std::exception &) { rejected = true; }
    check(rejected, "callback failure was not returned");
    { TemporaryLease released(operations, TemporaryLease::Mode::TryExclusive);
      check(released.locked(), "callback failure left the repository lock held"); }
    std::stop_source alreadyStopped; alreadyStopped.request_stop();
    bool entered = false;
    rejected = false;
    try { repository.withExclusiveOperation([&] { entered = true; }, alreadyStopped.get_token()); }
    catch (const std::exception &) { rejected = true; }
    check(rejected && !entered, "pre-cancelled callback was executed");
    native::Presentation::cancelPending(root / "Files");
    (void)native::Presentation::waitForIdle(10s);
    fs::remove(path);
    for (const auto &entry : fs::directory_iterator(root / "Files"))
        if (entry.is_regular_file() && native::Presentation::isControlFile(entry.path().filename()))
            fs::remove(entry.path());
    std::optional<TemporaryLease> lock;
    lock.emplace(operations, TemporaryLease::Mode::TryExclusive);
    check(lock->locked(), "could not acquire fixture lock");
    std::stop_source stop;
    std::atomic_bool started = false, finished = false, interrupted = false;
    std::jthread worker([&] {
        try {
            FileRepository peer(root, "lock-fixture", sections);
            started = true;
            (void)peer.reconcile(stop.get_token());
        } catch (const std::exception &) { interrupted = true; }
        finished = true;
    });
    const auto startLimit = std::chrono::steady_clock::now() + 5s;
    while (!started && !finished && std::chrono::steady_clock::now() < startLimit)
        std::this_thread::sleep_for(10ms);
    const auto waitLimit = std::chrono::steady_clock::now() + 2s;
    while (!finished && std::chrono::steady_clock::now() < waitLimit)
        std::this_thread::sleep_for(10ms);
    const bool waitedForLock = !finished;
    stop.request_stop();
    const auto cancelLimit = std::chrono::steady_clock::now() + 5s;
    while (!finished && std::chrono::steady_clock::now() < cancelLimit)
        std::this_thread::sleep_for(10ms);
    const bool cancelledWhileLocked = finished;
    lock.reset();
    worker.join();
    check(started, "reconciler did not start");
    check(waitedForLock, "reconciliation tombstoned a missing file while another operation owned the lock");
    check(cancelledWhileLocked && interrupted, "waiting reconciliation did not respond to cancellation");
    const auto current = repository.objects().lookup(original.key);
    check(current && !current->deleted, "a cancelled reconciliation lost the original record");
    repository.removed("files/source.txt");

    const auto moveSource = root / "Files" / "move.txt";
    const auto staging = root / ".moving-test";
    const auto destination = root / "Files" / "moved.txt";
    { native::OutputFile file(moveSource); file << "move without losing identity"; }
    check(native::Presentation::waitForIdle(10s), "move source presentation did not settle");
    const auto moving = repository.observe("files/move.txt");
    const auto originalRevision = repository.resource("files/move.txt")->contentRevision;
    native::Presentation::cancelPending(root / "Files");
    (void)native::Presentation::waitForIdle(10s);
    std::atomic_bool scanStarted = false, scanFinished = false, scanFailed = false;
    std::optional<std::jthread> scanner;
    bool scanWaited = false;
    repository.withExclusiveOperation([&] {
        native::renameFile(moveSource, staging);
        scanner.emplace([&] {
            try {
                FileRepository peer(root, "lock-fixture", sections);
                scanStarted = true;
                (void)peer.reconcile();
            } catch (const std::exception &) { scanFailed = true; }
            scanFinished = true;
        });
        const auto scanLimit = std::chrono::steady_clock::now() + 5s;
        while (!scanStarted && !scanFinished && std::chrono::steady_clock::now() < scanLimit)
            std::this_thread::sleep_for(10ms);
        const auto moveLimit = std::chrono::steady_clock::now() + 2s;
        while (!scanFinished && std::chrono::steady_clock::now() < moveLimit)
            std::this_thread::sleep_for(10ms);
        scanWaited = scanStarted && !scanFinished;
        native::renameFile(staging, destination);
        repository.relocated("files/move.txt", "files/moved.txt");
    });
    scanner->join();
    check(scanWaited && !scanFailed, "reconciliation did not serialize with the native move");
    const auto moved = repository.lookup("files/moved.txt");
    check(moved && moved->key == moving.key && !moved->deleted, "reconciliation lost the moved object");
    check(repository.resource("files/moved.txt")->contentRevision == originalRevision,
          "a path-only move changed the content revision");
    std::cout << "Repository reconciliation respects the operation lock.\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
