#include "Container.h"
#include <QDateTime>
#include <QSet>
#include <QHash>
#include <algorithm>

namespace iiSocietyContainer {
namespace {
// Keep the exported Container layout unchanged: existing native consumers
// construct this QML adapter by value. The QObject owns this private index.
class AliasLookup final : public QObject {
public:
    explicit AliasLookup(QObject *owner) : QObject(owner) {}
    QHash<QString, qsizetype> positions;
};
AliasLookup *lookup(const Container *owner) {
    for (auto *child : owner->children())
        if (auto *index = dynamic_cast<AliasLookup *>(child)) return index;
    return nullptr;
}
}
void Container::synchronize(QString id, QString root, QVariantList aliases,
                           bool available, QString error) {
    QVariantList accepted;
    QSet<QString> keys;
    if (available && !id.isEmpty()) for (const auto &value : aliases) {
        const auto entry = value.toMap();
        const auto key = entry.value("key").toString();
        if (entry.value("containerId").toString() != id || key.isEmpty() || keys.contains(key)
            || key.startsWith('/') || key.contains('\\') || key.contains(QChar::Null)) continue;
        const auto parts = key.split('/');
        if (parts.contains("..") || parts.contains(".") || parts.contains(QString{})) continue;
        keys.insert(key); accepted.append(entry);
    }
    std::sort(accepted.begin(), accepted.end(), [](const QVariant &a, const QVariant &b) {
        const auto left = a.toMap(), right = b.toMap();
        const auto at = left.value("modified").toDateTime(), bt = right.value("modified").toDateTime();
        return at == bt ? left.value("key").toString() < right.value("key").toString() : at > bt;
    });
    if (m_id == id && m_root == root && m_aliases == accepted && m_available == available && m_error == error) return;
    auto *index = lookup(this);
    if (!index) index = new AliasLookup(this);
    QHash<QString, qsizetype> positions;
    positions.reserve(accepted.size());
    for (qsizetype i = 0; i < accepted.size(); ++i)
        positions.insert(accepted[i].toMap().value("key").toString(), i);
    m_id = std::move(id); m_root = std::move(root); m_aliases = std::move(accepted);
    index->positions = std::move(positions);
    m_available = available; m_error = std::move(error); ++m_revision;
    emit stateChanged();
}
void Container::setQuery(const QString &query) {
    if (m_query == query) return;
    m_query = query; emit stateChanged();
}
QVariantMap Container::alias(const QString &key) const {
    if (const auto *index = lookup(this)) {
        const auto position = index->positions.constFind(key);
        return position == index->positions.cend() ? QVariantMap{} : m_aliases[*position].toMap();
    }
    for (const auto &value : m_aliases) if (value.toMap().value("key").toString() == key) return value.toMap();
    return {};
}
QVariantList Container::filtered(const QString &section, qsizetype limit) const {
    QVariantList result;
    for (const auto &value : m_aliases) {
        auto row = value.toMap();
        if (row.value("section").toString() != section || !row.value("exists").toBool()) continue;
        if (section == "generation-history" && !row.value("isImage").toBool()) continue;
        if (!m_query.trimmed().isEmpty() && !row.value("name").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)
            && !row.value("metadata2").toString().contains(m_query.trimmed(), Qt::CaseInsensitive)) continue;
        row.insert("dateText", row.value("modified").toDateTime().toLocalTime().date().toString(Qt::ISODate));
        result.append(row); if (result.size() == limit) break;
    }
    return result;
}
QVariantList Container::recentFiles() const { return filtered("files", 20); }
QVariantList Container::recentPublished() const { return filtered("published", 4); }
QVariantList Container::generationHistory() const { return filtered("generation-history", 20); }
}
