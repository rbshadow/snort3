/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

// ip_module.cc author Russ Combs <rucombs@cisco.com>

#include "ip_module.h"

#include <string>
using namespace std;

#include "stream_ip.h"
#include "main/snort_config.h"
#include "stream/stream.h"

#define DEFRAG_IPOPTIONS_STR \
    "(defrag) Inconsistent IP Options on Fragmented Packets"

#define DEFRAG_TEARDROP_STR \
    "(defrag) Teardrop attack"

#define DEFRAG_SHORT_FRAG_STR \
    "(defrag) Short fragment, possible DoS attempt"

#define DEFRAG_ANOMALY_OVERSIZE_STR \
    "(defrag) Fragment packet ends after defragmented packet"

#define DEFRAG_ANOMALY_ZERO_STR \
    "(defrag) Zero-byte fragment packet"

#define DEFRAG_ANOMALY_BADSIZE_SM_STR \
    "(defrag) Bad fragment size, packet size is negative"

#define DEFRAG_ANOMALY_BADSIZE_LG_STR \
    "(defrag) Bad fragment size, packet size is greater than 65536"

#define DEFRAG_ANOMALY_OVLP_STR \
    "(defrag) Fragmentation overlap"

#if 0  // OBE
#define DEFRAG_IPV6_BSD_ICMP_FRAG_STR
    "(defrag) IPv6 BSD mbufs remote kernel buffer overflow"

#define DEFRAG_IPV6_BAD_FRAG_PKT_STR
    "(defrag) Bogus fragmentation packet. Possible BSD attack"
#endif

#define DEFRAG_MIN_TTL_EVASION_STR \
    "(defrag) TTL value less than configured minimum, not using for reassembly"

#define DEFRAG_EXCESSIVE_OVERLAP_STR \
    "(defrag) Excessive fragment overlap"

#define DEFRAG_TINY_FRAGMENT_STR \
    "(defrag) Tiny fragment"

FragEngine::FragEngine()
{ memset(this, 0, sizeof(*this)); }

//-------------------------------------------------------------------------
// stream_ip module
//-------------------------------------------------------------------------

static const char* policies = 
    "first | linux | bsd | bsd_right |last | windows | solaris";

static const RuleMap stream_ip_rules[] =
{
    { DEFRAG_IPOPTIONS, DEFRAG_IPOPTIONS_STR },
    { DEFRAG_TEARDROP, DEFRAG_TEARDROP_STR },
    { DEFRAG_SHORT_FRAG, DEFRAG_SHORT_FRAG_STR },
    { DEFRAG_ANOMALY_OVERSIZE, DEFRAG_ANOMALY_OVERSIZE_STR },
    { DEFRAG_ANOMALY_ZERO, DEFRAG_ANOMALY_ZERO_STR },
    { DEFRAG_ANOMALY_BADSIZE_SM, DEFRAG_ANOMALY_BADSIZE_SM_STR },
    { DEFRAG_ANOMALY_BADSIZE_LG, DEFRAG_ANOMALY_BADSIZE_LG_STR },
    { DEFRAG_ANOMALY_OVLP, DEFRAG_ANOMALY_OVLP_STR },
    { DEFRAG_MIN_TTL_EVASION, DEFRAG_MIN_TTL_EVASION_STR },
    { DEFRAG_EXCESSIVE_OVERLAP, DEFRAG_EXCESSIVE_OVERLAP_STR },
    { DEFRAG_TINY_FRAGMENT, DEFRAG_TINY_FRAGMENT_STR },

    { 0, nullptr }
};

static const Parameter stream_ip_params[] =
{
    { "max_frags", Parameter::PT_INT, "1:", "8192",
      "maximum number of simultaneous fragments being tracked" },

    { "max_overlaps", Parameter::PT_INT, "0:", "0",
      "maximum allowed overlaps per datagram; 0 is unlimited" },

    { "min_frag_length", Parameter::PT_INT, "0:", "0",
      "alert if fragment length is below this limit before or after trimming" },

    { "min_ttl", Parameter::PT_INT, "1:255", "1",
      "discard fragments with ttl below the minimum" },

    { "policy", Parameter::PT_ENUM, policies, "linux",
      "fragment reassembly policy" },

    { "session_timeout", Parameter::PT_INT, "1:86400", "30",
      "session tracking timeout" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

StreamIpModule::StreamIpModule() :
    Module(MOD_NAME, stream_ip_params, stream_ip_rules)
{
    config = nullptr;
}

StreamIpModule::~StreamIpModule()
{
    if ( config )
        delete config;
}

StreamIpConfig* StreamIpModule::get_data()
{
    StreamIpConfig* temp = config;
    config = nullptr;
    return temp;
}

bool StreamIpModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("max_frags") )
        config->frag_engine.max_frags = v.get_long();

    else if ( v.is("max_overlaps") )
        config->frag_engine.max_overlaps = v.get_long();

    else if ( v.is("min_frag_length") )
        config->frag_engine.min_fragment_length = v.get_long();

    else if ( v.is("min_ttl") )
        config->frag_engine.min_ttl = v.get_long();

    else if ( v.is("policy") )
        config->frag_engine.frag_policy = v.get_long() + 1;

    else if ( v.is("session_timeout") )
    {
        // FIXIT need to integrate to eliminate redundant data
        config->session_timeout = v.get_long();
        config->frag_engine.frag_timeout = v.get_long();
    }

    else
        return false;

    return true;
}

bool StreamIpModule::begin(const char*, int, SnortConfig*)
{
    if ( !config )
        config = new StreamIpConfig;

    return true;
}

bool StreamIpModule::end(const char*, int, SnortConfig*)
{
    return true;
}
