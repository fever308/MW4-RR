#pragma once
#include <windows.h>
#include <cstdio>
// Caps the test process's committed memory with a job object, so that DXC
// disassembling a corrupted bitcode fails with an allocation error instead of
// filling the machine's memory. Fails the test when the cap cannot be applied.
inline bool CapTestMemory(SIZE_T bytes = SIZE_T(4) << 30) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = bytes;
    const bool capped = job &&
                        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit,
                                                sizeof(limit)) &&
                        AssignProcessToJobObject(job, GetCurrentProcess());
    if (!capped)
        std::fprintf(stderr, "FAIL: memory cap not applied (error %lu)\n", GetLastError());
    return capped; // the handle stays open: the job lives as long as the process
}
