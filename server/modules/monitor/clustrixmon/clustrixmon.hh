/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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

#define MXS_MODULE_NAME "clustrixmon"

#include <maxscale/ccdefs.hh>
#include <maxbase/log.hh>

#define    CLUSTER_MONITOR_INTERVAL_NAME            "cluster_monitor_interval"
const long DEFAULT_CLUSTER_MONITOR_INTERVAL_VALUE = 60000;
#define    DEFAULT_CLUSTER_MONITOR_INTERVAL_ZVALUE  "60000"

#define    HEALTH_CHECK_THRESHOLD_NAME              "health_check_threshold"
const long DEFAULT_HEALTH_CHECK_THRESHOLD_VALUE   = 2;
#define    DEFAULT_HEALTH_CHECK_THRESHOLD_ZVALUE    "2"

#define    DYNAMIC_NODE_DETECTION_NAME              "dynamic_node_detection"
const bool DEFAULT_DYNAMIC_NODE_DETECTION_VALUE   = true;
#define    DEFAULT_DYNAMIC_NODE_DETECTION_ZVALUE    "true"

#define    HEALTH_CHECK_PORT_NAME                   "health_check_port"
const long DEFAULT_HEALTH_CHECK_PORT_VALUE        = 3581;
#define    DEFAULT_HEALTH_CHECK_PORT_ZVALUE         "3581"
