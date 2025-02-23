/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SqlOperations.h"
#include "SqlDelayThread.h"
#include "DatabaseEnv.h"
#include "DatabaseImpl.h"
#include "Timer.h"
#include "ThreadPool.h"

#define LOCK_DB_CONN(conn) SqlConnection::Lock guard(conn)

// ---- ASYNC STATEMENTS / TRANSACTIONS ----

bool SqlPlainRequest::Execute(SqlConnection* conn)
{
    // just do it
    LOCK_DB_CONN(conn);
    return conn->Execute(m_sql);
}

SqlTransaction::~SqlTransaction()
{
    while(!m_queue.empty())
    {
        delete m_queue.back();
        m_queue.pop_back();
    }
}

bool SqlTransaction::Execute(SqlConnection* conn)
{
    if(m_queue.empty())
        return true;

    LOCK_DB_CONN(conn);

    conn->BeginTransaction();

    int const nItems = m_queue.size();
    for (int i = 0; i < nItems; ++i)
    {
        SqlOperation* pStmt = m_queue[i];

        if(!pStmt->Execute(conn))
        {
            conn->RollbackTransaction();
            return false;
        }
    }

    return conn->CommitTransaction();
}

SqlPreparedRequest::SqlPreparedRequest(int nIndex, SqlStmtParameters* arg) : m_nIndex(nIndex), m_param(arg)
{
}

SqlPreparedRequest::~SqlPreparedRequest()
{
    delete m_param;
}

bool SqlPreparedRequest::Execute(SqlConnection* conn)
{
    LOCK_DB_CONN(conn);
    return conn->ExecuteStmt(m_nIndex, *m_param);
}

// ---- ASYNC QUERIES ----

bool SqlQuery::Execute(SqlConnection* conn)
{
    if(!m_callback || !m_queue)
        return false;

    LOCK_DB_CONN(conn);

    // execute the query and store the result in the callback
    std::unique_ptr<QueryResult> result = conn->Query(m_sql);
    m_callback->SetResult(std::move(result));

    // add the callback to the sql result queue of the thread it originated from
    m_queue->add(m_callback);

    return true;
}

void SqlResultQueue::Update(uint32 timeout)
{
    uint32 begin = WorldTimer::getMSTime();
    // execute the callbacks waiting in the synchronization queue
    MaNGOS::IQueryCallback* callback = NULL;
    int n = 0;
    while (next(callback))
    {
        if (!callback->IsThreadSafe())
        {
            _threadUnsafeWaitingQueries.add(callback);
            ++numUnsafeQueries;
        }
        else
        {
            ++n;
            //caller->queue.add(callback);
            m_callbackThreads << [callback, n](){
                callback->Execute();
                delete callback;
            };
        }
    }
    std::future<void> job = m_callbackThreads->processWorkload();
    MaNGOS::IQueryCallback* s = NULL;
    while (_threadUnsafeWaitingQueries.next(s))
    {
        s->Execute();
        delete s;
        --numUnsafeQueries;
        if (timeout && WorldTimer::getMSTimeDiffToNow(begin) > timeout)
            break;
    }

    if (numUnsafeQueries > 1000) // Bottleneck here
        sLog.Out(LOG_PERFORMANCE, LOG_LVL_MINIMAL, "Database: %u unsafe queries remaining!", numUnsafeQueries);

    if (job.valid())
        job.wait();
 }


#ifndef DO_POSTGRESQL
using SqlResultQueueWorker = ThreadPool::ThreadPool::MySQL<>;
#else
using SqlResultQueueWorker = ThreadPool::SingleQueue;
#endif

SqlResultQueue::SqlResultQueue() :
    numUnsafeQueries(0), m_callbackThreads(new ThreadPool(6))
{
    m_callbackThreads->start<SqlResultQueueWorker>();
}

SqlResultQueue::~SqlResultQueue(){}

