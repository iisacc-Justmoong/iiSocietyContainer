#pragma once
#include "iiSocietyContainerExport.h"
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace iiSocietyContainer {
// QML-facing, read-only container mirror. It owns metadata/Alias values only;
// storage owners supply snapshots through iiSocietyHelper. No filesystem I/O.
class IISOCIETYCONTAINER_EXPORT Container : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString containerId READ containerId NOTIFY stateChanged)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY stateChanged)
    Q_PROPERTY(QString containerPath READ rootPath NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(quint64 revision READ revision NOTIFY stateChanged)
    Q_PROPERTY(QVariantList aliases READ aliases NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recentPublished READ recentPublished NOTIFY stateChanged)
    Q_PROPERTY(QVariantList generationHistory READ generationHistory NOTIFY stateChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
public:
    explicit Container(QObject *parent = nullptr) : QObject(parent) {}
    QString containerId() const { return m_id; }
    QString rootPath() const { return m_root; }
    bool available() const { return m_available; }
    quint64 revision() const { return m_revision; }
    QVariantList aliases() const { return m_aliases; }
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    bool loading() const { return false; }
    QString errorString() const { return m_error; }
    Q_INVOKABLE QVariantMap alias(const QString &key) const;
    QVariantList recentFiles() const;
    QVariantList recentPublished() const;
    QVariantList generationHistory() const;
    Q_INVOKABLE void refresh() { emit refreshRequested(); }
    // Binding adapter ingress, deliberately not invokable by application QML.
    void synchronize(QString id, QString root, QVariantList aliases, bool available,
                     QString error = {});
signals:
    void stateChanged();
    void refreshRequested();
private:
    QVariantList filtered(const QString &section, qsizetype limit) const;
    QString m_id, m_root, m_error, m_query;
    bool m_available = false;
    quint64 m_revision = 0;
    QVariantList m_aliases;
};
}
