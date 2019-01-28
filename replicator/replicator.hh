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

#pragma once

#include <string>
#include <memory>

#include "config.hh"

namespace cdc
{

// Final name pending
class Replicator
{
public:

    /**
     * Create a new data replicator
     *
     * @param cnf The configuration to use
     *
     * @return The new Replicator instance
     */
    static std::unique_ptr<Replicator> start(const Config& cnf);

    /**
     * Stops a running replication stream
     */
    void stop();

    /**
     * Get the current error message
     *
     * @return Current error message
     */
    std::string error() const;

    ~Replicator();

private:
    class Imp;
    Replicator(const Config& cnf);

    // Pointer to the implementation of the abstract interface
    std::unique_ptr<Replicator::Imp> m_imp;
};
}
