/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2013-2013 Sourcefire, Inc.
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

#include "snort.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string>
using namespace std;

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/select.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <time.h>
#include <syslog.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#if !defined(CATCH_SEGV)
# include <sys/resource.h>
#endif

#include <thread>

#include "helpers/process.h"
#include "decode.h"
#include "encode.h"
#include "packet_io/sfdaq.h"
#include "packet_io/active.h"
#include "rules.h"
#include "treenodes.h"
#include "snort_debug.h"
#include "main/snort_config.h"
#include "util.h"
#include "parser.h"
#include "packet_io/trough.h"
#include "tag.h"
#include "detect.h"
#include "mstring.h"
#include "fpcreate.h"
#include "fpdetect.h"
#include "filters/sfthreshold.h"
#include "filters/rate_filter.h"
#include "packet_time.h"
#include "perf_monitor/perf_base.h"
#include "perf_monitor/perf.h"
#include "mempool/mempool.h"
#include "sflsq.h"
#include "ips_options/ips_flowbits.h"
#include "event_queue.h"
#include "asn1.h"
#include "framework/mpse.h"
#include "managers/shell.h"
#include "managers/module_manager.h"
#include "managers/plugin_manager.h"
#include "managers/script_manager.h"
#include "managers/event_manager.h"
#include "managers/inspector_manager.h"
#include "managers/ips_manager.h"
#include "managers/mpse_manager.h"
#include "managers/packet_manager.h"
#include "detection/sfrim.h"
#include "ppm.h"
#include "profiler.h"
#include "utils/strvec.h"
#include "packet_io/intf.h"
#include "detection_util.h"
#include "control/idle_processing.h"
#include "file_api/file_service.h"
#include "flow/flow_control.h"
#include "main/analyzer.h"
#include "log/sf_textlog.h"
#include "log/log_text.h"
#include "time/periodic.h"
#include "parser/config_file.h"
#include "parser/cmd_line.h"
#include "target_based/sftarget_reader.h"
#include "stream5/stream_api.h"
#include "stream5/stream_common.h"
#include "stream5/stream_ha.h"

#ifdef SIDE_CHANNEL
#include "side_channel/sidechannel.h"
#endif

#ifdef INTEL_SOFT_CPM
#include "search/intel_soft_cpm.h"
#endif

//-------------------------------------------------------------------------

THREAD_LOCAL SnortConfig* snort_conf = NULL;
static SnortConfig* snort_cmd_line_conf = NULL;

static bool snort_initializing = true;
static int snort_exiting = 0;

static pid_t snort_main_thread_pid = 0;
static pthread_t snort_main_thread_id = 0;

static int snort_argc = 0;
static char** snort_argv = NULL;

static void CleanExit(int);
static void SnortCleanup();

//-------------------------------------------------------------------------
// nascent policy management
//-------------------------------------------------------------------------
// FIXIT need stub binding rule to set these for runtime
// FIXIT need to set these on load too somehow

static THREAD_LOCAL NetworkPolicy* s_traffic_policy = nullptr;
static THREAD_LOCAL InspectionPolicy* s_inspection_policy = nullptr;
static THREAD_LOCAL IpsPolicy* s_detection_policy = nullptr;

NetworkPolicy* get_network_policy()
{ return s_traffic_policy; }

InspectionPolicy* get_inspection_policy()
{ return s_inspection_policy; }

IpsPolicy* get_ips_policy()
{ return s_detection_policy; }

void set_network_policy(NetworkPolicy* p)
{ s_traffic_policy = p; }

void set_inspection_policy(InspectionPolicy* p)
{ s_inspection_policy = p; }

void set_ips_policy(IpsPolicy* p)
{ s_detection_policy = p; }

//-------------------------------------------------------------------------
// utility
//-------------------------------------------------------------------------

#if 0
#ifdef HAVE_DAQ_ACQUIRE_WITH_META
static int MetaCallback(
    void* user, const DAQ_MetaHdr_t *metahdr, const uint8_t* data)
{
    PolicyId policy_id = getDefaultPolicy();
    SnortPolicy *policy;

    PROFILE_VARS;
    PREPROC_PROFILE_START(metaPerfStats);

    policy = snort_conf->targeted_policies[policy_id];
    InspectorManager::dispatch_meta(policy->framework_policy, metahdr->type, data);

    PREPROC_PROFILE_END(metaPerfStats);

    return 0;
}
#endif

