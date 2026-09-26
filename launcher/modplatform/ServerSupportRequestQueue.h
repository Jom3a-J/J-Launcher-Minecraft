// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace ModPlatform {

template <typename Value>
class ServerSupportRequestQueue {
   public:
    using Result = std::optional<Value>;
    using Completion = std::function<void(Result)>;
    using Cancel = std::function<void()>;
    using Starter = std::function<Cancel(Completion)>;
    using Callback = std::function<void(Result)>;

    explicit ServerSupportRequestQueue(int concurrency) : m_concurrency(concurrency) {}

    void request(const QString& key, QObject* owner, Starter starter, Callback callback)
    {
        if (m_cache.contains(key)) {
            const Value value = m_cache.value(key);
            QTimer::singleShot(0, owner, [callback = std::move(callback), value] { callback(value); });
            return;
        }

        auto entry = m_entries.value(key);
        if (!entry) {
            entry = std::make_shared<Entry>();
            entry->key = key;
            entry->starter = std::move(starter);
            m_entries.insert(key, entry);
            m_pending.enqueue(key);
        }
        entry->waiters.append({ owner, std::move(callback) });
        pump();
    }

    void cancelOwner(QObject* owner)
    {
        QStringList removeKeys;
        QList<Cancel> cancelRequests;
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            const auto entry = it.value();
            for (int i = entry->waiters.size(); i-- > 0;) {
                if (entry->waiters.at(i).owner == owner) entry->waiters.removeAt(i);
            }
            if (!entry->waiters.isEmpty()) continue;
            if (entry->active) {
                entry->cancelled = true;
                if (entry->cancel) {
                    cancelRequests.append(std::move(entry->cancel));
                }
                removeKeys.append(entry->key);
            } else {
                removeKeys.append(it.key());
                while (m_pending.removeOne(it.key())) {}
            }
        }
        for (const auto& key : removeKeys) m_entries.remove(key);
        for (const auto& cancel : cancelRequests) cancel();
        pump();
    }

    int activeCount() const { return m_active; }
    int pendingCount() const { return m_pending.size(); }
    int cachedCount() const { return m_cache.size(); }
    int concurrencyLimit() const { return m_concurrency; }
    void cacheValue(const QString& key, const Value& value) { m_cache.insert(key, value); }

   private:
    struct Waiter {
        QPointer<QObject> owner;
        Callback callback;
    };
    struct Entry {
        QString key;
        Starter starter;
        QList<Waiter> waiters;
        Cancel cancel;
        bool active = false;
        bool cancelled = false;
        bool completed = false;
    };

    void pump()
    {
        while (m_active < m_concurrency && !m_pending.isEmpty()) {
            const QString key = m_pending.dequeue();
            const auto entry = m_entries.value(key);
            if (!entry || entry->waiters.isEmpty()) continue;
            entry->active = true;
            ++m_active;
            entry->cancel = entry->starter([this, key, weakEntry = std::weak_ptr<Entry>(entry)](Result result) mutable {
                const auto completed = weakEntry.lock();
                if (!completed || completed->completed) return;
                completed->completed = true;
                if (m_entries.value(key) == completed) m_entries.remove(key);
                if (!completed->cancelled && result.has_value()) m_cache.insert(key, *result);
                --m_active;
                const auto waiters = std::move(completed->waiters);
                for (const auto& waiter : waiters) {
                    if (!completed->cancelled && waiter.owner) waiter.callback(result);
                }
                pump();
            });
        }
    }

    const int m_concurrency;
    int m_active = 0;
    QQueue<QString> m_pending;
    QHash<QString, std::shared_ptr<Entry>> m_entries;
    QHash<QString, Value> m_cache;
};

}  // namespace ModPlatform
