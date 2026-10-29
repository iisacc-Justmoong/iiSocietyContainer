#include "SocietyApplication.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileOpenEvent>
#ifdef Q_OS_WIN
#include <QDir>
#include <QSettings>
#endif

namespace iiSocietyContainer {
SocietyApplication::SocietyApplication(QObject *parent) : QObject(parent) {}
SocietyApplication::~SocietyApplication() {
    if (m_listening) QDesktopServices::unsetUrlHandler("society");
}
bool SocietyApplication::openGenerationHistory() {
    return QDesktopServices::openUrl(QUrl(QStringLiteral("society://generation-history")));
}
void SocietyApplication::listen() {
    if (m_listening) return;
    m_listening = true;
    QCoreApplication::instance()->installEventFilter(this);
    QDesktopServices::setUrlHandler("society", this, "handleUrl");
#ifdef Q_OS_WIN
    QSettings protocol(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\society"), QSettings::NativeFormat);
    protocol.setValue(".", "URL:Society");
    protocol.setValue("URL Protocol", "");
    protocol.setValue("shell/open/command/.", QStringLiteral("\"%1\" \"%2\"")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1")));
#endif
    for (const auto &argument : QCoreApplication::arguments())
        if (argument.startsWith("society:", Qt::CaseInsensitive)) handleUrl(QUrl(argument));
}
bool SocietyApplication::handleUrl(const QUrl &url) {
    if (!url.isValid() || url.scheme() != "society" || url.host() != "generation-history"
        || (!url.path().isEmpty() && url.path() != "/") || url.hasQuery() || url.hasFragment()
        || !url.userInfo().isEmpty() || url.port() != -1) return false;
    emit generationHistoryRequested();
    return true;
}
bool SocietyApplication::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FileOpen && handleUrl(static_cast<QFileOpenEvent *>(event)->url())) {
        event->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}
}