static void SetupMetadataCallback(void)  // FIXDAQ
{
#ifdef HAVE_DAQ_ACQUIRE_WITH_META
    DAQ_Set_MetaCallback(&MetaCallback);
#endif
}
#endif

#if 0
// FIXIT not yet used
static void restart()
{
    int daemon_mode = ScDaemonMode();

    if ((!ScReadMode() && (getuid() != 0)) ||
        (snort_conf->chroot_dir != NULL))
    {
        LogMessage("Reload via Signal Reload does not work if you aren't root "
                   "or are chroot'ed.\n");
        /* We are restarting because of a configuration verification problem */
        CleanExit(1);
    }

    LogMessage("\n");
    LogMessage("** Restarting Snort **\n");
    LogMessage("\n");
    SnortCleanup();

    if (daemon_mode)
        set_daemon_args(snort_argc, snort_argv);

#ifdef PARANOID
    execv(snort_argv[0], snort_argv);
#else
    execvp(snort_argv[0], snort_argv);
#endif

    /* only get here if we failed to restart */
    LogMessage("Restarting %s failed: %s\n", snort_argv[0], get_error(errno));

    closelog();

    exit(-1);
}
#endif

//-------------------------------------------------------------------------
// perf stats
// FIXIT - this stuff should be in inits where the data lives
//-------------------------------------------------------------------------

static PreprocStats* get_profile(const char* key)
{
    if ( !strcmp(key, "detect") )
        return &detectPerfStats;

    if ( !strcmp(key, "mpse") )
        return &mpsePerfStats;

    if ( !strcmp(key, "rule eval") )
        return &rulePerfStats;

    if ( !strcmp(key, "rtn eval") )
        return &ruleRTNEvalPerfStats;

    if ( !strcmp(key, "rule tree eval") )
        return &ruleOTNEvalPerfStats;

    if ( !strcmp(key, "decode") )
        return &decodePerfStats;

    if ( !strcmp(key, "eventq") )
        return &eventqPerfStats;

    if ( !strcmp(key, "total") )
        return &totalPerfStats;

    if ( !strcmp(key, "daq meta") )
        return &metaPerfStats;

    return nullptr;
}

static void register_profiles()
{
    RegisterPreprocessorProfile(
        "detect", &detectPerfStats, 0, &totalPerfStats, get_profile);
    RegisterPreprocessorProfile(
        "mpse", &mpsePerfStats, 1, &detectPerfStats, get_profile);
    RegisterPreprocessorProfile(
        "rule eval", &rulePerfStats, 1, &detectPerfStats, get_profile);
    RegisterPreprocessorProfile(
        "rtn eval", &ruleRTNEvalPerfStats, 2, &rulePerfStats, get_profile);
    RegisterPreprocessorProfile(
        "rule tree eval", &ruleOTNEvalPerfStats, 2, &rulePerfStats, get_profile);
    RegisterPreprocessorProfile(
        "decode", &decodePerfStats, 0, &totalPerfStats, get_profile);
    RegisterPreprocessorProfile(
        "eventq", &eventqPerfStats, 0, &totalPerfStats, get_profile);
    RegisterPreprocessorProfile(
        "total", &totalPerfStats, 0, NULL, get_profile);
    RegisterPreprocessorProfile(
        "daq meta", &metaPerfStats, 0, NULL, get_profile);
}

//-------------------------------------------------------------------------
// initialization
//-------------------------------------------------------------------------

