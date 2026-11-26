#include "FileManagerService.h"

FileManagerService::FileManagerService(QObject *parent) : QObject(parent)
{
    new FileManager1(this);
    new DesktopApplication(this);
}
