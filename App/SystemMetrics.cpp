#include "App/SystemMetrics.h"

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <QHash>
#include <QVarLengthArray>

namespace {
quint64 ticks(FILETIME time)
{
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

template<class Read>
void readInstances(PDH_HCOUNTER counter, Read read)
{
    if (!counter)
        return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                     &size, &count, nullptr) != PDH_MORE_DATA || size > 16*1024*1024)
        return;
    QVarLengthArray<quint64, 512> buffer((size + sizeof(quint64)-1)/sizeof(quint64));
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; i < count; ++i) {
        const auto &value = items[i].FmtValue;
        if ((value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)
            && std::isfinite(value.doubleValue) && value.doubleValue >= 0)
            read(QString::fromWCharArray(items[i].szName).toLower(), value.doubleValue);
    }
}
}
#endif

struct SystemMetrics::Platform {
#ifdef Q_OS_WIN
    PDH_HQUERY query{};
    PDH_HCOUNTER engines{}, dedicated{}, shared{};
    std::optional<quint64> lastTotal, lastIdle;

    Platform()
    {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
            return;
        PdhAddEnglishCounterW(query, LR"(\GPU Engine(*)\Utilization Percentage)", 0, &engines);
        PdhAddEnglishCounterW(query, LR"(\GPU Adapter Memory(*)\Dedicated Usage)", 0, &dedicated);
        PdhAddEnglishCounterW(query, LR"(\GPU Adapter Memory(*)\Shared Usage)", 0, &shared);
        PdhCollectQueryData(query);
    }
    ~Platform()
    {
        if (query)
            PdhCloseQuery(query);
    }
#endif
};

SystemMetrics::SystemMetrics() : platform_(std::make_unique<Platform>()) {}
SystemMetrics::~SystemMetrics() = default;

SystemMetricsSnapshot SystemMetrics::sample(const QString &adapter, quint64 dedicatedCapacity, quint64 sharedCapacity)
{
    SystemMetricsSnapshot result;
#ifdef Q_OS_WIN
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto total = ticks(kernel) + ticks(user);
        const auto idleTicks = ticks(idle);
        if (platform_->lastTotal && total > *platform_->lastTotal && idleTicks >= *platform_->lastIdle) {
            const auto elapsed = double(total - *platform_->lastTotal);
            result.cpu = std::clamp(100.0 * (1.0 - double(idleTicks - *platform_->lastIdle)/elapsed), 0.0, 100.0);
        }
        platform_->lastTotal = total;
        platform_->lastIdle = idleTicks;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys >= memory.ullAvailPhys) {
        result.ramUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        result.ramTotal = memory.ullTotalPhys;
    }
    if (adapter.isEmpty() || !platform_->query || PdhCollectQueryData(platform_->query) != ERROR_SUCCESS)
        return result;

    // Instances contain a process prefix. Sum processes per engine and use the busiest engine.
    QHash<QString, double> utilization;
    const auto prefix = adapter + "_phys_";
    readInstances(platform_->engines, [&](const QString &name, double value) {
        const auto start = name.indexOf(prefix);
        if (start >= 0)
            utilization[name.mid(start)] += value;
    });
    if (!utilization.isEmpty())
        result.gpu = std::clamp(*std::ranges::max_element(utilization), 0.0, 100.0);
    const auto capacity = dedicatedCapacity ? dedicatedCapacity : sharedCapacity;
    if (capacity) {
        result.vramTotal = capacity;
        readInstances(dedicatedCapacity ? platform_->dedicated : platform_->shared, [&](const QString &name, double value) {
            if (name.startsWith(prefix) && value <= double(capacity))
                result.vramUsed = result.vramUsed.value_or(0) + quint64(value);
        });
    }
#else
    Q_UNUSED(adapter);
    Q_UNUSED(dedicatedCapacity);
    Q_UNUSED(sharedCapacity);
#endif
    return result;
}
