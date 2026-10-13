#ifndef PIKO_QT_H
#define PIKO_QT_H

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <list>
#include <mutex>
#include <string>
#include <vector>

#define Q_OBJECT
#define signals public
#define slots
#define emit
#define SIGNAL(x) #x
#define SLOT(x) #x

namespace Qt {
enum ConnectionType { AutoConnection, DirectConnection, QueuedConnection };
}

class QThread;

class QString {
public:
    QString() {}
    QString(const char *s) : s_(s ? s : "") {}
    QString(const std::string &s) : s_(s) {}

    QString &operator+=(const QString &o) { s_ += o.s_; return *this; }
    QString &operator+=(const char *o) { s_ += o ? o : ""; return *this; }
    QString &operator+=(char c) { s_ += c; return *this; }
    QString &append(const QString &o) { s_ += o.s_; return *this; }

    static QString number(long n) { return QString(std::to_string(n)); }

    bool isEmpty() const { return s_.empty(); }
    const char *c_str() const { return s_.c_str(); }
    const std::string &toStdString() const { return s_; }

    bool operator<(const QString &o) const { return s_ < o.s_; }
    bool operator==(const QString &o) const { return s_ == o.s_; }
    bool operator!=(const QString &o) const { return s_ != o.s_; }

private:
    std::string s_;
};

class QStringRef {
public:
    QStringRef(const QString &s) : s_(s) {}

    QString toString() const { return s_; }

private:
    QString s_;
};

inline QString operator+(const QString &a, const QString &b)
{
    QString r(a);
    r += b;
    return r;
}

class QObject {
public:
    QObject(QObject * = 0) {}
    virtual ~QObject() {}

    static bool connect(const QObject *, const char *, const QObject *,
                        const char *, Qt::ConnectionType = Qt::AutoConnection)
    {
        return true;
    }

    void moveToThread(QThread *) {}
};

class QMutex {
public:
    void lock() { m_.lock(); }
    void unlock() { m_.unlock(); }

private:
    std::mutex m_;
};

class QMutexLocker {
public:
    explicit QMutexLocker(QMutex *m) : m_(m) { m_->lock(); }
    ~QMutexLocker() { m_->unlock(); }

private:
    QMutex *m_;
};

template <typename E>
class QFlags {
public:
    typedef E enum_type;

    QFlags() : i_(0) {}
    QFlags(E e) : i_(e) {}

    operator int() const { return i_; }

    QFlags operator|(QFlags o) const { return from(i_ | o.i_); }
    QFlags operator|(E e) const { return from(i_ | e); }
    QFlags operator&(E e) const { return from(i_ & e); }
    QFlags &operator|=(QFlags o) { i_ |= o.i_; return *this; }

private:
    static QFlags from(int i) { QFlags f; f.i_ = i; return f; }

    int i_;
};

#define Q_DECLARE_FLAGS(N, E) typedef QFlags<E> N
#define Q_DECLARE_OPERATORS_FOR_FLAGS(N) \
    inline N operator|(N::enum_type a, N::enum_type b) { return N(a) | b; }

class QVariant {
public:
    QVariant() : i_(0) {}
    QVariant(int i) : i_(i) {}

    int toInt() const { return i_; }

private:
    int i_;
};

class QComboBox {
public:
    struct Item {
        QString text;
        QVariant data;
    };

    void clear() { items.clear(); }

    void addItem(const QString &text, const QVariant &data = QVariant())
    {
        Item item = { text, data };
        items.push_back(item);
    }

    void insertItem(int index, const QString &text,
                    const QVariant &data = QVariant())
    {
        Item item = { text, data };
        if (index < 0 || index > (int)items.size())
            index = items.size();
        items.insert(items.begin() + index, item);
    }

    std::vector<Item> items;
};

template <typename T>
class QList : public std::list<T> {
public:
    void append(const T &t) { this->push_back(t); }
};

template <typename T>
int qRegisterMetaType(const char *)
{
    return 0;
}

#endif