static void SnortInit(int argc, char **argv)
{
    init_signals();

#if defined(NOCOREFILE)
    SetNoCores();
#else
    StoreSnortInfoStrings();
#endif

    InitProtoNames();
#ifdef SIDE_CHANNEL
    pthread_mutex_init(&snort_process_lock, NULL);
#endif

    if (snort_cmd_line_conf != NULL)  // FIXIT can this be deleted?
    {
        FatalError("%s(%d) Trying to parse the command line again.\n",
                   __FILE__, __LINE__);
    }

    /* chew up the command line */
    snort_cmd_line_conf = ParseCmdLine(argc, argv);
    snort_conf = snort_cmd_line_conf;

    /* Tell 'em who wrote it, and what "it" is */
    if (!ScLogQuiet())
        PrintVersion();

    LogMessage("--------------------------------------------------\n");

    // FIXIT config plugin_path won't work like this
    Shell::init();
    ModuleManager::init();
    ScriptManager::load_scripts(snort_cmd_line_conf->script_path);
    PluginManager::load_plugins(snort_cmd_line_conf->plugin_path);

    ModuleManager::dump_modules();
    PluginManager::dump_plugins();
    FileAPIInit();

    SnortConfig *sc;

#ifdef PERF_PROFILING
    register_profiles();
#endif

    sc = ParseSnortConf(snort_cmd_line_conf->var_list);

    /* Merge the command line and config file confs to take care of
     * command line overriding config file.
     * Set the global snort_conf that will be used during run time */
    snort_conf = MergeSnortConfs(snort_cmd_line_conf, sc);

    if ( snort_conf->output )
        EventManager::instantiate(snort_conf->output, sc);

    {
        // FIXIT AttributeTable::end() is where this should go
        if ( snort_conf->attribute_file )
            SFAT_ParseAttributeTable(snort_conf->attribute_file);
    }

    if (snort_conf->asn1_mem != 0)
        asn1_init_mem(snort_conf->asn1_mem);
    else
        asn1_init_mem(256);

    if (snort_conf->alert_file != NULL)
    {
        char *tmp = snort_conf->alert_file;
        snort_conf->alert_file = ProcessFileOption(snort_conf, snort_conf->alert_file);
        free(tmp);
    }

#ifdef PERF_PROFILING
    /* Parse profiling here because of file option and potential
     * dependence on log directory */
    ConfigProfiling(snort_conf);
#endif

    if (ScAlertBeforePass())
    {
        OrderRuleLists(snort_conf, "activation dynamic drop sdrop reject alert pass log");
    }
    if ( !InspectorManager::configure(snort_conf) )
        SnortFatalExit();

    InspectorManager::print_config(snort_conf); // FIXIT make optional

    ParseRules(snort_conf);

    // FIXIT print should be through generic module list 
    // and only print configured / active stuff
    //detection_filter_print_config(snort_conf->detection_filter_config);
    //RateFilter_PrintConfig(snort_conf->rate_filter_config);
    //print_thresholding(snort_conf->threshold_config, 0);
    //PrintRuleOrder(snort_conf->rule_lists);

    /* Check rule state lists, enable/disabled
     * and err on 'special' GID without OTN.
     */
    SetRuleStates(snort_conf);

    SetPortFilterLists(snort_conf);  // FIXIT need to do these on reload?
    InitServiceFilterStatus(snort_conf);

    /* Need to do this after dynamic detection stuff is initialized, too */
    IpsManager::verify();

    if (snort_conf->file_mask != 0)
        umask(snort_conf->file_mask);
    else
        umask(077);    /* set default to be sane */

    IpsManager::global_init(snort_conf);

    fpCreateFastPacketDetection(snort_conf);
    MpseManager::activate_search_engine(snort_conf);

#ifdef PPM_MGR
    PPM_PRINT_CFG(&snort_conf->ppm_cfg);
#endif

    /* Finish up the pcap list and put in the queues */
    Trough_SetUp();

    // FIXIT stuff like this that is also done in snort_config.cc::VerifyReload()
    // should be refactored
    if ((snort_conf->bpf_filter == NULL) && (snort_conf->bpf_file != NULL))
        snort_conf->bpf_filter = read_infile(snort_conf->bpf_file);

    if (snort_conf->bpf_filter != NULL)
        LogMessage("Snort BPF option: %s\n", snort_conf->bpf_filter);

    if (ScOutputUseUtc())
        snort_conf->thiszone = 0;
#ifndef VALGRIND_TESTING
    else
        snort_conf->thiszone = gmt2local(0);
#endif

    EventManager::configure_outputs(snort_conf);

#ifdef SIDE_CHANNEL
    RegisterSideChannelModules();
    ConfigureSideChannelModules(snort_conf);
    SideChannelConfigure(snort_conf);
    SideChannelInit();
    SideChannelStartTXThread();
#endif
}

