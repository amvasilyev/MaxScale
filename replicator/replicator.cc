/*
 * Copyright (c) 2019 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXB_MODULE_NAME "Replicator"
#include <maxbase/log.h>

// The public header
#include "replicator.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <fstream>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <mysql.h>
#include <mariadb_rpl.h>
#include <errmsg.h>

#include <maxscale/query_classifier.hh>
#include <maxscale/buffer.hh>
#include <maxscale/utils.hh>

// Private headers
#include "table.hh"
#include "executor.hh"
#include "sql.hh"

using std::chrono::duration_cast;
using Clock = std::chrono::steady_clock;
using Timepoint = Clock::time_point;
using std::chrono::milliseconds;

namespace std
{
template<>
struct default_delete<MARIADB_RPL_EVENT>
{
    void operator()(MARIADB_RPL_EVENT* event)
    {
        mariadb_free_rpl_event(event);
    }
};
}

using Event = std::unique_ptr<MARIADB_RPL_EVENT>;

namespace cdc
{


// A very small daemon. The main class that drives the whole conversion process
class Replicator::Imp
{
public:
    Imp& operator=(Imp&) = delete;
    Imp(Imp&) = delete;

    // Flag used in GTID events to signal statements that perform an implicit commit
    static constexpr int IMPLICIT_COMMIT_FLAG = 0x1;

    // Creates a new replication stream and starts it
    Imp(const Config& cnf);

    // Check if the replicator is still OK
    bool ok() const;

    ~Imp();

private:
    enum class State
    {
        BULK,   // Processing one or more bulk inserts
        STMT    // Processing SQL statements
    };

    enum class Skip
    {
        NONE,
        ALL,
        NEXT_TRX,
        NEXT_STMT
    };

    static const std::string STATEFILE_DIR;
    static const std::string STATEFILE_NAME;
    static const std::string STATEFILE_TMP_SUFFIX;

    bool connect();
    void process_events();
    bool process_one_event(Event& event);
    bool load_gtid_state();
    bool save_gtid_state() const;
    bool commit_transactions();
    bool should_process(Event& event);
    bool set_state(State state);
    bool find_binlog_start_gtid(std::string& output);

    Config               m_cnf;                 // The configuration the stream was started with
    std::unique_ptr<SQL> m_sql;                 // Database connection
    std::atomic<bool>    m_running {true};      // Whether the stream is running
    std::string          m_gtid;                // GTID position to start from
    std::string          m_current_gtid;        // GTID of the transaction being processed
    mutable std::mutex   m_lock;

    // Map of active tables
    std::unordered_map<uint64_t, std::unique_ptr<Table>> m_tables;

    // SQL executor that handles query events
    SQLExecutor m_executor;

    State       m_state {State::STMT};      // Current state
    Timepoint   m_last_commit;              // The last time all open transactions were committed
    bool        m_implicit_commit {false};  // Whether the current GTID is generated by an implicit commit
    Skip        m_skip {Skip::NONE};        // Skip binlog events until correct GTID
    std::thread m_thr;                      // Thread that receives the replication events
};

const std::string Replicator::Imp::STATEFILE_DIR = "./";
const std::string Replicator::Imp::STATEFILE_NAME = "current_gtid.txt";
const std::string Replicator::Imp::STATEFILE_TMP_SUFFIX = ".tmp";

Replicator::Imp::Imp(const Config& cnf)
    : m_cnf(cnf)
    , m_gtid(cnf.mariadb.gtid)
    , m_executor(cnf.cs.server)
    , m_thr(std::thread(&Imp::process_events, this))
{
}

bool Replicator::Imp::ok() const
{
    return m_running;
}

static bool gtid_list_is_newer(const std::string& gtid, const std::vector<std::string>& gtid_list)
{
    bool rval = false;
    auto lhs_parts = mxs::strtok(gtid, "-");

    for (const auto& a : gtid_list)
    {
        auto rhs_parts = mxs::strtok(a, "-");
        mxb_assert(lhs_parts.size() == rhs_parts.size());

        if (lhs_parts[0] == rhs_parts[0])
        {
            auto lhs = strtoull(lhs_parts[2].c_str(), nullptr, 10);
            auto rhs = strtoull(rhs_parts[2].c_str(), nullptr, 10);

            if (lhs < rhs)
            {
                rval = true;
                break;
            }
        }
    }

    return rval;
}

bool Replicator::Imp::find_binlog_start_gtid(std::string& output)
{
    bool rval = true;
    auto res = SQL::connect(m_cnf.mariadb.servers);

    if (!res.first.empty())
    {
        MXB_ERROR("%s", res.first.c_str());
    }
    else
    {
        auto& sql = res.second;

        if (sql->query("SHOW BINARY LOGS"))
        {
            auto binlogs = sql->fetch();

            for (const auto& b : binlogs)
            {
                /**
                 * Get the GTID coordinates for the start of the binlog. This tells us whether
                 * the file contains the GTID we're looking for.
                 */
                if (sql->query("SELECT BINLOG_GTID_POS('%s', 4)", b[0].c_str()))
                {
                    auto row = sql->fetch_row()[0];

                    // If there have been GTID events before this binlog, row will contain the GTID list
                    if (row.length() > 2)
                    {
                        row = row.substr(1, row.length() - 2);
                        auto gtids = mxs::strtok(row, ",");

                        if (gtid_list_is_newer(m_gtid, gtids))
                        {
                            // Found a binlog with newer GTIDs in it, start from the previous binlog
                            break;
                        }
                    }

                    output = row;
                }
                else
                {
                    MXB_ERROR("%s", sql->error().c_str());
                    rval = false;
                }
            }

            /**
             * @c output now points to the GTID at the start of the binlog file that contains our GTID.
             * We can now start replicating from it to retrieve the format description event and skip
             * events until we reach the GTID we're looking for.
             */
        }
        else
        {
            MXB_ERROR("%s", sql->error().c_str());
            rval = false;
        }
    }

    return rval;
}

