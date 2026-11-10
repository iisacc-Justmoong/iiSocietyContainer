#include "MobileStorage.h"
#include "DirectoryStorage.h"
#include <iiFileProviderNative.h>
#include <iiFileProviderNativeDevice.h>
#include <iiFileProviderStorage.h>
#include <iiJson/Json.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace iiSocietyContainer {
namespace fs = std::filesystem;
namespace {
using ii::json::Value;
using iiFileProvider::native::File;
constexpr std::array<std::pair<std::string_view,std::string_view>,9> sections{{
    {"asset-library","Asset Library"},{"deleted","Deleted"},{"files","Files"},
    {"forked","Forked"},{"generation-history","Generation History"},{"models","Models"},
    {"photos","Photos"},{"published","Published"},{"thinking-space","Thinking Space"}}};

const Value& field(const Value& value, std::string_view name) {
    static const Value empty;
    const auto found=value.find(name);
    return found ? found->get() : empty;
}
std::string text(const Value& value) {
    const auto result=value.get<std::string>();
    return result ? result->get() : std::string{};
}
bool schema_one(const Value& value) {
    const auto& v=value.storage();
    if(const auto p=std::get_if<std::int64_t>(&v))return *p==1;
    if(const auto p=std::get_if<std::uint64_t>(&v))return *p==1;
    return false;
}
bool direct_directory(const fs::path& path) {
    std::error_code error;
    if(!path.is_absolute() || path.lexically_normal()!=path ||
       !fs::is_directory(fs::symlink_status(path,error)) || error) return false;
    return fs::canonical(path,error)==path && !error;
}
bool contains_path(const fs::path& root,const fs::path& path) {
    auto r=root.begin(),p=path.begin();
    for(;r!=root.end();++r,++p)if(p==path.end() || *p!=*r)return false;
    return true;
}
bool valid_uuid(std::string_view id) {
    if(id.size()!=36)return false;
    for(std::size_t i=0;i<id.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) { if(id[i]!='-')return false; }
        else if(!std::isxdigit(static_cast<unsigned char>(id[i])))return false;
    }
    return true;
}
std::expected<MobileContainer,std::string> inspect(const fs::path& root, bool require_package) {
    try {
        if(!direct_directory(root) || (require_package && !DirectoryStorage::isPackageDirectory(root)))
            return std::unexpected("The Society container is unavailable or redirected");
        const auto manifest=root/".society-drive.json";
        const auto status=fs::symlink_status(manifest);
        if(!fs::is_regular_file(status) || fs::is_symlink(status))
            return std::unexpected("The Society container manifest is unavailable");
        const auto parsed=ii::json::parse(File::read(manifest,65536));
        const auto display=parsed ? text(field(*parsed,"displayName")) : std::string{};
        if(!parsed || text(field(*parsed,"type"))!="SocietyDrive" ||
           !schema_one(field(*parsed,"schemaVersion")) || (display!="Society" && display!="Society Container"))
            return std::unexpected("The Society container manifest is invalid");
        const auto id=text(field(*parsed,"identifier"));
        const auto list=field(*parsed,"sections").get<Value::Array>();
        if(!valid_uuid(id) || !list || list->get().size()!=sections.size())
            return std::unexpected("The Society container identity or sections are invalid");
        for(std::size_t i=0;i<sections.size();++i) {
            const auto& [key,name]=sections[i]; const auto& item=list->get()[i];
            if(text(field(item,"id"))!=key || text(field(item,"name"))!=name ||
               text(field(item,"path"))!=name || !direct_directory(root/fs::path(name)))
                return std::unexpected("A Society container section is unavailable or redirected");
        }
        return MobileContainer{root,id};
    } catch(const std::exception& error) { return std::unexpected(error.what()); }
}
std::string uuid() {
    std::random_device random;
    std::array<unsigned char,16> bytes{};
    for(auto& byte:bytes)byte=static_cast<unsigned char>(random());
    bytes[6]=(bytes[6]&0x0f)|0x40;bytes[8]=(bytes[8]&0x3f)|0x80;
    std::ostringstream out;out<<std::hex<<std::setfill('0');
    for(std::size_t i=0;i<bytes.size();++i) {
        if(i==4||i==6||i==8||i==10)out<<'-';
        out<<std::setw(2)<<unsigned(bytes[i]);
    }
    return out.str();
}
std::string manifest(const std::string& id) {
    std::string out="{\"type\":\"SocietyDrive\",\"schemaVersion\":1,\"filesLayoutVersion\":1,\"displayName\":\"Society\",\"identifier\":\""+id+"\",\"sections\":[";
    for(const auto& [key,name]:sections) {
        if(out.back()!='[')out+=',';
        out+="{\"id\":\""+std::string(key)+"\",\"name\":\""+std::string(name)+"\",\"path\":\""+std::string(name)+"\"}";
    }
    return out+"]}";
}
bool same_file(const fs::path& left,const fs::path& right) {
    if(!fs::is_regular_file(fs::symlink_status(right)) || fs::is_symlink(fs::symlink_status(right)))return false;
    if(fs::file_size(left)!=fs::file_size(right))return false;
    auto a=File::openRead(left),b=File::openRead(right);
    std::array<char,65536> x{},y{};
    for(;;) {
        const auto n=a->read(x.data(),x.size()),m=b->read(y.data(),y.size());
        if(n<0 || m<0 || n!=m || !std::equal(x.begin(),x.begin()+n,y.begin()))return false;
        if(n==0)return true;
    }
}
bool sqlite_sidecar(const fs::path& path) {
    const auto name=path.filename().string();
    if(!name.ends_with("-wal") && !name.ends_with("-shm"))return false;
    const auto database=path.parent_path()/name.substr(0,name.size()-4);
    const auto status=fs::symlink_status(database);
    if(!fs::is_regular_file(status) || fs::is_symlink(status))return false;
    auto file=File::openRead(database);
    return file->read(16)==std::string("SQLite format 3\0",16);
}
void copy_exclusive(const fs::path& source,const fs::path& target) {
    const auto size=fs::file_size(source);
    const auto modified=fs::last_write_time(source);
    auto in=File::openRead(source);
    iiFileProvider::native::AtomicSave out;out.open(target);
    std::array<char,65536> buffer{};
    for(;;) {
        const auto n=in->read(buffer.data(),buffer.size());
        if(n<0)throw std::runtime_error("Cannot read a mobile storage file");
        if(n==0)break;
        if(out.device().write(buffer.data(),n)!=n)throw std::runtime_error("Cannot copy a mobile storage file");
    }
    out.device().flush(true);
    if(fs::file_size(source)!=size || fs::last_write_time(source)!=modified || !same_file(source,out.stagedPath()))
        throw std::runtime_error("A mobile storage file changed during migration");
    const auto published=out.commit(false);
    if(!published.succeeded)throw std::runtime_error(published.message);
}
}

