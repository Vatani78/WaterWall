#include "api.h"
#include "utils/fileutils.h"
#include "utils/stringutils.h"
#include "managers/socket_manager.h"
#include "managers/node_manager.h"
#include "core_settings.h"
#include "static_tunnels.h"
#include "loggers/dns_logger.h"
#include "loggers/network_logger.h"
#include "loggers/core_logger.h"

#define CORE_FILE "core.json"

#ifdef OS_LINUX
#include <sys/resource.h>
static void increaseFileLimit()
{

    struct rlimit rlim;
    // Get the current limit
    if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
    {
        LOGF("Core: getrlimit failed");
        exit(EXIT_FAILURE);
    }
    if ((unsigned long)rlim.rlim_max < 8192)
    {
        LOGW("Core: Maximum allowed open files limit is %lu which is below 8192 !\n if you are running as a server then \
        you might experince time-outs if this limits gets reached, depends on how many clients are connected to the server simultaneously\
        ", (unsigned long)rlim.rlim_max);
    }
    else
        LOGD("Core: File limit  %lu -> %lu", (unsigned long)rlim.rlim_cur, (unsigned long)rlim.rlim_max);
    // Set the hard limit to the maximum allowed value
    rlim.rlim_cur = rlim.rlim_max;
    // Apply the new limit
    if (setrlimit(RLIMIT_NOFILE, &rlim) == -1)
    {
        LOGF("Core: setrlimit failed");
        exit(EXIT_FAILURE);
    }
}
#else
static void increaseFileLimit() { (void)(0); }
#endif

int main(int argc, char **argv)
{
    // test ASAN works -_-
    // int test[3] = {0};
    // printf("hello world %d", test[4]);

    initCoreSettings();
    char *core_file_content = readFile(CORE_FILE);
    if (core_file_content == NULL)
    {
        fprintf(stderr, "Waterwall version %s\n Could not read core settings file \"%s\" \n",
                TOSTRING(WATERWALL_VERSION), CORE_FILE);
        exit(1);
    }
    parseCoreSettings(core_file_content);

    //  [Runtime setup]
    {
        hv_mkdir_p(getCoreSettings()->log_path);
        char *core_log_file_path = concat(getCoreSettings()->log_path, getCoreSettings()->core_log_file);
        char *network_log_file_path = concat(getCoreSettings()->log_path, getCoreSettings()->network_log_file);
        char *dns_log_file_path = concat(getCoreSettings()->log_path, getCoreSettings()->dns_log_file);

        // libhv has a separate logger, we forward it to the core logger
        logger_set_level_by_str(hv_default_logger(), getCoreSettings()->core_log_level);
        logger_set_handler(hv_default_logger(), getCoreLoggerHandle(getCoreSettings()->core_log_console));

        createWW(
            core_log_file_path,
            network_log_file_path,
            dns_log_file_path,
            getCoreSettings()->core_log_level,
            getCoreSettings()->network_log_level,
            getCoreSettings()->dns_log_level,
            getCoreSettings()->core_log_console,
            getCoreSettings()->network_log_console,
            getCoreSettings()->dns_log_console,
            getCoreSettings()->threads);

        free(core_log_file_path);
        free(network_log_file_path);
        free(dns_log_file_path);
    }
    LOGI("Starting Waterwall version %s", TOSTRING(WATERWALL_VERSION));
    LOGI("Parsing core file complete");
    increaseFileLimit();
    loadStaticTunnelsIntoCore();

    //  [Parse ConfigFiles]
    //  TODO this currently runs only 1 config file
    {
        c_foreach(k, vec_config_path_t, getCoreSettings()->config_paths)
        {
            // read config file
            LOGD("Core: begin parsing config file \"%s\"", *k.ref);
            config_file_t *cfile = parseConfigFile(*k.ref);

            if (cfile == NULL)
            {
                LOGF("Core: could not read core file \"%s\" ", *k.ref);
                exit(1);
            }

            LOGI("Core: parsing config file \"%s\" complete", *k.ref);
            runConfigFile(cfile);
            LOGD("Core: starting eventloops ...");
            startSocketManager();
            runMainThread();
            LOGW("Core: Eventloops are finished");
        }
    }
}