void SqlResultQueue::CancelAll()
{
    MaNGOS::IQueryCallback* cb;
    while (next(cb))
    {
        cb->SetResult(nullptr);
        cb->Execute();
        delete cb;
    }
}

bool SqlQueryHolder::Execute(MaNGOS::IQueryCallback* callback, Database* database, SqlResultQueue* queue)
{
    if(!callback || !database || !queue)
        return false;

    // delay the execution of the queries, sync them with the delay thread
    // which will in turn resync on execution (via the queue) and call back
    SqlQueryHolderEx *holderEx = new SqlQueryHolderEx(this, callback, queue, serialId);

    database->AddToSerialDelayQueue(holderEx);
    return true;
}

bool SqlQueryHolder::SetQuery(size_t index, char const* sql)
{
    if(m_queries.size() <= index)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Query index (" SIZEFMTD ") out of range (size: " SIZEFMTD ") for query: %s", index, m_queries.size(), sql);
        return false;
    }

    if(m_queries[index].first != nullptr)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Attempt assign query to holder index (" SIZEFMTD ") where other query stored (Old: [%s] New: [%s])",
            index,m_queries[index].first,sql);
        return false;
    }

    // not executed yet, just stored (it's not called a holder for nothing)
    m_queries[index] = SqlResultPair(mangos_strdup(sql), (QueryResult*)nullptr);
    return true;
}

bool SqlQueryHolder::SetPQuery(size_t index, char const* format, ...)
{
    if(!format)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Query (index: " SIZEFMTD ") is empty.",index);
        return false;
    }

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if(res==-1)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL Query truncated (and not execute) for format: %s",format);
        return false;
    }

    return SetQuery(index,szQuery);
}

/// When you are using this function, you are the new owner of the ptr. The query will be removed from the QueryHolder
std::unique_ptr<QueryResult> SqlQueryHolder::TakeResult(size_t index)
{
    if(index < m_queries.size())
    {
        // the query strings are freed on the first TakeResult or in the destructor
        if(m_queries[index].first != nullptr)
        {
            delete [] (const_cast<char*>(m_queries[index].first));
            m_queries[index].first = nullptr;
        }
        return std::move(m_queries[index].second);
    }
    else
        return nullptr;
}

void SqlQueryHolder::SetResult(size_t index, std::unique_ptr<QueryResult> result)
{
    // store the result in the holder
    if(index < m_queries.size())
        m_queries[index].second = std::move(result);
}

SqlQueryHolder::~SqlQueryHolder() // TODO: Delete me when .first is also a smartpointer
{
    for(size_t i = 0; i < m_queries.size(); i++)
    {
        // if the result was never used, free the resources
        // results used already (getresult called) are expected to be deleted
        if(m_queries[i].first != nullptr)
        {
            delete [] (const_cast<char*>(m_queries[i].first));
        }
    }
}

void SqlQueryHolder::DeleteAllResults()
{
    for (size_t i = 0; i < m_queries.size(); i++)
    {
        // if the result was never used, free the resources
        // results used already (getresult called) are expected to be deleted
        m_queries[i].second.reset();
    }
}

void SqlQueryHolder::SetSize(size_t size)
{
    // to optimize push_back, reserve the number of queries about to be executed
    m_queries.resize(size);
}

bool SqlQueryHolderEx::Execute(SqlConnection* conn)
{
    if(!m_holder || !m_callback || !m_queue)
        return false;

    LOCK_DB_CONN(conn);
    // we can do this, we are friends
    std::vector<SqlQueryHolder::SqlResultPair> &queries = m_holder->m_queries;
    for(size_t i = 0; i < queries.size(); i++)
    {
        // execute all queries in the holder and pass the results
        char const *sql = queries[i].first;
        if (sql)
            m_holder->SetResult(i, conn->Query(sql));
    }

    // sync with the caller thread
    m_queue->add(m_callback);

    return true;
}
