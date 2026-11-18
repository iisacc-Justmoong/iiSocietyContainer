#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QPersistentModelIndex>
#include <QStringList>
#include <QVariant>
#include <optional>
#include <utility>
#include <vector>

namespace society_ctest {

// A non-destructive contract observer for the flat, single-column directory
// model exercised by this suite. Failures are recorded inside callbacks and
// asserted by the caller after the operation completes.
class FlatModelContract final {
public:
    explicit FlatModelContract(QAbstractItemModel &model) : model_(model)
    {
        connect(&QAbstractItemModel::rowsAboutToBeInserted, [this](const QModelIndex &parent, int first, int last) {
            begin(Kind::insert, parent, first, last);
        });
        connect(&QAbstractItemModel::rowsInserted, [this](const QModelIndex &parent, int first, int last) {
            finishRows(Kind::insert, parent, first, last);
        });
        connect(&QAbstractItemModel::rowsAboutToBeRemoved, [this](const QModelIndex &parent, int first, int last) {
            begin(Kind::remove, parent, first, last);
        });
        connect(&QAbstractItemModel::rowsRemoved, [this](const QModelIndex &parent, int first, int last) {
            finishRows(Kind::remove, parent, first, last);
        });
        connect(&QAbstractItemModel::rowsAboutToBeMoved,
                [this](const QModelIndex &source, int first, int last, const QModelIndex &destination, int position) {
            begin(Kind::move, source, first, last, destination, position);
        });
        connect(&QAbstractItemModel::rowsMoved,
                [this](const QModelIndex &source, int first, int last, const QModelIndex &destination, int position) {
            finishRows(Kind::move, source, first, last, destination, position);
        });
        connect(&QAbstractItemModel::modelAboutToBeReset, [this] { begin(Kind::reset, {}, 0, 0); });
        connect(&QAbstractItemModel::modelReset, [this] { finishReset(); });
        connect(&QAbstractItemModel::layoutAboutToBeChanged, [this] { begin(Kind::layout, {}, 0, 0); });
        connect(&QAbstractItemModel::layoutChanged, [this] { finishLayout(); });
        connect(&QAbstractItemModel::dataChanged,
                [this](const QModelIndex &first, const QModelIndex &last, const QList<int> &) {
            expect(!pending_, "dataChanged during another model operation");
            expect(validRootIndex(first) && validRootIndex(last) && first.row() <= last.row(),
                   "dataChanged has invalid bounds");
            checkState();
        });
        connect(&QAbstractItemModel::headerDataChanged, [this](Qt::Orientation orientation, int first, int last) {
            const int count = orientation == Qt::Horizontal ? model_.columnCount() : model_.rowCount();
            expect(first >= 0 && last >= first && last < count, "headerDataChanged has invalid bounds");
        });
        connect(&QAbstractItemModel::columnsAboutToBeInserted, [this] { fail("flat model inserted columns"); });
        connect(&QAbstractItemModel::columnsAboutToBeRemoved, [this] { fail("flat model removed columns"); });
        connect(&QAbstractItemModel::columnsAboutToBeMoved, [this] { fail("flat model moved columns"); });
        checkState();
    }

    [[nodiscard]] bool ok() const { return errors_.isEmpty() && !pending_; }
    [[nodiscard]] QString errorString() const
    {
        QStringList errors = errors_;
        if (pending_) errors.append(QStringLiteral("unfinished model operation"));
        return errors.join(QStringLiteral("; "));
    }

private:
    enum class Kind { insert, remove, move, reset, layout };
    struct Row final {
        QPersistentModelIndex index;
        int oldRow = -1;
        QHash<int, QVariant> values;
    };
    struct Pending final {
        Kind kind;
        int first = 0, last = 0, destination = 0, count = 0;
        std::vector<Row> rows;
    };

