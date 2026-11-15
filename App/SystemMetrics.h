#pragma once

#include <QString>
#include <QtTypes>
#include <memory>
#include <optional>

struct SystemMetricsSnapshot {
    std::optional<double> cpu, gpu;
    std::optional<quint64> ramUsed, ramTotal, vramUsed, vramTotal;
};

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();
    SystemMetricsSnapshot sample(const QString &adapter, quint64 dedicatedCapacity, quint64 sharedCapacity);

private:
    struct Platform;
    std::unique_ptr<Platform> platform_;
};