bool Replicator::Imp::connect()
{
    if (m_sql)
    {
        // We already have a connection
        return true;
    }

    bool rval = false;
    std::string start_gtid;

    if (!m_gtid.empty())
    {
        if (!find_binlog_start_gtid(start_gtid))
        {
            return false;
        }

        m_skip = Skip::ALL;
        MXB_INFO("Starting from GTID '%s' and skipping events until GTID '%s'",
                 start_gtid.c_str(), m_gtid.c_str());
    }

    std::string gtid_start_pos = "SET @slave_connect_state='" + start_gtid + "'";
    std::string err;

    std::tie(err, m_sql) = SQL::connect(m_cnf.mariadb.servers);

    if (!err.empty())
    {
        MXB_ERROR("%s", err.c_str());
    }
    else
    {
        mxb_assert(m_sql);
        // Queries required to start GTID replication
        std::vector<std::string> queries =
        {
            "SET @master_binlog_checksum = @@global.binlog_checksum",
            "SET @mariadb_slave_capability=4",
            gtid_start_pos,
            "SET @slave_gtid_strict_mode=1",
            "SET @slave_gtid_ignore_duplicates=1",
            "SET NAMES latin1"
        };

        if (!m_sql->query(queries))
        {
            MXB_ERROR("Failed to prepare connection: %s", m_sql->error().c_str());
        }
        else if (!m_sql->replicate(m_cnf.mariadb.server_id))
        {
            MXB_ERROR("Failed to open replication channel: %s", m_sql->error().c_str());
        }
        else
        {
            MXB_NOTICE("Started replicating from [%s]:%d at GTID '%s'", m_sql->server().host.c_str(),
                       m_sql->server().port, m_gtid.c_str());
            rval = true;
        }
    }

    if (!rval)
    {
        m_sql.reset();
    }

    return rval;
}