    template <typename Signal, typename Callback>
    void connect(Signal signal, Callback callback)
    {
        if (!QObject::connect(&model_, signal, &receiver_, std::move(callback)))
            fail("could not observe a model signal");
    }
    void fail(const char *message) { errors_.append(QString::fromLatin1(message)); }
    void expect(bool condition, const char *message) { if (!condition) fail(message); }
    [[nodiscard]] bool validRootIndex(const QModelIndex &index) const
    {
        return index.isValid() && index.model() == &model_ && index.column() == 0 &&
               index.row() >= 0 && index.row() < model_.rowCount() && !index.parent().isValid();
    }
    void checkState()
    {
        const int count = model_.rowCount();
        const int columns = model_.columnCount();
        expect(count >= 0 && count <= 100000, "invalid flat model row count");
        expect(columns == 1, "flat model must expose one column");
        if (count < 0 || count > 100000 || columns != 1) return;
        expect(model_.hasChildren() == (count > 0 || model_.canFetchMore({})), "root child availability is inconsistent");
        expect(!model_.hasIndex(-1, 0) && !model_.hasIndex(count, 0) && !model_.hasIndex(0, 1),
               "out-of-range index was reported valid");
        expect(!model_.index(-1, 0).isValid() && !model_.index(count, 0).isValid() && !model_.index(0, 1).isValid(),
               "out-of-range index was created");
        for (int row = 0; row < count; ++row) {
            const auto index = model_.index(row, 0);
            expect(validRootIndex(index) && index.row() == row && model_.hasIndex(row, 0), "invalid root row index");
            if (!index.isValid()) continue;
            expect(model_.rowCount(index) == 0 && !model_.hasChildren(index) && !model_.index(0, 0, index).isValid(),
                   "flat model row unexpectedly has children");
            expect(index.sibling(row, 0) == index, "row sibling is inconsistent");
        }
    }
    std::vector<Row> snapshot() const
    {
        std::vector<Row> rows;
        const int count = model_.rowCount();
        if (count < 0 || count > 100000) return rows;
        rows.reserve(static_cast<std::size_t>(count));
        const auto roles = model_.roleNames();
        for (int row = 0; row < count; ++row) {
            const auto index = model_.index(row, 0);
            Row item{QPersistentModelIndex(index), row, {}};
            for (auto role = roles.cbegin(); role != roles.cend(); ++role)
                item.values.insert(role.key(), model_.data(index, role.key()));
            rows.push_back(std::move(item));
        }
        return rows;
    }
    void begin(Kind kind, const QModelIndex &parent, int first, int last,
               const QModelIndex &destinationParent = {}, int destination = 0)
    {
        if (pending_) { fail("overlapping model operations"); return; }
        checkState();
        const int count = model_.rowCount();
        expect(!parent.isValid() && !destinationParent.isValid(), "flat model operation has a parent");
        if (kind == Kind::insert)
            expect(first >= 0 && last >= first && first <= count, "invalid insertion range");
        if (kind == Kind::remove || kind == Kind::move)
            expect(first >= 0 && last >= first && last < count, "invalid removal or move range");
        if (kind == Kind::move)
            expect(destination >= 0 && destination <= count && (destination < first || destination > last + 1),
                   "invalid move destination");
        pending_ = Pending{kind, first, last, destination, count, snapshot()};
    }
    void checkRows(const Pending &operation, Kind kind)
    {
        const int width = operation.last - operation.first + 1;
        for (const auto &row : operation.rows) {
            const bool removed = kind == Kind::remove && row.oldRow >= operation.first && row.oldRow <= operation.last;
            if (removed) { expect(!row.index.isValid(), "removed persistent index survived"); continue; }
            int expected = row.oldRow;
            if (kind == Kind::insert && row.oldRow >= operation.first) expected += width;
            if (kind == Kind::remove && row.oldRow > operation.last) expected -= width;
            if (kind == Kind::move) {
                if (row.oldRow >= operation.first && row.oldRow <= operation.last)
                    expected = (operation.destination < operation.first ? operation.destination : operation.destination - width) + row.oldRow - operation.first;
                else if (operation.destination < operation.first && row.oldRow >= operation.destination && row.oldRow < operation.first)
                    expected += width;
                else if (operation.destination > operation.last && row.oldRow > operation.last && row.oldRow < operation.destination)
                    expected -= width;
            }
            expect(row.index.isValid() && row.index.row() == expected, "persistent index moved incorrectly");
            if (!row.index.isValid()) continue;
            for (auto value = row.values.cbegin(); value != row.values.cend(); ++value)
                expect(model_.data(row.index, value.key()) == value.value(), "persistent index changed its data");
        }
    }
    void finishRows(Kind kind, const QModelIndex &parent, int first, int last,
                    const QModelIndex &destinationParent = {}, int destination = 0)
    {
        if (!pending_ || pending_->kind != kind) { fail("model operation completed without matching start"); return; }
        const auto operation = std::move(*pending_); pending_.reset();
        expect(!parent.isValid() && !destinationParent.isValid() && first == operation.first && last == operation.last &&
               (kind != Kind::move || destination == operation.destination), "model operation endpoints changed");
        const int width = last - first + 1;
        const int expected = operation.count + (kind == Kind::insert ? width : kind == Kind::remove ? -width : 0);
        expect(model_.rowCount() == expected, "model row count does not match the operation");
        checkRows(operation, kind); checkState();
    }
    void finishReset()
    {
        if (!pending_ || pending_->kind != Kind::reset) { fail("reset completed without matching start"); return; }
        const auto operation = std::move(*pending_); pending_.reset();
        for (const auto &row : operation.rows) expect(!row.index.isValid(), "reset retained a persistent index");
        checkState();
    }
    void finishLayout()
    {
        if (!pending_ || pending_->kind != Kind::layout) { fail("layout completed without matching start"); return; }
        const auto operation = std::move(*pending_); pending_.reset();
        expect(model_.rowCount() == operation.count, "layout changed the row count");
        for (const auto &row : operation.rows) {
            expect(row.index.isValid(), "layout invalidated a persistent index");
            if (!row.index.isValid()) continue;
            for (auto value = row.values.cbegin(); value != row.values.cend(); ++value)
                expect(model_.data(row.index, value.key()) == value.value(), "layout changed persistent data");
        }
        checkState();
    }

    QAbstractItemModel &model_;
    QObject receiver_;
    QStringList errors_;
    std::optional<Pending> pending_;
};
} // namespace society_ctest
