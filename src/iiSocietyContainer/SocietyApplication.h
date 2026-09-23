#pragma once
#include "FileActions.h"
#include <QUrl>

namespace iiSocietyContainer {
// Shared URL contract. Only the Society host installs the incoming handler.
class IISOCIETY_GUI_EXPORT SocietyApplication : public QObject {
    Q_OBJECT
public:
    explicit SocietyApplication(QObject *parent = nullptr);
    ~SocietyApplication() override;
    Q_INVOKABLE bool openGenerationHistory();
    void listen();
public slots:
    bool handleUrl(const QUrl &url);
signals:
    void generationHistoryRequested();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    bool m_listening = false;
};
}
