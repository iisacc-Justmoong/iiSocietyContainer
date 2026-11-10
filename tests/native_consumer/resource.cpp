#include <FileRepository.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace iiSocietyContainer;
namespace native = iiFileProvider::native;

int main(int argc, char **argv) try {
    if (argc != 2) throw std::runtime_error("resource workspace required");
    const auto root = fs::absolute(argv[1]) / "installed.societycontainer";
    native::File::createDirectories(root / "Files");
    native::File::create(root / "Files" / "cover.txt", "installed resource");
    {
        FileRepository repository(root, "installed-consumer", {{"files", root / "Files"}});
        const auto file = repository.observe("files/cover.txt");
        auto resource = repository.resource(file.path);
        if (!resource || resource->id != file.key || resource->entitlementFileStatus != "ready")
            throw std::runtime_error("installed package did not create the resource");
        repository.setEntitlement(file.path, R"({"schemaVersion":1,"grants":[]})");
        repository.setResourceAttribute(file.path, "project", "example");
        repository.setReplica(file.path, {"remote", resource->contentRevision, "device:cover"});
        resource = repository.resource(file.path);
        if (!resource || resource->replicas.size() != 1 || resource->attributes.at("project") != "example" ||
            repository.findResourceAttribute("project", "example").files.size() != 1)
            throw std::runtime_error("installed resource API failed");
        repository.endSession();
    }
    native::Presentation::cancelPending(root);
    std::cout << "installed resource consumer passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