void Replicator::Imp::process_events()
{
    if (!load_gtid_state())
    {
        m_running = false;
    }

    while (m_running)
    {
        if (!connect())
        {
            // We failed to connect to any of the servers, try again in a few seconds
            std::this_thread::sleep_for(milliseconds(5000));
            continue;
        }

        Event event(m_sql->fetch_event());

        if (event)
        {
            if (should_process(event))
            {
                if (!process_one_event(event))
                {
                    /**
                     * Fatal error encountered. Fixing it might require manual intervention so
                     * the safest thing to do is to stop processing data.
                     */
                    m_running = false;
                }
            }
        }
        else if (m_sql->errnum() == CR_SERVER_LOST)
        {
            // Network error, close the connection and connect again at the start of the next loop
            m_sql.reset();
        }
        else
        {
            MXB_ERROR("Failed to read replicated event: %s", m_sql->error().c_str());
            break;
        }
    }

    m_executor.rollback();

    for (const auto& a : m_tables)
    {
        a.second->rollback();
    }
}

std::string to_gtid_string(const MARIADB_RPL_EVENT& event)
{
    std::stringstream ss;
    ss << event.event.gtid.domain_id << '-' << event.server_id << '-' << event.event.gtid.sequence_nr;
    return ss.str();
}

bool Replicator::Imp::load_gtid_state()
{
    bool rval = false;
    std::string filename = STATEFILE_DIR + STATEFILE_NAME;
    std::ifstream statefile(filename);
    std::string gtid;
    statefile >> gtid;

    if (statefile)
    {
        rval = true;

        if (!gtid.empty())
        {
            m_gtid = gtid;
            MXB_NOTICE("Continuing from GTID '%s'", m_gtid.c_str());
        }
    }
    else
    {
        if (errno == ENOENT)
        {
            //  No GTID file, use the GTID provided in the configuration
            rval = true;
        }
        else
        {
            MXB_ERROR("Failed to load current GTID state from file '%s': %d, %s",
                      filename.c_str(), errno, mxb_strerror(errno));
        }
    }

    return rval;
}

bool Replicator::Imp::save_gtid_state() const
{
    bool rval = false;
    std::string filename = STATEFILE_DIR + STATEFILE_NAME;
    std::string tmpname = filename + STATEFILE_TMP_SUFFIX;
    std::ofstream statefile(tmpname);
    statefile << m_current_gtid << std::endl;

    if (statefile)
    {
        rval = rename(tmpname.c_str(), filename.c_str()) == 0;
    }

    if (!rval)
    {
        MXB_ERROR("Failed to store current GTID state into file '%s': %d, %s",
                  filename.c_str(), errno, mxb_strerror(errno));
    }

    return rval;
}

bool Replicator::Imp::commit_transactions()
{
    bool rval = m_executor.commit();

    for (auto& t : m_tables)
    {
        if (!t.second->commit())
        {
            rval = false;
        }
    }

    if (rval)
    {
        rval = save_gtid_state();
    }
    else
    {
        MXB_ERROR("One or more transactions failed to commit at GTID '%s'", m_current_gtid.c_str());
    }

    return rval;
}


bool Replicator::Imp::set_state(State state)
{
    bool rval = false;

    if (m_state != state)
    {
        if (commit_transactions())
        {
            m_state = state;
            rval = true;
        }
    }
    else
    {
        rval = true;
    }

    return rval;
}

