#pragma once

// Unity may unwind a managed exception through the native avatar update. A
// latched re-entry flag must not turn that one failed frame into a permanent
// freeze while the independent network/HUD paths continue receiving poses.
template<class Update, class ReportFailure>
void run_avatar_update_frame(bool& updating, Update update, ReportFailure report_failure) {
    if (updating) return;
    try {
        struct ResetOnExit {
            bool& flag;
            explicit ResetOnExit(bool& value) : flag(value) { flag = true; }
            ~ResetOnExit() { flag = false; }
            ResetOnExit(const ResetOnExit&) = delete;
            ResetOnExit& operator=(const ResetOnExit&) = delete;
        } reset(updating);
        update();
    } catch (...) {
        // The guard has already unwound. Reporting or a later frame can safely
        // enter again; no reset depends on reaching the bottom of update().
        report_failure();
    }
}
