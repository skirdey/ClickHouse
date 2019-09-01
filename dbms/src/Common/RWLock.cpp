#include "RWLock.h"
#include <Common/Stopwatch.h>
#include <Common/Exception.h>
#include <Common/CurrentMetrics.h>
#include <Common/ProfileEvents.h>

#include <cassert>


namespace ProfileEvents
{
    extern const Event RWLockAcquiredReadLocks;
    extern const Event RWLockAcquiredWriteLocks;
    extern const Event RWLockReadersWaitMilliseconds;
    extern const Event RWLockWritersWaitMilliseconds;
}


namespace CurrentMetrics
{
    extern const Metric RWLockWaitingReaders;
    extern const Metric RWLockWaitingWriters;
    extern const Metric RWLockActiveReaders;
    extern const Metric RWLockActiveWriters;
}


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int DEADLOCK_AVOIDED;
}


class RWLockImpl::LockHolderImpl
{
    RWLock parent;
    GroupsContainer::iterator it_group;
    ClientsContainer::iterator it_client;
    ThreadToHolder::key_type thread_id;
    QueryIdToHolder::key_type query_id;
    CurrentMetrics::Increment active_client_increment;

    LockHolderImpl(RWLock && parent, GroupsContainer::iterator it_group, ClientsContainer::iterator it_client);

public:

    LockHolderImpl(const LockHolderImpl & other) = delete;

    ~LockHolderImpl();

    friend class RWLockImpl;
};


namespace
{
    /// Global information about all read locks that query has. It is needed to avoid some type of deadlocks.

    class QueryLockInfo
    {
    private:
        std::mutex mutex;
        std::map<std::string, size_t> queries;

    public:
        void add(const String & query_id)
        {
            std::lock_guard lock(mutex);
            ++queries[query_id];
        }

        void remove(const String & query_id)
        {
            std::lock_guard lock(mutex);
            auto it = queries.find(query_id);
            assert(it != queries.end());
            if (--it->second == 0)
                queries.erase(it);
        }

        void check(const String & query_id)
        {
            std::lock_guard lock(mutex);
            if (queries.count(query_id))
                throw Exception("Possible deadlock avoided. Client should retry.", ErrorCodes::DEADLOCK_AVOIDED);
        }
    };

    QueryLockInfo all_read_locks;
}


RWLockImpl::LockHolder RWLockImpl::getLock(RWLockImpl::Type type, const String & query_id)
{
    Stopwatch watch(CLOCK_MONOTONIC_COARSE);
    CurrentMetrics::Increment waiting_client_increment((type == Read) ? CurrentMetrics::RWLockWaitingReaders
                                                                      : CurrentMetrics::RWLockWaitingWriters);
    auto finalize_metrics = [type, &watch] ()
    {
        ProfileEvents::increment((type == Read) ? ProfileEvents::RWLockAcquiredReadLocks
                                                : ProfileEvents::RWLockAcquiredWriteLocks);
        ProfileEvents::increment((type == Read) ? ProfileEvents::RWLockReadersWaitMilliseconds
                                                : ProfileEvents::RWLockWritersWaitMilliseconds, watch.elapsedMilliseconds());
    };

    GroupsContainer::iterator it_group;
    ClientsContainer::iterator it_client;

    std::unique_lock lock(mutex);

    /// Check if the same query is acquiring previously acquired lock
    LockHolder existing_holder_ptr;

    auto this_thread_id = std::this_thread::get_id();
    auto it_thread = thread_to_holder.find(this_thread_id);

    auto it_query = query_id_to_holder.end();
    if (query_id != RWLockImpl::NO_QUERY)
        it_query = query_id_to_holder.find(query_id);

    if (it_thread != thread_to_holder.end())
        existing_holder_ptr = it_thread->second.lock();
    else if (it_query != query_id_to_holder.end())
        existing_holder_ptr = it_query->second.lock();

    if (existing_holder_ptr)
    {
        /// XXX: it means we can't upgrade lock from read to write - with proper waiting!
        if (type != Read || existing_holder_ptr->it_group->type != Read)
            throw Exception("Attempt to acquire exclusive lock recursively", ErrorCodes::LOGICAL_ERROR);

        return existing_holder_ptr;
    }

    /** If the query already has any active read lock and tries to acquire another read lock
      *  but it is not in front of the queue and has to wait, deadlock is possible:
      *
      * Example (four queries, two RWLocks - 'a' and 'b'):
      *
      *     --> time -->
      *
      * q1: ra          rb
      * q2:    wa
      * q3:       rb       ra
      * q4:          wb
      *
      * We will throw an exception instead.
      */

    if (type == Type::Write || queue.empty() || queue.back().type == Type::Write)
    {
        if (type == Type::Read && !queue.empty() && queue.back().type == Type::Write && query_id != RWLockImpl::NO_QUERY)
            all_read_locks.check(query_id);

        /// Create new group of clients
        it_group = queue.emplace(queue.end(), type);
    }
    else
    {
        /// Will append myself to last group
        it_group = std::prev(queue.end());

        if (it_group != queue.begin() && query_id != RWLockImpl::NO_QUERY)
            all_read_locks.check(query_id);
    }

    /// Append myself to the end of chosen group
    auto & clients = it_group->clients;
    try
    {
        it_client = clients.emplace(clients.end(), type);
    }
    catch (...)
    {
        /// Remove group if it was the first client in the group and an error occurred
        if (clients.empty())
            queue.erase(it_group);
        throw;
    }

    LockHolder res(new LockHolderImpl(shared_from_this(), it_group, it_client));

    /// Wait a notification until we will be the only in the group.
    it_group->cv.wait(lock, [&] () { return it_group == queue.begin(); });

    /// Insert myself (weak_ptr to the holder) to threads set to implement recursive lock
    thread_to_holder.emplace(this_thread_id, res);
    res->thread_id = this_thread_id;

    if (query_id != RWLockImpl::NO_QUERY)
    {
        query_id_to_holder.emplace(query_id, res);

        if (type == Type::Read)
            all_read_locks.add(query_id);
    }
    res->query_id = query_id;

    finalize_metrics();
    return res;
}


RWLockImpl::LockHolderImpl::~LockHolderImpl()
{
    std::lock_guard lock(parent->mutex);

    /// Remove weak_ptrs to the holder, since there are no owners of the current lock
    parent->thread_to_holder.erase(thread_id);
    parent->query_id_to_holder.erase(query_id);

    if (*it_client == RWLockImpl::Read && query_id != RWLockImpl::NO_QUERY)
        all_read_locks.remove(query_id);

    /// Removes myself from client list of our group
    it_group->clients.erase(it_client);

    /// Remove the group if we were the last client and notify the next group
    if (it_group->clients.empty())
    {
        auto & parent_queue = parent->queue;
        parent_queue.erase(it_group);

        if (!parent_queue.empty())
            parent_queue.front().cv.notify_all();
    }
}


RWLockImpl::LockHolderImpl::LockHolderImpl(RWLock && parent_, RWLockImpl::GroupsContainer::iterator it_group_,
                                             RWLockImpl::ClientsContainer::iterator it_client_)
    : parent{std::move(parent_)}, it_group{it_group_}, it_client{it_client_},
      active_client_increment{(*it_client == RWLockImpl::Read) ? CurrentMetrics::RWLockActiveReaders
                                                               : CurrentMetrics::RWLockActiveWriters}
{
}

}
