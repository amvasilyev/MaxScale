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

namespace cdc
{

namespace real
{
// Private namespace for implementation details
class Replicator;
}

// Final name pending
class Replicator
{
public:

    /**
     * Create a new data replicator
     *
     * @param cnf Path to the INI format configuration file
     *
     * @return On success, an empty string is returned and the new Replicator instance is stored in
     *         the unique_ptr. If an error occurred, the string contains the error description.
     */
    static std::pair<std::string, std::unique_ptr<Replicator>> start(const std::string& cnf);

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
    Replicator(std::unique_ptr<real::Replicator> real);

    // Pointer to the implementation of the abstract interface
    std::unique_ptr<real::Replicator> m_real;
};
}
