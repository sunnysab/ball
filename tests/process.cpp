// ball: process identity, bounded argv reads and thread-name regression tests.
#include "process.h"
#include "daemon/processes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {
constexpr int kPid = 42;
constexpr quint64 kStartNs = 100'000'000'000;

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QByteArray statLine(const QByteArray& name, quint64 seconds = 100) {
    QByteArray data = QByteArray::number(kPid) + " (" + name + ") S";
    for (int field = 1; field <= 19; ++field) {
        data += ' ';
        data += field == 19 ? QByteArray::number(seconds * quint64(::sysconf(_SC_CLK_TCK))) : "0";
    }
    return data + '\n';
}
} // namespace

class TestProcess : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init();
    void verifiesIdentityAndParsesParentheses();
    void commandLinePreservesArgumentBoundaries();
    void rejectsUnavailableMalformedAndOversizedData();
    void leaderNameReplacesThreadNameAndSurvivesExit();
    void pidReuseDoesNotBorrowAnotherProcessesMetadata();

private:
    QTemporaryDir m_directory;
    QString m_root;
    QString m_stat;
    QString m_cmdline;
};

void TestProcess::init() {
    QVERIFY(m_directory.isValid());
    m_root = m_directory.filePath(QString::fromLatin1(QTest::currentTestFunction()));
    QVERIFY(QDir().mkpath(m_root + u"/42"_s));
    m_stat = m_root + u"/42/stat"_s;
    m_cmdline = m_root + u"/42/cmdline"_s;
    QVERIFY(writeFile(m_stat, statLine("real ) application")));
}

void TestProcess::verifiesIdentityAndParsesParentheses() {
    QCOMPARE(ball::readProcessName({kPid, kStartNs}, m_root), u"real ) application"_s);
    QVERIFY(ball::readProcessName({kPid, kStartNs + 1'000'000'000}, m_root).isEmpty());
    QVERIFY(ball::readProcessName({0, kStartNs}, m_root).isEmpty());
    QVERIFY(ball::readProcessName({-1, kStartNs}, m_root).isEmpty());
}

void TestProcess::commandLinePreservesArgumentBoundaries() {
    QByteArray command;
    for (const auto& arg :
         {QByteArray("/usr/bin/app"), QByteArray("--profile"), QByteArray("path with spaces"),
          QByteArray("<tag>&'quoted'"), QByteArray(), QByteArray()}) {
        command += arg + '\0';
    }
    QVERIFY(writeFile(m_cmdline, command));
    QCOMPARE(ball::readProcessCommandLine({kPid, kStartNs}, m_root),
             QStringList({u"/usr/bin/app"_s, u"--profile"_s, u"path with spaces"_s,
                          u"<tag>&'quoted'"_s, QString(), QString()}));
    QVERIFY(ball::readProcessCommandLine({kPid, kStartNs + 1'000'000'000}, m_root).isEmpty());
    QVERIFY(writeFile(m_stat, statLine("")));
    QCOMPARE(ball::readProcessCommandLine({kPid, kStartNs}, m_root).size(), 6);
}

void TestProcess::rejectsUnavailableMalformedAndOversizedData() {
    QVERIFY(ball::readProcessCommandLine({kPid, kStartNs}, m_root).isEmpty());
    QVERIFY(writeFile(m_cmdline, QByteArray(64 * 1024 + 1, 'x')));
    QVERIFY(ball::readProcessCommandLine({kPid, kStartNs}, m_root).isEmpty());
    QVERIFY(writeFile(m_cmdline, {}));
    QVERIFY(ball::readProcessCommandLine({kPid, kStartNs}, m_root).isEmpty());
    for (const auto& data : {QByteArray("bad stat"), QByteArray("42 (app) S"),
                             QByteArray(4097, 'x'), QByteArray("wrong") + statLine("app")}) {
        QVERIFY(writeFile(m_stat, data));
        QVERIFY(ball::readProcessName({kPid, kStartNs}, m_root).isEmpty());
    }
}

void TestProcess::leaderNameReplacesThreadNameAndSurvivesExit() {
    ball::Processes processes(m_root);
    ball::ProcessCounter counter{kPid, kStartNs, u"tokio-rt-worker"_s, 100, 200};
    const auto now = QDateTime::currentDateTime();
    QVERIFY(processes.update({counter}, now, 0));
    QCOMPARE(processes.row(kPid, kStartNs).name, u"real ) application"_s);
    QVERIFY(QFile::remove(m_stat));
    counter.rxBytes += 100;
    counter.name = u"another-worker"_s;
    QVERIFY(processes.update({counter}, now, 1000));
    QCOMPARE(processes.row(kPid, kStartNs).name, u"real ) application"_s);
    QCOMPARE(processes.row(kPid, kStartNs).session.rxBytes, quint64(100));
    QVERIFY(processes.update({}, now, 2000));
    QVERIFY(!processes.row(kPid, kStartNs).present);
    QCOMPARE(processes.row(kPid, kStartNs).name, u"real ) application"_s);
}

void TestProcess::pidReuseDoesNotBorrowAnotherProcessesMetadata() {
    ball::Processes processes(m_root);
    ball::ProcessCounter old{kPid, kStartNs, u"tokio-rt-worker"_s, 100, 200};
    const auto now = QDateTime::currentDateTime();
    QVERIFY(processes.update({old}, now, 0));
    QVERIFY(writeFile(m_stat, statLine("new application", 200)));
    QVERIFY(writeFile(m_cmdline, QByteArray("new-app\0--private\0", 18)));
    QVERIFY(ball::readProcessCommandLine({kPid, kStartNs}, m_root).isEmpty());
    const ball::ProcessCounter fresh{kPid, 200'000'000'000, u"worker"_s, 10, 20};
    QVERIFY(processes.update({fresh}, now, 1000));
    QCOMPARE(processes.row(kPid, kStartNs).name, u"real ) application"_s);
    QCOMPARE(processes.row(kPid, fresh.startNs).name, u"new application"_s);
}

QTEST_GUILESS_MAIN(TestProcess)
#include "process.moc"
