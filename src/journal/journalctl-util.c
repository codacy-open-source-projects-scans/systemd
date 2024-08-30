/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "id128-util.h"
#include "journal-util.h"
#include "journalctl.h"
#include "journalctl-util.h"
#include "logs-show.h"
#include "rlimit-util.h"
#include "strv.h"
#include "terminal-util.h"

char* format_timestamp_maybe_utc(char *buf, size_t l, usec_t t) {
        assert(buf);

        if (arg_utc)
                return format_timestamp_style(buf, l, t, TIMESTAMP_UTC);

        return format_timestamp(buf, l, t);
}

int acquire_journal(sd_journal **ret) {
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        int r;

        assert(ret);

        journal_browse_prepare();

        if (arg_directory)
                r = sd_journal_open_directory(&j, arg_directory, arg_journal_type | arg_journal_additional_open_flags);
        else if (arg_root)
                r = sd_journal_open_directory(&j, arg_root, arg_journal_type | arg_journal_additional_open_flags | SD_JOURNAL_OS_ROOT);
        else if (arg_file_stdin)
                r = sd_journal_open_files_fd(&j, (int[]) { STDIN_FILENO }, 1, arg_journal_additional_open_flags);
        else if (arg_file)
                r = sd_journal_open_files(&j, (const char**) arg_file, arg_journal_additional_open_flags);
        else if (arg_machine)
                r = journal_open_machine(&j, arg_machine, arg_journal_additional_open_flags);
        else
                r = sd_journal_open_namespace(
                                &j,
                                arg_namespace,
                                (arg_merge ? 0 : SD_JOURNAL_LOCAL_ONLY) |
                                arg_namespace_flags | arg_journal_type | arg_journal_additional_open_flags);
        if (r < 0)
                return log_error_errno(r, "Failed to open %s: %m", arg_directory ?: arg_file ? "files" : "journal");

        r = journal_access_check_and_warn(j, arg_quiet,
                                          !(arg_journal_type == SD_JOURNAL_CURRENT_USER || arg_user_units));
        if (r < 0)
                return r;

        *ret = TAKE_PTR(j);
        return 0;
}

bool journal_boot_has_effect(sd_journal *j) {
        assert(j);

        if (arg_boot_offset != 0 &&
            sd_journal_has_runtime_files(j) > 0 &&
            sd_journal_has_persistent_files(j) == 0) {
                log_info("Specifying boot ID or boot offset has no effect, no persistent journal was found.");
                return false;
        }

        return true;
}

int journal_acquire_boot(sd_journal *j) {
        int r;

        assert(j);

        if (!arg_boot) {
                /* Clear relevant field for safety. */
                arg_boot_id = SD_ID128_NULL;
                arg_boot_offset = 0;
                return 0;
        }

        /* Take a shortcut and use the current boot_id, which we can do very quickly.
         * We can do this only when the logs are coming from the current machine,
         * so take the slow path if log location is specified. */
        if (arg_boot_offset == 0 && sd_id128_is_null(arg_boot_id) &&
            !arg_directory && !arg_file && !arg_file_stdin && !arg_root) {
                r = id128_get_boot_for_machine(arg_machine, &arg_boot_id);
                if (r < 0)
                        return log_error_errno(r, "Failed to get boot ID%s%s: %m",
                                               isempty(arg_machine) ? "" : " of container ", strempty(arg_machine));
        } else {
                sd_id128_t boot_id;

                r = journal_find_boot(j, arg_boot_id, arg_boot_offset, &boot_id);
                if (r < 0)
                        return log_error_errno(r, "Failed to find journal entry for the specified boot (%s%+i): %m",
                                               sd_id128_is_null(arg_boot_id) ? "" : SD_ID128_TO_STRING(arg_boot_id),
                                               arg_boot_offset);
                if (r == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENODATA),
                                               "No journal boot entry found for the specified boot (%s%+i).",
                                               sd_id128_is_null(arg_boot_id) ? "" : SD_ID128_TO_STRING(arg_boot_id),
                                               arg_boot_offset);

                log_debug("Found boot %s for %s%+i",
                          SD_ID128_TO_STRING(boot_id),
                          sd_id128_is_null(arg_boot_id) ? "" : SD_ID128_TO_STRING(arg_boot_id),
                          arg_boot_offset);

                arg_boot_id = boot_id;
        }

        return 1;
}

int acquire_unit(const char *option_name, const char **ret_unit, LogIdType *ret_type) {
        size_t n;

        assert(option_name);
        assert(ret_unit);
        assert(ret_type);

        n = strv_length(arg_system_units) + strv_length(arg_user_units);
        if (n <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Using %s requires a unit. Please specify a unit name with -u/--unit=/--user-unit=.",
                                       option_name);
        if (n > 1)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Using %s with multiple units is not supported.",
                                       option_name);

        if (!strv_isempty(arg_system_units)) {
                *ret_type = LOG_SYSTEM_UNIT_INVOCATION_ID;
                *ret_unit = arg_system_units[0];
        } else {
                assert(!strv_isempty(arg_user_units));
                *ret_type = LOG_USER_UNIT_INVOCATION_ID;
                *ret_unit = arg_user_units[0];
        }

        return 0;
}

int journal_acquire_invocation(sd_journal *j) {
        LogIdType type = LOG_SYSTEM_UNIT_INVOCATION_ID;
        const char *unit = NULL;
        sd_id128_t id;
        int r;

        assert(j);

        /* journal_acquire_boot() must be called before this. */

        if (!arg_invocation) {
                /* Clear relevant field for safety. */
                arg_invocation_id = SD_ID128_NULL;
                arg_invocation_offset = 0;
                return 0;
        }

        /* When an invocation ID is explicitly specified without an offset, we do not care the ID is about
         * system unit or user unit, and calling without unit name is allowed. Otherwise, a unit name must
         * be specified. */
        if (arg_invocation_offset != 0 || sd_id128_is_null(arg_invocation_id)) {
                r = acquire_unit("-I/--invocation= with an offset", &unit, &type);
                if (r < 0)
                        return r;
        }

        r = journal_find_log_id(j, type, arg_boot_id, unit, arg_invocation_id, arg_invocation_offset, &id);
        if (r < 0)
                return log_error_errno(r, "Failed to find journal entry for the invocation (%s%+i): %m",
                                       sd_id128_is_null(arg_invocation_id) ? "" : SD_ID128_TO_STRING(arg_invocation_id),
                                       arg_invocation_offset);
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(ENODATA),
                                       "No journal entry found for the invocation (%s%+i).",
                                       sd_id128_is_null(arg_invocation_id) ? "" : SD_ID128_TO_STRING(arg_invocation_id),
                                       arg_invocation_offset);

        log_debug("Found invocation ID %s for %s%+i",
                  SD_ID128_TO_STRING(id),
                  sd_id128_is_null(arg_invocation_id) ? "" : SD_ID128_TO_STRING(arg_invocation_id),
                  arg_invocation_offset);

        arg_invocation_id = id;
        return 1;
}
