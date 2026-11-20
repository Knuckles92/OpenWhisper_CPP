#include "process/win/job.h"

#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <vector>

namespace fv::process_win {

HANDLE create_kill_on_close_job(std::uint32_t& error) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        error = GetLastError();
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = GetLastError();
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool clear_job_limits(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits)) != FALSE;
}

namespace {

void terminate(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process != nullptr) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

} // namespace

void terminate_tree(std::uint32_t pid) {
    std::multimap<DWORD, DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL found = Process32FirstW(snapshot, &entry); found;
             found = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID != entry.th32ParentProcessID) {
                children.emplace(entry.th32ParentProcessID, entry.th32ProcessID);
            }
        }
        CloseHandle(snapshot);
    }
    // Parents first, so none can start another child after its children were ended.
    std::vector<DWORD> order{pid};
    for (std::size_t i = 0; i < order.size() && order.size() < 4096; ++i) {
        const auto [first, last] = children.equal_range(order[i]);
        for (auto it = first; it != last; ++it) {
            if (std::find(order.begin(), order.end(), it->second) == order.end()) {
                order.push_back(it->second);
            }
        }
    }
    for (const DWORD each : order) {
        terminate(each);
    }
}

bool in_job(HANDLE process) {
    BOOL result = FALSE;
    return IsProcessInJob(process, nullptr, &result) && result;
}

} // namespace fv::process_win
