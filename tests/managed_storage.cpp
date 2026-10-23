#include "SharedStorage.h"
#include "FileOperations.h"
#include <File.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>
using namespace iiSocietyContainer;
using namespace iiFileProvider;
void check(bool value,const QString &message){if(!value)throw std::runtime_error(message.toStdString());}
int main(int argc,char**argv)try{
    QCoreApplication app(argc,argv);app.setApplicationName("SocietyIntegration");app.setApplicationVersion("test");
    QTemporaryDir fixture(QDir::currentPath()+"/managed-storage-XXXXXX.societycontainer");
    check(fixture.isValid(),"fixture");auto drive=SocietyDrive::create(fixture.path());check(bool(drive),"create");
    auto storage=SharedStorage::open(fixture.path());check(bool(storage),"open");QString error;
    const auto file=fixture.filePath("Files/original.txt");File::create(file,"original bytes");
    std::stop_source cancelled; cancelled.request_stop();
    check(!storage->recordFile(StoreSection::Files,"original.txt",&error,{},cancelled.get_token()),"cancelled recording rejected");
    check(QFileInfo(file).size()==14,"cancelled recording retains published bytes");
    check(storage->fileTracking(StoreSection::Files,"original.txt").value("status")=="pending","cancelled recording does not create history");
    check(storage->recordFile(StoreSection::Files,"original.txt",&error),error);
    const auto unicodeName=QString::fromUtf8("선택한 모델.txt").normalized(QString::NormalizationForm_D);
    File::create(fixture.filePath("Files/"+unicodeName),"Unicode original");
    check(storage->recordFile(StoreSection::Files,unicodeName,&error),error);
    check(storage->fileTracking(StoreSection::Files,unicodeName,&error).value("status")=="ready","native decomposed Unicode name remains trackable");
    const auto before=storage->fileTracking(StoreSection::Files,"original.txt",&error);
    check(before.value("status")=="ready" && before.value("application")=="SocietyIntegration","tracking and app context");
    {
        auto repository=storage->fileRepository();FileMetadata metadata;metadata.title="Launch campaign";metadata.keywords={"approved"};metadata.rating=5;
        repository->observe("files/original.txt",metadata);
        auto search=storage->searchFiles({{"text","camp"},{"keywords",QJsonArray{"approved"}},{"minimumRating",5}},&error);
        check(error.isEmpty() && search.value("files").toArray().size()==1,"real provider FTS and filters: "+error);
        repository->ontology().apply({OntologyClass{"ProjectAsset","Project","",{FileRepository::fileClass}},OntologyTypeAssignment{OntologyResource::file(before.value("objectKey").toString().toStdString()),"ProjectAsset"},OntologyDirectory{"Projects/Launch","Launch",{"ProjectAsset"}}},repository->ontology().revision(),repository->sessionKey());
        check(storage->searchFiles({{"directory","Projects/Launch"}},&error).value("files").toArray().size()==1,error);
    }
    FileOperations operations(*drive);
    auto rename=operations.perform(FileOperations::Action::Rename,file,"renamed.txt");check(bool(rename),rename.error);
    auto tracked=storage->fileTracking(StoreSection::Files,"renamed.txt",&error);check(tracked.value("objectKey")==before.value("objectKey"),"rename preserves working identity");
    auto duplicate=operations.perform(FileOperations::Action::Duplicate,rename.path);check(bool(duplicate),duplicate.error);
    check(storage->searchFiles({{"keywords",QJsonArray{"approved"}}},&error).value("files").toArray().size()==2,"duplicate keeps classification");
    auto trash=operations.perform(FileOperations::Action::Trash,rename.path);check(bool(trash),trash.error);
    auto removed=operations.perform(FileOperations::Action::Remove,trash.path);check(bool(removed),removed.error);
    check(storage->searchFiles({{"directory","Projects/Launch"}},&error).value("files").toArray().isEmpty(),error);
    {auto repository=storage->fileRepository({},FileRepository::Access::ReadOnly);check(repository->objects().lookup(before.value("objectKey").toString().toStdString())->deleted,"permanent deletion retains tombstone");check(repository->original(before.value("objectKey").toString().toStdString()).record.sha256==before.value("originalSha256").toString().toStdString(),"original survives real operations");}
    const auto retiredId=drive->identifier();const auto hostId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(bool(SocietyDrive::adoptReplicaIdentity(fixture.path(),retiredId,hostId,&error)),error);
    check(SocietyDrive::completeReplica(fixture.path(),hostId,&error),error);
    const auto mirror=SharedStorage::open(fixture.path(),&error);check(bool(mirror),error);
    check(mirror->recordFile(StoreSection::Files,QFileInfo(duplicate.path).fileName(),&error),error);
    {ObjectStore retired(FileRepository::packagePath(std::filesystem::path(fixture.path().toStdU16String()),retiredId.toStdString()),retiredId.toStdString(),ObjectStore::Access::ReadOnly);check(retired.lookup(before.value("objectKey").toString().toStdString())->deleted,"host identity adoption preserves retired history");}
    std::cout<<"Container native storage, file actions, RAW and actual search verified\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
