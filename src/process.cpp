// ball: bounded /proc reads, verified against the collector's process identity.
#include "process.h"

#include <QFile>

#include <optional>

#include <unistd.h>

namespace ball {
namespace {
constexpr qint64 kMaxStatBytes = 4096;
constexpr qint64 kMaxCommandLineBytes = 64LL * 1024;
constexpr quint64 kNanosecondsPerSecond = 1'000'000'000;

QString processPath(const QString& root, int pid, const char* file) {
    return root + QLatin1Char('/') + QString::number(pid) + QLatin1Char('/') + QLatin1String(file);
}

std::optional<QString> verifiedName(ProcessIdentity process, const QString& procRoot) {
    if (process.pid <= 0) {
        return {};
    }
    QFile stat(processPath(procRoot, process.pid, "stat"));
    if (!stat.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = stat.read(kMaxStatBytes + 1);
    const qsizetype open = data.indexOf('(');
    const qsizetype close = data.lastIndexOf(')');
    if (data.size() > kMaxStatBytes || open < 0 || close <= open) {
        return {};
    }
    bool validPid = false;
    const int statPid = data.first(open).trimmed().toInt(&validPid);
    const auto fields = data.mid(close + 1).simplified().split(' ');
    if (!validPid || statPid != process.pid || fields.size() <= 19) {
        return {};
    }
    bool validStart = false;
    const quint64 startTicks = fields.at(19).toULongLong(&validStart);
    static const long ticksPerSecond = ::sysconf(_SC_CLK_TCK);
    if (!validStart || ticksPerSecond <= 0 || quint64(ticksPerSecond) > kNanosecondsPerSecond) {
        return {};
    }
    const auto hz = quint64(ticksPerSecond);
    // Divide first: multiplying a nanosecond timestamp by HZ could overflow.
    const quint64 expectedTicks =
        (process.startNs / kNanosecondsPerSecond) * hz +
        (process.startNs % kNanosecondsPerSecond) * hz / kNanosecondsPerSecond;
    if (startTicks != expectedTicks) {
        return {};
    }
    return QString::fromLocal8Bit(data.mid(open + 1, close - open - 1));
}

} // namespace

QString readProcessName(ProcessIdentity process, const QString& procRoot) {
    return verifiedName(process, procRoot).value_or(QString());
}

QStringList readProcessCommandLine(ProcessIdentity process, const QString& procRoot) {
    if (!verifiedName(process, procRoot).has_value()) {
        return {};
    }
    QFile commandLine(processPath(procRoot, process.pid, "cmdline"));
    if (!commandLine.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = commandLine.read(kMaxCommandLineBytes + 1);
    if (data.isEmpty() || data.size() > kMaxCommandLineBytes ||
        !verifiedName(process, procRoot).has_value()) {
        return {};
    }
    auto arguments = data.split('\0');
    if (data.endsWith('\0')) {
        arguments.removeLast();
    }
    QStringList result;
    result.reserve(arguments.size());
    for (const auto& argument : arguments) {
        result.append(QString::fromLocal8Bit(argument));
    }
    return result;
}

} // namespace ball