// this function should only include initialization that must be done as a
// non-root user such as creating log files.  other initialization stuff should
// be in the main initialization function since, depending on platform and
// configuration, this may be running in a background thread while passing
// packets in a fail open mode in the main thread.  we don't want big delays
// here to cause excess latency or dropped packets in that thread which may
// be the case if all threads are pinned to a single cpu/core.
//
// clarification: once snort opens/starts the DAQ, packets are queued for snort
// and must be disposed of quickly or the queue will overflow and packets will
// be dropped so the fail open thread does the remaining initialization while
// the main thread passes packets.  prior to opening and starting the DAQ,
// packet passing is done by the driver/hardware.  the goal then is to put as
// much initialization stuff in SnortInit() as possible and to restrict this
// function to those things that depend on DAQ startup or non-root user/group.
//
// FIXIT breaks DAQ_New()/Start() because packet threads won't be root when
// opening iface
static void SnortUnprivilegedInit(void)
{
    /* create the PID file */
    if ( !ScReadMode() &&
        (ScDaemonMode() || *snort_conf->pidfile_suffix || ScCreatePidFile()))
    {
        CreatePidFile(snort_main_thread_pid);
    }

    /* Drop the Chrooted Settings */
    if (snort_conf->chroot_dir)
        SetChroot(snort_conf->chroot_dir, &snort_conf->log_dir);

    /* Drop privileges if requested, when initialization is done */
    SetUidGid(ScUid(), ScGid());

#ifdef SIDE_CHANNEL
    SideChannelPostInit();
#endif

    snort_initializing = false;
}

void snort_setup(int argc, char *argv[])
{
    snort_argc = argc;
    snort_argv = argv;

    // must be done now in case of fatal error
    // and again after daemonization
    snort_main_thread_id = pthread_self();
    OpenLogger();

    SnortInit(argc, argv);

    LogMessage("%s\n", LOG_DIV);
    DAQ_Init(snort_conf);

    if ( ScDaemonMode() )
        daemonize();

    // this must follow daemonization
    snort_main_thread_pid = gettid();
    snort_main_thread_id = pthread_self();

    /* Change groups */
    InitGroups(ScUid(), ScGid());
    SnortUnprivilegedInit();

    set_quick_exit(false);
}

//-------------------------------------------------------------------------
// termination
//-------------------------------------------------------------------------

static void CleanExit(int exit_val)
{
    SnortConfig tmp;

#ifdef DEBUG
#if 0
    SFLAT_dump();
#endif
#endif

    /* Have to trick LogMessage to log correctly after snort_conf
     * is freed */
    memset(&tmp, 0, sizeof(tmp));

    if (snort_conf != NULL)
    {
        tmp.logging_flags |=
            (snort_conf->logging_flags & LOGGING_FLAG__QUIET);

        tmp.run_flags |= (snort_conf->run_flags & RUN_FLAG__DAEMON);

        tmp.logging_flags |=
            (snort_conf->logging_flags & LOGGING_FLAG__SYSLOG);
    }

    SnortCleanup();
    snort_conf = &tmp;

    LogMessage("Snort exiting\n");
    closelog();
    //if ( !done_processing )  // FIXIT
        exit(exit_val);
}

