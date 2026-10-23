#include "FileRepository.h"
#include <iiFileProviderStorage.h>
#include <iiFileProviderPresentation.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
using namespace iiSocietyContainer;
using namespace iiFileProvider;
void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool no=false;try{f();}catch(const std::exception&){no=true;}check(no,"expected boundary rejection");}
int main(int argc,char**argv)try{
    const auto root=fs::path(argc>1?argv[1]:".")/("repository-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    native::createDirectories(root/"Files");native::createDirectory(root/"Published");
    {native::OutputFile out(root/"Files/input.txt");out<<"first version";}
    (void)native::Presentation::waitForIdle(std::chrono::seconds(10));
    bool available=true;std::string key,raw,session;
    const std::vector<FileRepositorySection> sections{{"files",root/"Files"},{"published",root/"Published"}};
    {
        FileRepository repo(root,"fixture",sections,{{"local:test","author","Author"},"device","editing",{"Society","test","project","input"}},FileRepository::Access::ReadWrite,[&]{return available;});
        auto record=repo.observe("files/input.txt");key=record.key;raw=repo.original(key).record.key;session=record.sessionKey;
        check(repo.objects().workSession(session).context.document=="input","work context persists");
        check(raw!=key && repo.original(key).record.sha256==record.sha256,"RAW original and mutable head");
        const auto events=repo.objects().journal().size(),sessions=repo.objects().sessions().size(); const auto revision=repo.ontology().revision();
        repo.observe("files/input.txt");
        check(repo.objects().journal().size()==events && repo.objects().sessions().size()==sessions && repo.ontology().revision()==revision,"no-op observation has no mutations");
        FileMetadata metadata;metadata.title="Launch campaign";metadata.keywords={"approved","client"};metadata.rating=5;metadata.color=ColorLabel::Green;
        repo.observe("files/input.txt",metadata);
        check(repo.original(key).metadata.annotations==metadata,"annotations reach RAW journal");
        FileFilter filter;filter.text="camp";filter.allKeywords={"approved"};filter.minimumRating=5;
        check(repo.search("files",filter).files.size()==1,"FTS, keyword and rating use provider index");
        OntologyQuery query{FileRepository::fileClass,true,{{"society:keyword",OntologyValue::text("approved")}}};
        check(repo.ontology().queryFiles(query).files[0].key==key,"semantic classification is searchable");
        repo.ontology().apply({OntologyClass{"ProjectAsset","Project", "", {FileRepository::fileClass}},OntologyTypeAssignment{OntologyResource::file(key),"ProjectAsset"},OntologyDirectory{"Projects/Launch","Launch",{"ProjectAsset"}}},repo.ontology().revision(),repo.sessionKey());
        check(repo.ontology().listDirectory("Projects/Launch").files[0].key==key,"saved semantic directory");
        {native::OutputFile out(root/"Files/input.txt");out<<"second version";}
        (void)native::Presentation::waitForIdle(std::chrono::seconds(10));
        record=repo.observe("files/input.txt");
        check(record.key==key && record.version>1 && !repo.objects().diff(key,record.version).empty(),"native content update journals a binary diff");
        check(repo.original(key).record.key==raw && repo.original(key).record.sha256!=record.sha256,"editing retains first original");
        repo.captureTree("files/input.txt");native::renameFile(root/"Files/input.txt",root/"Published/result.txt");
        repo.relocated("files/input.txt","published/result.txt");
        check(repo.lookup("published/result.txt")->key==key && !repo.lookup("files/input.txt"),"move preserves identity");
        check(repo.ontology().listDirectory("Projects/Launch").files[0].path=="published/result.txt","semantic folder follows move");
        check(repo.original(key).metadata.annotations==metadata,"move preserves classification");
        rejects([&]{repo.removed("published/result.txt");});
        rejects([&]{repo.observe("files/../secret");});
        rejects([&]{repo.observe("files/input.txt.iisacc.json");});
        fs::create_symlink(root/"Published/result.txt",root/"Files/link.txt");
        rejects([&]{repo.observe("files/link.txt");});
        fs::remove(root/"Files/link.txt");
        available=false;rejects([&]{repo.reconcile();});available=true;
        std::stop_source stop;stop.request_stop();rejects([&]{repo.reconcile(stop.get_token());});
        check(!repo.objects().lookup(key)->deleted,"failed reconciliation does not tombstone files");
        native::Presentation::cancelPending(root/"Published/result.txt");fs::remove(root/"Published/result.txt");
        auto reconciled=repo.reconcile();check(reconciled.deleted==1,"external deletion reaches journal");
        check(repo.ontology().listDirectory("Projects/Launch").files.empty(),"tombstones leave semantic folder");
        repo.objects().extract(raw,1,root/"restored.txt");check(fs::file_size(root/"restored.txt")==13,"original remains recoverable after deletion");
        repo.endSession();
    }
    {
        FileRepository repo(root,"fixture",sections,{},FileRepository::Access::ReadOnly);
        check(repo.objects().session(session).endedAtNs>0,"normal session is closed");
        check(repo.original(key).record.key==raw && repo.objects().validate(key,repo.objects().lookup(key)->version),"reopen retains valid history");
        rejects([&]{repo.observe("files/new.txt");});
    }
    {
        FileRepository replacement(root,"replacement-identity",sections);
        check(replacement.objects().count()==0,"new container identity gets an independent package");
        check(fs::exists(FileRepository::packagePath(root,"fixture")/"objects.sqlite3"),"previous identity's journal is retained without moving its WAL");
    }
    native::Presentation::cancelPending(root);fs::remove_all(root);std::cout<<"Society RAW, journal, sessions, metadata and semantic search verified\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
