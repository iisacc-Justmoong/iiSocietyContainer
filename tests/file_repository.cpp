#include "FileRepository.h"
#include "src/ManagedOntologyProperties.h"
#include <iiFileProviderStorage.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
using namespace iiSocietyContainer;
using namespace iiFileProvider;
void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool no=false;try{f();}catch(const std::exception&){no=true;}check(no,"expected boundary rejection");}
int main(int argc,char**argv)try{
    static_assert(detail::managedOntologyProperty("society:original"));
    static_assert(detail::managedOntologyProperty("society:collection"));
    static_assert(!detail::managedOntologyProperty("society:unmanaged"));
    check(detail::managedOntologyProperty("society:rating"),"managed ontology properties remain available without runtime initialization");
    const auto root=fs::path(argc>1?argv[1]:".")/("repository-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    native::createDirectories(root/"Files");native::createDirectory(root/"Published");
    {native::OutputFile out(root/"Files/input.txt");out<<"first version";}
    {native::OutputFile out(root/"Files/plain.iiproject");out<<"a regular file, not a directory package";}
    native::createDirectories(root/"Files/Projects/Test.iiproject/assets");
    {native::OutputFile out(root/"Files/Projects/Test.iiproject/manifest.json");out<<"{}";}
    {native::OutputFile out(root/"Files/Projects/Test.iiproject/assets/image.txt");out<<"embedded asset";}
    (void)native::Presentation::waitForIdle(std::chrono::seconds(10));
    bool available=true;std::string key,raw,session,contentRevision;
    const std::vector<FileRepositorySection> sections{{"files",root/"Files"},{"published",root/"Published"}};
    {
        FileRepository repo(root,"fixture",sections,{{"local:test","author","Author"},"device","editing",{"Society","test","project","input",{{"jobId","job-123"}}}},FileRepository::Access::ReadWrite,[&]{return available;});
        auto record=repo.observe("files/input.txt");key=record.key;raw=repo.original(key).record.key;session=record.sessionKey;
        auto resource=repo.resource("files/input.txt");
        check(resource && resource->id==key && resource->location=="files/input.txt" &&
              resource->hashes.at("sha256")==record.sha256 && resource->hashes.at("original.sha256")==record.sha256 &&
              !resource->contentRevision.empty() && !resource->jobIds.empty() &&
              resource->entitlementJson.find("unassigned")!=std::string::npos,
              "file intake did not assign resource identity, revision, location, hash, entitlement and job");
        check(resource->entitlementFile.extension()==".json" && resource->entitlementFileStatus=="ready" &&
              native::File::read(resource->entitlementFile)==resource->entitlementJson,
              "resource entitlement JSON file was not materialized");
        check(std::ranges::find(resource->jobIds,"job-123")!=resource->jobIds.end(),"caller job ID was not retained");
        check(repo.ontology().queryFiles({FileRepository::fileClass,true,
              {{"society:jobId",OntologyValue::text("job-123")}}}).files.size()==1,"job ID is not indexed");
        check(resource->containerId=="fixture" && resource->localLocator==root/"Files/input.txt",
              "resource container and local locator were not retained");
        contentRevision=resource->contentRevision;
        const std::string entitlement=R"({"schemaVersion":1,"status":"declared","grants":[{"subject":"artist","rights":["read"]}]})";
        repo.setEntitlement("files/input.txt",entitlement);
        resource=repo.resource("files/input.txt");
        check(resource->entitlementFileStatus=="ready" && native::File::read(resource->entitlementFile)==entitlement,
              "entitlement JSON file was not updated with the index");
        check(resource->hashes.at("entitlement.sha256")==native::sha256(entitlement),"entitlement digest was not updated");
        const std::string external=R"({"external":"do not overwrite"})";
        native::File::update(resource->entitlementFile,entitlement,external);
        check(repo.resource("files/input.txt")->entitlementFileStatus=="conflict","external entitlement conflict was not reported");
        rejects([&]{repo.observe("files/input.txt");});
        check(native::File::read(resource->entitlementFile)==external,"external entitlement was overwritten");
        native::File::update(resource->entitlementFile,external,entitlement);
        rejects([&]{repo.setEntitlement("files/input.txt","{invalid");});
        rejects([&]{repo.setEntitlement("files/input.txt",R"({"grant":1,"grant":2})");});
        repo.exportEntitlement("files/input.txt",root/"entitlement.json");
        { std::ifstream in(root/"entitlement.json"); const std::string output{std::istreambuf_iterator<char>(in),{}};
          check(output==entitlement,"exported entitlement JSON differs from the stored document"); }
        rejects([&]{repo.exportEntitlement("files/input.txt",root/"entitlement.json");});
        repo.setReplica("files/input.txt",{"replica-1",contentRevision,"device-b:Files/input.txt"});
        repo.setPublication("files/input.txt",{"publication-1",contentRevision,"community:post-1"});
        repo.setResourceAttribute("files/input.txt","workflow:batch","aurora-2026");
        check(repo.findResourceAttribute("workflow:batch","aurora-2026").files.size()==1,
              "resource key/value attribute is not indexed");
        check(repo.resource("files/input.txt")->attributes.at("workflow:batch")=="aurora-2026",
              "resource key/value attribute was not retained");
        repo.setResourceAttribute("files/input.txt","workflow:batch","aurora-2027");
        check(repo.findResourceAttribute("workflow:batch","aurora-2026").files.empty() &&
              repo.findResourceAttribute("workflow:batch","aurora-2027").files.size()==1,
              "resource attribute update left a stale index value");
        const auto beforeRepeatedRelations=repo.ontology().revision();
        repo.setReplica("files/input.txt",{"replica-1",contentRevision,"device-b:Files/input.txt"});
        check(repo.ontology().revision()==beforeRepeatedRelations,"repeated relation created an unnecessary revision");
        rejects([&]{repo.setReplica("files/input.txt",{"replica-bad","","device-b"});});
        resource=repo.resource("files/input.txt");
        check(resource && resource->entitlementJson==entitlement && resource->replicas.size()==1 &&
              resource->replicas[0].revision==contentRevision && resource->publications.size()==1,
              "resource declarations did not persist");
        const auto replicaSearch=repo.ontology().queryFiles({FileRepository::fileClass,true,
            {{"society:replicaId",OntologyValue::text("replica-1")}}});
        check(replicaSearch.files.size()==1 && replicaSearch.files[0].key==key,"replica id is not indexed");
        const auto publicationSearch=repo.ontology().queryFiles({FileRepository::fileClass,true,
            {{"society:publicationId",OntologyValue::text("publication-1")}}});
        check(publicationSearch.files.size()==1 && publicationSearch.files[0].key==key,"publication id is not indexed");
        {
            FileRepository readOnly(root,"fixture",sections,{},FileRepository::Access::ReadOnly);
            const auto persisted=readOnly.resource("files/input.txt");
            check(persisted && persisted->entitlementJson==entitlement && persisted->replicas.size()==1 &&
                  persisted->publications.size()==1,"resource metadata was not persistent");
            rejects([&]{readOnly.setEntitlement("files/input.txt",entitlement);});
        }
        {native::OutputFile out(root/"Files/twin.txt");out<<"first version";}
        check(native::Presentation::waitForIdle(std::chrono::seconds(10)),"twin presentation did not settle");
        const auto twin=repo.observe("files/twin.txt");
        check(twin.key!=key && twin.sha256==record.sha256,
              "distinct resources with equal content were merged");
        {native::OutputFile out(root/"Files/legacy.txt");out<<"legacy content";}
        check(native::Presentation::waitForIdle(std::chrono::seconds(10)),"legacy presentation did not settle");
        const auto legacy=repo.objects().importDocument(root/"Files/legacy.txt","files/legacy.txt",{"legacy.txt"},repo.sessionKey());
        check(!repo.resource("files/legacy.txt"),"unprojected legacy object was treated as current");
        const auto migrated=repo.observe("files/legacy.txt");
        check(migrated.key==legacy.file.key && repo.resource("files/legacy.txt")->entitlementFileStatus=="ready" &&
              repo.original(migrated.key).record.key==legacy.original.record.key,
              "legacy intake did not preserve object identity and original history");
        check(repo.objects().workSession(session).context.document=="input","work context persists");
        check(raw!=key && repo.original(key).record.sha256==record.sha256,"RAW original and mutable head");
        const auto events=repo.objects().journal().size(),sessions=repo.objects().sessions().size(); const auto revision=repo.ontology().revision();
        repo.observe("files/input.txt");
        check(repo.objects().journal().size()==events && repo.objects().sessions().size()==sessions && repo.ontology().revision()==revision,"no-op observation has no mutations");
        FileMetadata metadata;metadata.title="Launch campaign";metadata.keywords={"approved","client"};metadata.rating=5;metadata.color=ColorLabel::Green;
        repo.observe("files/input.txt",metadata);
        check(repo.resource("files/input.txt")->contentRevision==contentRevision,"metadata changed content revision");
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
        check(repo.resource("files/input.txt")->contentRevision!=contentRevision,"new content did not create a content revision");
        check(repo.resource("files/input.txt")->contentRevisionHistory.size()==2,"content revision history was not preserved");
        contentRevision=repo.resource("files/input.txt")->contentRevision;
        check(record.key==key && record.version>1 && !repo.objects().diff(key,record.version).empty(),"native content update journals a binary diff");
        check(repo.original(key).record.key==raw && repo.original(key).record.sha256!=record.sha256,"editing retains first original");
        repo.captureTree("files/input.txt");native::renameFile(root/"Files/input.txt",root/"Published/result.txt");
        repo.relocated("files/input.txt","published/result.txt");
        resource=repo.resource("published/result.txt");
        check(resource && resource->id==key && resource->contentRevision==contentRevision &&
              resource->location=="published/result.txt" && resource->entitlementJson==entitlement &&
              resource->replicas[0].revision!=contentRevision && resource->publications.size()==1 &&
              resource->attributes.at("workflow:batch")=="aurora-2027",
              "move changed resource identity or silently changed pinned relations");
        check(repo.lookup("published/result.txt")->key==key && !repo.lookup("files/input.txt"),"move preserves identity");
        check(repo.ontology().listDirectory("Projects/Launch").files[0].path=="published/result.txt","semantic folder follows move");
        check(repo.original(key).metadata.annotations==metadata,"move preserves classification");
        rejects([&]{repo.removed("published/result.txt");});
        rejects([&]{repo.observe("files/../secret");});
        rejects([&]{repo.observe("files/input.txt.iisacc.json");});
        rejects([&]{repo.observe("files/Projects/Test.iiproject/manifest.json");});
        rejects([&]{repo.captureTree("files/Projects/Test.iiproject");});
        check(!repo.observe("files/plain.iiproject").key.empty(),"a regular file with the suffix remains an ordinary file");
        repo.captureTree("files/Projects");
        check(!repo.lookup("files/Projects/Test.iiproject/manifest.json") && !repo.lookup("files/Projects/Test.iiproject/assets/image.txt"),"package capture never expands internals");
        fs::create_symlink(root/"Published/result.txt",root/"Files/link.txt");
        rejects([&]{repo.observe("files/link.txt");});
        fs::remove(root/"Files/link.txt");
        available=false;rejects([&]{repo.reconcile();});available=true;
        std::stop_source stop;stop.request_stop();rejects([&]{repo.reconcile(stop.get_token());});
        check(!repo.objects().lookup(key)->deleted,"failed reconciliation does not tombstone files");
        native::Presentation::cancelPending(root/"Published/result.txt");fs::remove(root/"Published/result.txt");
        check(repo.findResourceAttribute("workflow:batch","aurora-2027").files.empty(),
              "resource attribute search exposed a deleted native file");
        auto reconciled=repo.reconcile();check(reconciled.deleted==1,"external deletion reaches journal");
        check(!repo.lookup("files/Projects/Test.iiproject/manifest.json") && !repo.lookup("files/Projects/Test.iiproject/assets/image.txt"),"reconciliation never indexes package internals");
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
