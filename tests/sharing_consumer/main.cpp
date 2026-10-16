#include <FileActions.h>
#include <QCoreApplication>
#include <iostream>
#include <string_view>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (std::string_view(iiSocietyContainer::FileActions::staticMetaObject.className())
        != "iiSocietyContainer::FileActions") return 1;
    std::cout << "sharingAvailable=" << iiSocietyContainer::FileActions::sharingAvailable()
              << "; FileActions loaded without a QML engine\n";
}