bool Replicator::Imp::should_process(Event& event)
{
    bool rval = true;

    if (m_skip != Skip::NONE)
    {
        rval = false;

        if (event->event_type == GTID_EVENT)
        {
            mxb_assert(m_skip == Skip::ALL);
            auto gtid = to_gtid_string(*event);


            if (gtid == m_gtid)
            {
                m_skip = event->event.gtid.flags & IMPLICIT_COMMIT_FLAG ? Skip::NEXT_STMT : Skip::NEXT_TRX;
                MXB_INFO("Reached GTID '%s', skipping next transaction", m_gtid.c_str());
            }
            else if (gtid_list_is_newer(m_gtid, {gtid}))
            {
                MXB_ERROR("GTID '%s' is newer than '%s', cannot continue conversion process.",
                          gtid.c_str(), m_gtid.c_str());
                m_running = false;
            }
        }
        else if (m_skip == Skip::NEXT_STMT || (m_skip == Skip::NEXT_TRX && event->event_type == XID_EVENT))
        {
            m_skip = Skip::NONE;
            MXB_INFO("Transaction for GTID '%s' skipped, ready to process events", m_gtid.c_str());
        }
    }
    else if (!m_cnf.mariadb.tables.empty())
    {
        if (event->event_type == TABLE_MAP_EVENT)
        {
            auto tbl = to_string(event->event.table_map.table);
            auto db = to_string(event->event.table_map.database);
            rval = m_cnf.mariadb.tables.count(db + '.' + tbl);
        }
        else if (event->event_type == QUERY_EVENT)
        {
            // For query events, all participating tables must be in the list of accepted tables
            auto db = to_string(event->event.query.database);
            mxs::Buffer buffer(event->event.query.statement.str, event->event.query.statement.length);
            int sz = 0;
            char** tables = qc_get_table_names(buffer.get(), &sz, true);

            for (int i = 0; i < sz; i++)
            {
                std::string tbl = tables[i];

                /**
                 * This is not very reliable (the table name can have a dot in it) and the query classifier
                 * would need to tell us the database and table names separately.
                 */
                if (tbl.find('.') == std::string::npos)
                {
                    tbl = db + '.' + tbl;
                }

                if (m_cnf.mariadb.tables.count(tbl) == 0)
                {
                    rval = false;
                }
            }

            qc_free_table_names(tables, sz);
        }
    }

    return rval;
}

bool Replicator::Imp::process_one_event(Event& event)
{
    bool rval = true;

    switch (event->event_type)
    {
    case GTID_EVENT:
        if (event->event.gtid.flags & IMPLICIT_COMMIT_FLAG)
        {
            m_implicit_commit = true;
        }

        m_current_gtid = to_gtid_string(*event);
        MXB_INFO("GTID: %s", m_current_gtid.c_str());
        break;

    case XID_EVENT:
        if ((rval = commit_transactions()))
        {
            m_gtid = m_current_gtid;
            m_last_commit = Clock::now();
            MXB_INFO("XID for GTID '%s': %lu", m_current_gtid.c_str(), event->event.xid.transaction_nr);
        }
        break;

    case TABLE_MAP_EVENT:
        // The CS API likes to throw exceptions
        try
        {
            m_tables[event->event.table_map.table_id] = Table::open(m_cnf, event.get());
        }
        catch (mcsapi::ColumnStoreError& err)
        {
            MXB_ERROR("Could not open table: %s", err.what());
            rval = false;
        }
        break;

    case QUERY_EVENT:
        if ((rval = set_state(State::STMT)))
        {
            m_executor.enqueue(event.release());

            if (m_implicit_commit)
            {
                m_implicit_commit = false;
                m_gtid = m_current_gtid;
                rval = commit_transactions();
            }
        }
        break;

    case WRITE_ROWS_EVENT_V1:
    case UPDATE_ROWS_EVENT_V1:
    case DELETE_ROWS_EVENT_V1:
        {
            const auto& t = m_tables[event->event.rows.table_id];

            if (t && (rval = set_state(State::BULK)))
            {
                MXB_INFO("ROWS event for `%s`.`%s`", t->db(), t->table());
                t->enqueue(event.release());
            }
        }
        break;

    default:
        // Ignore the event
        break;
    }

    return rval;
}

Replicator::Imp::~Imp()
{
    m_running = false;
    m_thr.join();
}

//
// The public API
//

// static
std::unique_ptr<Replicator> Replicator::start(const Config& cnf)
{
    return std::unique_ptr<Replicator>(new Replicator(cnf));
}

bool Replicator::ok() const
{
    return m_imp->ok();
}

Replicator::~Replicator()
{
}

Replicator::Replicator(const Config& cnf)
    : m_imp(new Imp(cnf))
{
}
}
