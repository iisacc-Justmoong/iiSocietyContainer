#include <MobileStorage.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using iiSocietyContainer::MobileStorage;
using iiFileProvider::native::File;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    check(argc == 2, "test requires an isolated resource directory");
    const auto workspace=fs::canonical(argv[1]);
    const auto first=workspace/"first", second=workspace/"second", system=workspace/"system";
    File::createDirectories(first); File::createDirectories(second); File::createDirectories(system);
    const auto local=MobileStorage::prepare(first), other=MobileStorage::prepare(second);
    check(local && other && local->root!=other->root && local->identifier!=other->identifier,
          "apps did not receive independently identified containers");
    check(local->root==first/"Society.societycontainer" && fs::is_directory(local->root/"Files") &&
          fs::is_directory(local->root/"Asset Library") && fs::exists(local->root/".society-drive.json"),
          "the app container is not a complete Society package");
    const auto reopened=MobileStorage::prepare(first);
    check(reopened && reopened->identifier==local->identifier,"reopening replaced the container identity");
    const auto shared=MobileStorage::prepare(system);
    check(bool(shared),"system fixture could not be prepared");
    File::createDirectories(local->root/"Files"/"Projects");
    File::create(local->root/"Files"/"Projects"/"canvas.iisc","local canvas");
    File::createDirectories(shared->root/"Files"/"Projects");
    File::create(shared->root/"Files"/"Projects"/"canvas.iisc","existing canvas");
    File::create(local->root/"Asset Library"/"brush.bin","brush");
    const auto migrated=MobileStorage::migrate(*local,shared->root);
    if(!migrated)throw std::runtime_error("migration failed: "+migrated.error());
    if(migrated->copied!=2 || migrated->conflicts!=1)
        throw std::runtime_error("unexpected migration counts: copied="+std::to_string(migrated->copied)+
                                 " conflicts="+std::to_string(migrated->conflicts)+
                                 " already="+std::to_string(migrated->already_present));
    check(migrated && migrated->copied==2 && migrated->conflicts==1,
          "migration did not copy files and report a collision");
    check(File::read(shared->root/"Files"/"Projects"/"canvas.iisc")=="existing canvas" &&
          File::read(local->root/"Files"/"Projects"/"canvas.iisc")=="local canvas" &&
          File::read(shared->root/"Asset Library"/"brush.bin")=="brush",
          "migration overwrote existing or source data");
    const auto repeated=MobileStorage::migrate(*local,shared->root);
    check(repeated && repeated->copied==0 && repeated->already_present==2,
          "repeated migration duplicated files");
    const auto blocked_root=workspace/"blocked", blocked_target=workspace/"blocked-target";
    File::createDirectories(blocked_root); File::createDirectories(blocked_target);
    const auto blocked=MobileStorage::prepare(blocked_root), target=MobileStorage::prepare(blocked_target);
    check(blocked && target,"WAL fixture could not be prepared");
    File::create(blocked->root/"Files"/"ordinary.txt","preserve");
    File::create(blocked->root/"Files"/"pending.iisc",std::string("SQLite format 3\0",16));
    File::create(blocked->root/"Files"/"pending.iisc-wal","pending data");
    check(!MobileStorage::migrate(*blocked,target->root) &&
              !fs::exists(target->root/"Files"/"ordinary.txt") &&
              File::read(blocked->root/"Files"/"pending.iisc-wal")=="pending data",
          "migration copied files before checking for a pending database transaction");
#ifndef _WIN32
    check(File::remove(blocked->root/"Files"/"pending.iisc-wal"),"could not retire WAL fixture");
    fs::create_symlink(workspace/"outside",blocked->root/"Files"/"redirect");
    check(!MobileStorage::migrate(*blocked,target->root) && !fs::exists(target->root/"Files"/"ordinary.txt"),
          "migration followed a redirected source or published before validation");
#endif
    const auto bad=workspace/"bad"; File::createDirectories(bad/"Society.societycontainer");
    File::create(bad/"Society.societycontainer"/"keep.txt","keep");
    check(!MobileStorage::prepare(bad) && File::read(bad/"Society.societycontainer"/"keep.txt")=="keep",
          "an unrecognized existing package was modified");
}