#if !defined(__APPLE__)
std::optional<fs::path> MobileStorage::app_group_root(std::string_view) { return {}; }
#endif

std::expected<MobileContainer,std::string> MobileStorage::prepare(const fs::path& application_storage) {
    try {
        auto location=DirectoryStorage::prepare(application_storage);
        if(!location)return std::unexpected(location.error());
        if(!location->created)return inspect(location->path,true);
        for(const auto& [key,name]:sections) {
            (void)key;
            std::error_code error;
            if(!iiFileProvider::native::createDirectory(location->path/fs::path(name),error) || error)
                return std::unexpected("Cannot create a mobile Society section");
        }
        File::create(location->path/".society-drive.json",manifest(uuid()));
        return inspect(location->path,true);
    } catch(const std::exception& error) { return std::unexpected(error.what()); }
}

std::expected<MobileMigration,std::string> MobileStorage::migrate(const MobileContainer& source,const fs::path& destination) {
    try {
        auto local=inspect(source.root,true),target=inspect(destination,false);
        if(!local || local->identifier!=source.identifier || !target ||
           contains_path(local->root,target->root) || contains_path(target->root,local->root))
            return std::unexpected("The mobile or Society container is unavailable or changed");
        MobileMigration result;
        // Check all sources before publishing anything, especially SQLite WALs.
        for(const auto& [key,name]:sections) {
            (void)key;
            for(const auto& entry:fs::recursive_directory_iterator(source.root/fs::path(name))) {
                const auto status=entry.symlink_status();
                if(fs::is_symlink(status) || (!fs::is_regular_file(status) && !fs::is_directory(status)))
                    return std::unexpected("An unsupported mobile storage entry was preserved");
                if(fs::is_regular_file(status) && entry.path().filename().string().ends_with("-wal") &&
                   sqlite_sidecar(entry.path()) && fs::file_size(entry.path())>0)
                    return std::unexpected("A mobile database has pending changes and must be closed before migration");
            }
        }
        for(const auto& [key,name]:sections) {
            (void)key;
            const auto from=source.root/fs::path(name),to=destination/fs::path(name);
            for(const auto& entry:fs::recursive_directory_iterator(from)) {
                const auto status=entry.symlink_status();
                if(fs::is_symlink(status))return std::unexpected("A redirected mobile storage entry was preserved");
                const auto relative=entry.path().lexically_relative(from);
                if(relative.empty() || relative.is_absolute())return std::unexpected("Invalid mobile storage path");
                auto output=to/relative;
                if(fs::is_directory(status)) {
                    std::error_code error;
                    (void)iiFileProvider::native::createDirectory(output,error);
                    if(error || !direct_directory(output))return std::unexpected("A Society destination folder is unavailable or redirected");
                    continue;
                }
                if(!fs::is_regular_file(status))return std::unexpected("An unsupported mobile storage entry was preserved");
                if(!direct_directory(output.parent_path()))return std::unexpected("A Society destination folder is redirected");
                const auto filename=entry.path().filename().string();
                if(filename==".iisacc-temporary.lock" ||
                   (filename.starts_with('.') && filename.ends_with(".iisacc-preview")))continue;
                if(sqlite_sidecar(entry.path())) {
                    if(filename.ends_with("-wal") && fs::file_size(entry.path())>0)
                        return std::unexpected("A mobile database has pending changes and must be closed before migration");
                    continue;
                }
                bool collision=false,already=false;
                for(std::size_t attempt=0;fs::exists(fs::symlink_status(output));++attempt) {
                    if(same_file(entry.path(),output)){already=true;break;}
                    collision=true;
                    const auto suffix=" (from "+source.identifier.substr(0,8)+(attempt ? "-"+std::to_string(attempt+1) : "")+")";
                    output=to/relative.parent_path()/(relative.stem().string()+suffix+relative.extension().string());
                }
                if(already){++result.already_present;continue;}
                copy_exclusive(entry.path(),output);
                ++result.copied;if(collision)++result.conflicts;
            }
        }
        return result;
    } catch(const std::exception& error) { return std::unexpected(error.what()); }
}
} // namespace iiSocietyContainer
