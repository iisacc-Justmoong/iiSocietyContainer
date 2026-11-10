#include "MobileStorage.h"
#import <Foundation/Foundation.h>

namespace iiSocietyContainer {
std::optional<std::filesystem::path> MobileStorage::app_group_root(std::string_view identifier) {
    @autoreleasepool {
        NSString* group=[[NSString alloc] initWithBytes:identifier.data()
                                                  length:identifier.size()
                                                encoding:NSUTF8StringEncoding];
        if(!group.length)return {};
        NSURL* url=[[NSFileManager defaultManager] containerURLForSecurityApplicationGroupIdentifier:group];
        if(!url)return {};
        const char* value=url.URLByResolvingSymlinksInPath.fileSystemRepresentation;
        if(!value)return {};
        std::error_code error;
        auto root=std::filesystem::canonical(std::filesystem::path(value),error);
        if(error || !std::filesystem::is_directory(root,error) || error)return {};
        return root;
    }
}
}
