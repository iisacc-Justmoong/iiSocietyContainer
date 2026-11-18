#pragma once

#include "QtEventWait.h"
#include <QObject>
#include <QList>
#include <QVariant>
#include <type_traits>
#include <utility>

namespace society_ctest {
template <typename Signature> class SignalRecorder;

template <typename Sender, typename... Args>
class SignalRecorder<void (Sender::*)(Args...)> final {
public:
    using Signal = void (Sender::*)(Args...);
    template <typename Object>
    SignalRecorder(Object* sender, Signal signal)
    {
        if (!QObject::connect(sender, signal, &receiver_, [this](Args... args) {
            rows_.append(QVariantList{QVariant::fromValue(std::decay_t<Args>(args))...});
        })) throw std::runtime_error("Failed to connect signal recorder");
    }
    [[nodiscard]] int size() const { return rows_.size(); }
    [[nodiscard]] int count() const { return rows_.size(); }
    [[nodiscard]] bool isEmpty() const { return rows_.isEmpty(); }
    [[nodiscard]] const QVariantList& operator[](int index) const { return rows_.at(index); }
    [[nodiscard]] const QVariantList& at(int index) const { return rows_.at(index); }
    [[nodiscard]] const QVariantList& first() const { return rows_.first(); }
    [[nodiscard]] const QVariantList& last() const { return rows_.last(); }
    [[nodiscard]] auto begin() const { return rows_.cbegin(); }
    [[nodiscard]] auto end() const { return rows_.cend(); }
    bool wait(int timeoutMs = 5000)
    {
        const int before = size();
        return society_ctest::waitFor([&] { return size() > before; }, timeoutMs);
    }
private:
    QList<QVariantList> rows_;
    QObject receiver_;
};

template <typename Object, typename Sender, typename... Args>
SignalRecorder(Object*, void (Sender::*)(Args...)) -> SignalRecorder<void (Sender::*)(Args...)>;

// Some Qt model signals carry a private marker type. Counting needs no access
// to that marker and can use a zero-argument receiver.
class SignalCounter final {
public:
    template <typename Object, typename Signal>
    SignalCounter(Object *sender, Signal signal)
    {
        if (!QObject::connect(sender, signal, &receiver_, [this] { ++count_; }))
            throw std::runtime_error("Failed to connect signal counter");
    }
    [[nodiscard]] int count() const { return count_; }
    [[nodiscard]] int size() const { return count_; }
    [[nodiscard]] bool isEmpty() const { return count_ == 0; }
private:
    int count_ = 0;
    QObject receiver_;
};
} // namespace society_ctest
