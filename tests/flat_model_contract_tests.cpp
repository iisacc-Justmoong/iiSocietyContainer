#include "CTestSupport.h"
#include "FlatModelContract.h"
#include <QAbstractListModel>
#include <QCoreApplication>
#include <QStringList>

class FixtureList final : public QAbstractListModel {
public:
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : values_.size(); }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        return index.isValid() && index.row() >= 0 && index.row() < values_.size() && role == Qt::UserRole + 1
            ? QVariant(values_.at(index.row())) : QVariant{};
    }
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole + 1, "value"}}; }
    void insertAt(int row, const QString &value) {
        beginInsertRows({}, row, row); values_.insert(row, value); endInsertRows();
    }
    void removeAt(int row) { beginRemoveRows({}, row, row); values_.removeAt(row); endRemoveRows(); }
    void moveTo(int from, int destination) {
        beginMoveRows({}, from, from, {}, destination);
        values_.move(from, destination > from ? destination - 1 : destination); endMoveRows();
    }
    void change(int row, const QString &value) { values_[row] = value; emit dataChanged(index(row), index(row), {Qt::UserRole + 1}); }
    void reset() { beginResetModel(); values_.clear(); endResetModel(); }
    void brokenInsert() { beginInsertRows({}, 0, 0); values_.append("first"); values_.append("extra"); endInsertRows(); }
    void invalidChange() { const auto bad = createIndex(99, 0); emit dataChanged(bad, bad, {}); }
    void wrongMove() { beginMoveRows({}, 0, 0, {}, 2); endMoveRows(); }
private:
    QStringList values_;
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner;
    runner.run("acceptsValidListTransitions", [] {
        FixtureList model; society_ctest::FlatModelContract contract(model);
        model.insertAt(0, "a"); model.insertAt(1, "b"); model.insertAt(2, "c");
        model.change(0, "updated"); model.moveTo(0, 3); model.removeAt(1); model.reset();
        CTEST_CHECK_MESSAGE(contract.ok(), qPrintable(contract.errorString()));
    });
    runner.run("rejectsWrongInsertCount", [] {
        FixtureList model; society_ctest::FlatModelContract contract(model);
        model.brokenInsert(); CTEST_CHECK(!contract.ok());
    });
    runner.run("rejectsInvalidDataChange", [] {
        FixtureList model; society_ctest::FlatModelContract contract(model);
        model.insertAt(0, "a"); model.invalidChange(); CTEST_CHECK(!contract.ok());
    });
    runner.run("rejectsMoveWithoutMovingData", [] {
        FixtureList model; society_ctest::FlatModelContract contract(model);
        model.insertAt(0, "a"); model.insertAt(1, "b"); model.wrongMove(); CTEST_CHECK(!contract.ok());
    });
    return runner.result();
}