static void SnortCleanup()
{
    /* This function can be called more than once.  For example,
     * once from the SIGINT signal handler, and once recursively
     * as a result of calling pcap_close() below.  We only need
     * to perform the cleanup once, however.  So the static
     * variable already_exiting will act as a flag to prevent
     * double-freeing any memory.  Not guaranteed to be
     * thread-safe, but it will prevent the simple cases.
     */
    static int already_exiting = 0;
    if( already_exiting != 0 )
    {
        return;
    }
    already_exiting = 1;
    snort_exiting = 1;
    snort_initializing = false;  /* just in case we cut out early */

#ifdef SIDE_CHANNEL
    SideChannelStopTXThread();
    SideChannelCleanUp();
#endif
    IdleProcessingCleanUp();

    IpsManager::global_term(snort_conf);
    SFAT_Cleanup();
    Trough_CleanUp();
    ClosePidFile();

    /* remove pid file */
    if (SnortStrnlen(snort_conf->pid_filename, sizeof(snort_conf->pid_filename)) > 0)
    {
        int ret;

        ret = unlink(snort_conf->pid_filename);

        if (ret != 0)
        {
            ErrorMessage("Could not remove pid file %s: %s\n",
                         snort_conf->pid_filename, get_error(errno));
        }
    }

    //MpseManager::print_search_engine_stats();

    /* free allocated memory */
    if (snort_conf == snort_cmd_line_conf)
    {
        SnortConfFree(snort_cmd_line_conf);
        snort_cmd_line_conf = NULL;
        snort_conf = NULL;
    }
    else
    {
        SnortConfFree(snort_cmd_line_conf);
        snort_cmd_line_conf = NULL;
        SnortConfFree(snort_conf);
        snort_conf = NULL;
    }

    close_fileAPI();

    sfthreshold_free();  // FIXDAQ etc.
    RateFilter_Cleanup();
    asn1_free_mem();

    periodic_release();
    ParserCleanup();

#ifdef PERF_PROFILING
    CleanupPreprocStatsNodeList();
#endif

    CleanupProtoNames();
    cmd_line_term();
    ModuleManager::term();
    PluginManager::release_plugins();
    Shell::term();
}

void snort_cleanup()
{
    DAQ_Term();

    if ( !ScTestMode() )  // FIXIT ideally the check is in one place
        PrintStatistics();

    CloseLogger();
    CleanExit(0);
}

//-------------------------------------------------------------------------
// reload foo
//-------------------------------------------------------------------------

// FIXIT refactor this so startup and reload call the same core function to
// instantiate things that can be reloaded
static SnortConfig * get_reload_config(void)
{
    SnortConfig *sc = ParseSnortConf(snort_cmd_line_conf->var_list);

    sc = MergeSnortConfs(snort_cmd_line_conf, sc);

#ifdef PERF_PROFILING
    /* Parse profiling here because of file option and potential
     * dependence on log directory */
    ConfigProfiling(sc);
#endif

    if (VerifyReload(sc) == -1)
    {
        SnortConfFree(sc);
        return NULL;
    }

    if (sc->output_flags & OUTPUT_FLAG__USE_UTC)
        sc->thiszone = 0;
#ifndef VALGRIND_TESTING
    else
        sc->thiszone = gmt2local(0);
#endif

    if ( !InspectorManager::configure(sc) )
    {
        SnortConfFree(sc);
        return NULL;
    }

    FlowbitResetCounts();
    ParseRules(sc);

    // FIXIT see SnortInit() on config printing
    //detection_filter_print_config(sc->detection_filter_config);
    ////RateFilter_PrintConfig(sc->rate_filter_config);
    //print_thresholding(sc->threshold_config, 0);
    //PrintRuleOrder(sc->rule_lists);

    SetRuleStates(sc);
    SetPortFilterLists(sc);

    /* Need to do this after dynamic detection stuff is initialized, too */
    IpsManager::verify();

    if ((sc->file_mask != 0) && (sc->file_mask != snort_conf->file_mask))
        umask(sc->file_mask);

    /* Transfer any user defined rule type outputs to the new rule list */
    {
        RuleListNode *cur = snort_conf->rule_lists;

        for (; cur != NULL; cur = cur->next)
        {
            RuleListNode *rnew = sc->rule_lists;

            for (; rnew != NULL; rnew = rnew->next)
            {
                if (strcasecmp(cur->name, rnew->name) == 0)
                {
                    EventManager::copy_outputs(
                        rnew->RuleList->AlertList, cur->RuleList->AlertList);

                    EventManager::copy_outputs(
                        rnew->RuleList->LogList, cur->RuleList->LogList);
                    break;
                }
            }
        }
    }

    fpCreateFastPacketDetection(sc);

    if ( sc->fast_pattern_config->search_api !=
            snort_conf->fast_pattern_config->search_api )
    {
        MpseManager::activate_search_engine(sc);
    }

#ifdef PPM_MGR
    PPM_PRINT_CFG(&sc->ppm_cfg);
#endif

    return sc;
}

SnortConfig* reload_config()
{
    SnortConfig* new_conf = get_reload_config();

    if ( new_conf )
    {
        proc_stats.conf_reloads++;
        snort_conf = new_conf;
    }
    return new_conf;
}
