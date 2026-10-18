// Exercise PVA callback ordering and failures without a network server.
#include "services/pv_access.h"
#include <QtTest>
#ifdef _MSC_VER
// EPICS PVString intentionally inherits deserialize through virtual bases.
#pragma warning(push)
#pragma warning(disable: 4250)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <pv/clientFactory.h>
#include <pv/pvAccess.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cmath>
namespace pva = epics::pvAccess;
namespace pvd = epics::pvData;
using namespace alh;
namespace {
pvd::StructureConstPtr scalarType() {
  return pvd::getFieldCreate()->createFieldBuilder()
      ->add("value", pvd::pvDouble)
      ->addNestedStructure("alarm")
        ->add("severity", pvd::pvInt)->add("status", pvd::pvInt)
        ->add("message", pvd::pvString)->endNested()
      ->createStructure();
}
struct TestMonitor final : pva::Monitor, std::enable_shared_from_this<TestMonitor> {
  pva::MonitorRequester::shared_pointer requester;
  std::deque<pva::MonitorElementPtr> queue;
  pvd::Status startResult;
  bool destroyed = false;
  pvd::Status start() override { return startResult; }
  pvd::Status stop() override { return pvd::Status::Ok; }
  void destroy() override { destroyed = true; }
  pva::MonitorElementPtr poll() override {
    if (queue.empty()) return {};
    auto next = queue.front(); queue.pop_front(); return next;
  }
  void release(const pva::MonitorElementPtr&) override {}
  void connect() { requester->monitorConnect(pvd::Status::Ok, shared_from_this(), scalarType()); }
  void fail() { requester->monitorConnect(pvd::Status::error("monitor refused"), shared_from_this(), {}); }
  void post(double value, int severity = 0, const std::string& message = {}) {
    auto data = pvd::getPVDataCreate()->createPVStructure(scalarType());
    data->getSubField<pvd::PVDouble>("value")->put(value);
    data->getSubField<pvd::PVInt>("alarm.severity")->put(severity);
    data->getSubField<pvd::PVString>("alarm.message")->put(message);
    auto element = std::make_shared<pva::MonitorElement>(data);
    element->changedBitSet->set(0);
    queue.push_back(element);
    requester->monitorEvent(shared_from_this());
  }
};
struct TestPut final : pva::ChannelPut, std::enable_shared_from_this<TestPut> {
  pva::ChannelPutRequester::shared_pointer requester;
  std::weak_ptr<pva::Channel> channel;
  QVector<double> submitted;
  bool destroyed = false;
  pva::Channel::shared_pointer getChannel() override { return channel.lock(); }
  void lock() override {}
  void unlock() override {}
  void cancel() override {}
  void lastRequest() override {}
  void destroy() override { destroyed = true; }
  void get() override {}
  void put(const pvd::PVStructurePtr& data, const pvd::BitSetPtr&) override {
    submitted << data->getSubField<pvd::PVDouble>("value")->get();
  }
  void ready() { requester->channelPutConnect(pvd::Status::Ok, shared_from_this(), scalarType()); }
  void refuse() { requester->channelPutConnect(pvd::Status::error("write refused"), shared_from_this(), {}); }
  void done() { requester->putDone(pvd::Status::Ok, shared_from_this()); }
  // QSRV rejects a put denied by access security when it completes.
  void reject() { requester->putDone(pvd::Status::error("Put not permitted"), shared_from_this()); }
};
struct TestChannel final : pva::Channel, std::enable_shared_from_this<TestChannel> {
  pva::ChannelProvider::shared_pointer provider;
  pva::ChannelRequester::shared_pointer requester;
  std::string name;
  std::shared_ptr<TestMonitor> monitor;
  std::shared_ptr<TestPut> writer;
  std::string getChannelName() override { return name; }
  std::string getRemoteAddress() override { return "test"; }
  pva::ChannelProvider::shared_pointer getProvider() override { return provider; }
  pva::ChannelRequester::shared_pointer getChannelRequester() override { return requester; }
  void destroy() override {}
  pva::MonitorPtr createMonitor(const pva::MonitorRequester::shared_pointer& callback,
                               const pvd::PVStructurePtr&) override {
    monitor = std::make_shared<TestMonitor>();
    monitor->requester = callback;
    return monitor;
  }
  pva::ChannelPut::shared_pointer createChannelPut(const pva::ChannelPutRequester::shared_pointer& callback,
                                                  const pvd::PVStructurePtr&) override {
    writer = std::make_shared<TestPut>();
    writer->requester = callback;
    writer->channel = shared_from_this();
    return writer;
  }
  void state(ConnectionState state) { requester->channelStateChange(shared_from_this(), state); }
};
struct TestProvider final : pva::ChannelProvider, std::enable_shared_from_this<TestProvider> {
  std::vector<std::shared_ptr<TestChannel>> channels;
  bool connectOnCreate = true;
  std::string getProviderName() override { return "pva"; }
  void destroy() override {}
  pva::ChannelFind::shared_pointer channelFind(const std::string&,
                                             const pva::ChannelFindRequester::shared_pointer&) override { return {}; }
  pva::Channel::shared_pointer createChannel(const std::string& name,
      const pva::ChannelRequester::shared_pointer& requester, short, const std::string&) override {
    auto channel = std::make_shared<TestChannel>();
    channel->provider = shared_from_this();
    channel->requester = requester;
    channel->name = name;
    channels.push_back(channel);
    requester->channelCreated(pvd::Status::Ok, channel);
    if (connectOnCreate)
      channel->state(pva::Channel::CONNECTED);
    return channel;
  }
};
}
class PvaTests : public QObject {
  Q_OBJECT
  std::shared_ptr<TestProvider> provider;
  pva::ChannelProviderFactory::shared_pointer original;
private slots:
  void init() {
    pva::ClientFactory::start();
    auto registry = pva::ChannelProviderRegistry::clients();
    original = registry->getFactory("pva");
    provider = std::make_shared<TestProvider>();
    registry->addSingleton(provider);
  }
  void cleanup() {
    auto registry = pva::ChannelProviderRegistry::clients();
    registry->remove("pva");
    if (original) registry->add(original);
    provider->channels.clear();
    provider.reset();
  }
  void failedAlarmMonitor() {
    PvAccess pv;
    Event last;
    last.severity = -1;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    provider->channels.back()->monitor->fail();
    QTRY_COMPARE(last.severity, 4);
  }
  void failedNumericMonitor() {
    PvAccess pv;
    double last = -1;
    pv.number("pva://input", [&](double value) { last = value; });
    auto monitor = provider->channels.back()->monitor;
    monitor->post(5);
    QTRY_COMPARE(last, 5.0);
    monitor->fail();
    QTRY_VERIFY(std::isnan(last));
    monitor->post(6);
    QTRY_COMPARE(last, 6.0);
  }
  void monitorStartFailure() {
    PvAccess pv;
    Event last;
    last.severity = -1;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    auto monitor = provider->channels.back()->monitor;
    monitor->startResult = pvd::Status::error("start refused");
    monitor->connect();
    QTRY_COMPARE(last.severity, 4);
  }
  void endedMonitor() {
    PvAccess pv;
    Event last;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    auto monitor = provider->channels.back()->monitor;
    monitor->post(1);
    QTRY_COMPARE(last.value, QString("1"));
    monitor->requester->unlisten(monitor);
    QTRY_COMPARE(last.severity, 4);
  }
  void staleDataAfterDisconnect() {
    PvAccess pv;
    Event last;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    auto channel = provider->channels.back();
    channel->monitor->post(1);
    QTRY_COMPARE(last.value, QString("1"));
    channel->state(pva::Channel::DISCONNECTED);
    channel->monitor->post(2); // A late callback from the old connection.
    QCoreApplication::processEvents();
    QCOMPARE(last.severity, 4);
    QVERIFY(!pv.canWrite("pva://alarm"));
    channel->state(pva::Channel::CONNECTED);
    channel->monitor->post(3);
    QTRY_COMPARE(last.value, QString("3"));
    QCOMPARE(last.severity, 0);
  }
  void stalePutCompletionAfterReconnect() {
    PvAccess pv;
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto writer = channel->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    QCOMPARE(writer->submitted, QVector<double>{1});
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QCoreApplication::processEvents();
    auto recovered = channel->writer;
    recovered->ready();
    QCoreApplication::processEvents();
    QVERIFY(pv.put("pva://output", 2));
    QVERIFY(pv.put("pva://output", 3));
    writer->done(); // Completion from before the disconnect must be ignored.
    QCoreApplication::processEvents();
    QCOMPARE(recovered->submitted.back(), 2.0);
    recovered->done();
    QTRY_COMPARE(recovered->submitted.back(), 3.0);
  }
  void callbacksCanClearService() {
    PvAccess pv;
    int alarms = 0, numbers = 0;
    pv.monitor("pva://alarm", [&](Event) { ++alarms; pv.clear(); });
    provider->channels.back()->monitor->post(1);
    QTRY_COMPARE(alarms, 1);
    pv.error = [&](const QString&) { pv.clear(); };
    pv.number("pva://input", [&](double) { ++numbers; });
    provider->channels.back()->state(pva::Channel::DISCONNECTED);
    QCoreApplication::processEvents();
    QCOMPARE(numbers, 0); // clear() released the callback during the diagnostic.
  }
  void changedAlarmMessageNeedsAcknowledgement() {
    PvAccess pv({true});
    Event last;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    auto monitor = provider->channels.back()->monitor;
    monitor->post(1, 2, "temperature");
    QTRY_COMPARE(last.acks, 2);
    QVERIFY(pv.put("pva://alarm", 2, WriteKind::Acknowledge));
    QTRY_COMPARE(last.acks, 0);
    monitor->post(2, 2, "pressure");
    QTRY_COMPARE(last.acks, 2);
  }
  void duplicateObserversDoNotReplayAcknowledgedTransitions() {
    PvAccess pv({true});
    int first = 0, second = 0;
    Event a, b;
    pv.monitor("pva://alarm", [&](Event e) { a = e; }, &first);
    auto firstMonitor = provider->channels.back()->monitor;
    pv.monitor("pva://alarm", [&](Event e) { b = e; }, &second);
    auto secondMonitor = provider->channels.back()->monitor;
    firstMonitor->post(0);
    if (secondMonitor != firstMonitor) secondMonitor->post(0);
    QCoreApplication::processEvents();
    firstMonitor->post(1, 2);
    firstMonitor->post(0);
    QTRY_COMPARE(a.acks, 2);
    QVERIFY(pv.put("pva://alarm", 2, WriteKind::Acknowledge));
    QTRY_COMPARE(a.acks, 0);
    // Independent monitor queues can deliver one observer's old transition
    // after another observer has already acknowledged the recovered alarm.
    if (secondMonitor != firstMonitor) {
      secondMonitor->post(1, 2);
      secondMonitor->post(0);
    }
    QCoreApplication::processEvents();
    QCOMPARE(b.acks, 0);
    QCOMPARE(a.acks, 0);
    pv.cancel("pva://alarm", &first);
    firstMonitor->post(3, 1);
    QTRY_COMPARE(b.severity, 1);
    QCOMPARE(a.severity, 0);
    pv.cancel("pva://alarm", &second);
    QVERIFY(firstMonitor->destroyed);
    pv.monitor("pva://alarm", [&](Event e) { a = e; }, &first);
    provider->channels.back()->monitor->post(4, 2);
    QTRY_COMPARE(a.severity, 2);
    QCOMPARE(b.severity, 1);
  }
  void sharedMonitorDeliversNewAndReplacedCallbacks() {
    PvAccess pv;
    int first = 0, second = 0, oldCalls = 0, newCalls = 0, firstCalls = 0;
    pv.monitor("pva://alarm", [&](Event) { ++firstCalls; }, &first);
    auto channel = provider->channels.back();
    channel->monitor->post(1);
    QTRY_COMPARE(firstCalls, 1);
    pv.monitor("pva://alarm", [&](Event) { ++oldCalls; }, &second);
    QTRY_COMPARE(oldCalls, 1); // Initial state arrives even without a new server event.
    QCOMPARE(provider->channels.size(), size_t(1));
    pv.monitor("pva://alarm", [&](Event) { ++newCalls; }, &second);
    channel->monitor->post(2);
    QTRY_COMPARE(newCalls, 1);
    QCOMPARE(oldCalls, 1);
    QCOMPARE(firstCalls, 2);
    pv.cancel("pva://alarm", &second);
    channel->monitor->post(3);
    QTRY_COMPARE(firstCalls, 3);
    QCOMPARE(newCalls, 1);
    pv.monitor("pva://alarm", [&](Event e) {
      QCOMPARE(e.value, QString("3"));
      ++newCalls;
    }, &second);
    QTRY_COMPARE(newCalls, 2);
  }
  void rejectedWriteSetupCanBeRetried() {
    PvAccess pv;
    int errors = 0;
    pv.error = [&](const QString&) { ++errors; };
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto refused = channel->writer;
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    refused->refuse();
    QTRY_COMPARE(errors, 1);
    QVERIFY(pv.put("pva://output", 2));
    QVERIFY(channel->writer != refused);
    channel->writer->ready();
    QTRY_COMPARE(channel->writer->submitted, QVector<double>{2});
  }
  void repeatedWriteRejectionReportedOncePerOutage() {
    // PVA has no access-rights events, so heartbeats keep writing to a PV whose
    // server rejects each put. Report one diagnostic per outage, not per beat.
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto writer = channel->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    const auto beat = [&](bool accept) {
      const int sent = writer->submitted.size();
      QVERIFY(pv.put("pva://output", 1, WriteKind::Heartbeat));
      QTRY_COMPARE(writer->submitted.size(), sent + 1);
      if (accept) writer->done(); else writer->reject();
      QCoreApplication::processEvents();
    };
    for (int i = 0; i < 5; ++i) beat(false);
    const QString rejected = "Write pva://output: Put not permitted";
    QCOMPARE(errors, QStringList{rejected});
    // A successful write ends the outage.
    beat(true);
    for (int i = 0; i < 3; ++i) beat(false);
    QCOMPARE(errors, (QStringList{rejected, rejected}));
    // So does a reconnect.
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QTRY_VERIFY(channel->writer != writer);
    writer = channel->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    for (int i = 0; i < 3; ++i) beat(false);
    QCOMPARE(errors, (QStringList{rejected, rejected, rejected}));
    // A rejected put setup is torn down and recreated by the next beat.
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QTRY_VERIFY(channel->writer != writer);
    errors.clear();
    for (int i = 0; i < 3; ++i) {
      auto refused = channel->writer;
      QVERIFY(pv.put("pva://output", 1, WriteKind::Heartbeat));
      refused->refuse();
      QTRY_VERIFY(refused->destroyed);
      QCOMPARE(pv.pendingWriteCount("pva://output"), size_t(0));
      QVERIFY(pv.put("pva://output", 1, WriteKind::Heartbeat));
      QVERIFY(channel->writer != refused);
    }
    QCOMPARE(errors, QStringList{"Write pva://output: write refused"});
  }
  void boundedInboxPreservesOrder() {
    PvAccess pv;
    QVector<int> values;
    pv.monitor("pva://alarm", [&](Event e) { values << e.value.toInt(); });
    auto monitor = provider->channels.back()->monitor;
    for (int i = 0; i < 1000; ++i) monitor->post(i, i % 3);
    QVERIFY(pv.pendingUpdateCount() <= PvAccess::MaximumPendingUpdates);
    QVERIFY(pv.pendingUpdateBytes() <= PvAccess::MaximumPendingUpdateBytes);
    QTRY_COMPARE(values.size(), 1000);
    for (int i = 0; i < values.size(); ++i) QCOMPARE(values[i], i);
    QCOMPARE(pv.pendingUpdateCount(), size_t(0));
    QCOMPARE(pv.pendingUpdateBytes(), size_t(0));
  }
  void inboxOverflow_data() {
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("messageBytes");
    QTest::newRow("count") << int(PvAccess::MaximumPendingUpdates + 10) << 0;
    QTest::newRow("bytes") << 1 << int(PvAccess::MaximumPendingUpdateBytes / 2 + 100);
  }
  void inboxOverflow() {
    QFETCH(int, count); QFETCH(int, messageBytes);
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    Event last;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    auto old = provider->channels.back()->monitor;
    const std::string message(messageBytes, 'x');
    for (int i = 0; i < count; ++i) old->post(i, 2, message);
    QVERIFY(pv.pendingUpdateCount() <= PvAccess::MaximumPendingUpdates);
    QVERIFY(pv.pendingUpdateBytes() <= PvAccess::MaximumPendingUpdateBytes);
    QTRY_VERIFY(errors.join(' ').contains("observations lost"));
    QCOMPARE(last.severity, 4);
    QVERIFY(old->destroyed);
    QTRY_VERIFY(provider->channels.back()->monitor != old);
    auto fresh = provider->channels.back()->monitor;
    QVERIFY(fresh != old);
    old->post(999, 3); // Discard callbacks from the abandoned subscription.
    fresh->post(7, 1);
    QTRY_COMPARE(last.value, QString("7"));
    QCOMPARE(last.severity, 1);
  }
  void largeConfigurationRecoversFromInboxOverflow() {
    PvAccess pv;
    int overflows = 0, delivered = 0;
    pv.error = [&](const QString& s) { if (s.contains("buffer overflow")) ++overflows; };
    const int count = int(PvAccess::MaximumPendingUpdates) + 100;
    for (int i = 0; i < count; ++i)
      pv.number("pva://input" + QString::number(i), [&](double v) {
        if (std::isfinite(v)) ++delivered;
      });
    QTRY_COMPARE(provider->channels.size(), size_t(2 * count));
    QCOMPARE(overflows, 1);
    provider->channels.back()->monitor->post(7);
    QTRY_COMPARE(delivered, 1);
    QVERIFY(pv.pendingUpdateBytes() <= PvAccess::MaximumPendingUpdateBytes);
  }
  void boundedOrderedWrites() {
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto writer = provider->channels.back()->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 0, WriteKind::AckValue));
    for (size_t i = 1; i <= PvAccess::MaximumPendingWrites; ++i)
      QVERIFY(pv.put("pva://output", double(i), WriteKind::AckValue));
    QVERIFY(!pv.put("pva://output", 999, WriteKind::AckValue));
    QCOMPARE(pv.pendingWriteCount("pva://output"), PvAccess::MaximumPendingWrites);
    QVERIFY(errors.join(' ').contains("limit reached"));
    for (size_t i = 1; i <= PvAccess::MaximumPendingWrites; ++i) {
      writer->done();
      QTRY_COMPARE(writer->submitted.size(), int(i + 1));
      QCOMPARE(writer->submitted.back(), double(i));
    }
    QCOMPARE(pv.pendingWriteCount("pva://output"), size_t(0));
  }
  void latestStateWrites_data() {
    QTest::addColumn<bool>("heartbeat");
    QTest::newRow("severity") << false;
    QTest::newRow("heartbeat") << true;
  }
  void latestStateWrites() {
    QFETCH(bool, heartbeat);
    PvAccess pv;
    pv.prepare("pva://output");
    auto writer = provider->channels.back()->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 0));
    for (int i = 1; i <= 1000; ++i)
      QVERIFY(pv.put("pva://output", i, heartbeat ? WriteKind::Heartbeat : WriteKind::Severity));
    QCOMPARE(pv.pendingWriteCount("pva://output"), size_t(1));
    writer->done();
    QTRY_COMPARE(writer->submitted, (QVector<double>{0, 1000}));
  }
  void writeTimeout_data() {
    QTest::addColumn<bool>("ready");
    QTest::newRow("setup") << false;
    QTest::newRow("completion") << true;
  }
  void writeTimeout() {
    QFETCH(bool, ready);
    PvAccess pv;
    pv.writeTimeoutMs = 30;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto old = channel->writer;
    if (ready) old->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    QVERIFY(pv.put("pva://output", 2));
    QTRY_VERIFY(errors.join(' ').contains("timed out"));
    QVERIFY(old->destroyed);
    QCOMPARE(pv.pendingWriteCount("pva://output"), size_t(0));
    QVERIFY(pv.put("pva://output", 3));
    auto fresh = channel->writer;
    QVERIFY(fresh != old);
    fresh->ready();
    QTRY_COMPARE(fresh->submitted, QVector<double>{3});
    QVERIFY(pv.put("pva://output", 4));
    old->ready(); old->done();
    QCoreApplication::processEvents();
    QCOMPARE(fresh->submitted.size(), 1);
    fresh->done();
    QTRY_COMPARE(fresh->submitted, (QVector<double>{3, 4}));
  }
  void failedSeverityWriteRetriesOnReconnect_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("disconnect before ready") << "disconnect";
    QTest::newRow("refused setup") << "refused";
    QTest::newRow("setup timeout") << "setup timeout";
    QTest::newRow("rejected completion") << "rejected";
    QTest::newRow("disconnect in flight") << "in flight";
    QTest::newRow("completion timeout") << "completion timeout";
  }
  void failedSeverityWriteRetriesOnReconnect() {
    // Failed severity-output writes retry when the PV connects, as Channel
    // Access does (docs/qtalh-compatibility.md).
    QFETCH(QString, failure);
    PvAccess pv;
    if (failure.contains("timeout")) pv.writeTimeoutMs = 30;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto old = channel->writer;
    const bool sent = failure == "rejected" || failure == "in flight" || failure == "completion timeout";
    if (sent) old->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 2, WriteKind::Severity));
    QCOMPARE(old->submitted, sent ? QVector<double>{2} : QVector<double>{});
    if (failure == "refused") {
      old->refuse();
      QTRY_VERIFY(errors.join(' ').contains("write refused"));
    } else if (failure == "rejected") {
      old->reject();
      QTRY_VERIFY(errors.join(' ').contains("Put not permitted"));
    } else if (failure.contains("timeout")) {
      QTRY_VERIFY(errors.join(' ').contains("timed out"));
      pv.writeTimeoutMs = 30000;
    }
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QCoreApplication::processEvents();
    auto recovered = channel->writer;
    QVERIFY(recovered != old);
    recovered->ready();
    QTRY_COMPARE(recovered->submitted, QVector<double>{2});
    // A confirmed severity write is no longer pending.
    recovered->done();
    QCoreApplication::processEvents();
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QCoreApplication::processEvents();
    QVERIFY(channel->writer != recovered);
    channel->writer->ready();
    QCoreApplication::processEvents();
    QCOMPARE(channel->writer->submitted, QVector<double>{});
  }
  void olderSeverityCompletionKeepsNewerValuePending() {
    PvAccess pv;
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    auto writer = channel->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1, WriteKind::Severity));
    QVERIFY(pv.put("pva://output", 2, WriteKind::Severity));
    writer->done(); // Confirms 1, not the newer queued 2.
    QTRY_COMPARE(writer->submitted, (QVector<double>{1, 2}));
    channel->state(pva::Channel::DISCONNECTED);
    channel->state(pva::Channel::CONNECTED);
    QCoreApplication::processEvents();
    auto recovered = channel->writer;
    QVERIFY(recovered != writer);
    recovered->ready();
    QTRY_COMPARE(recovered->submitted, QVector<double>{2});
  }
  void idleOutputDoesNotReportWriteTimeout_data() {
    QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("online");
    QTest::newRow("active, never connected") << false << false;
    QTest::newRow("active, setup unanswered") << false << true;
    QTest::newRow("passive, never connected") << true << false;
    QTest::newRow("passive, connected") << true << true;
  }
  void idleOutputDoesNotReportWriteTimeout() {
    // SEVRPV/ACKPV/HEARTBEATPV outputs are prepared in every mode. With no
    // write submitted, a missing IOC must not produce a cancelled-write report.
    QFETCH(bool, passive); QFETCH(bool, online);
    provider->connectOnCreate = online;
    PvAccess pv({true, passive});
    pv.writeTimeoutMs = 30;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto channel = provider->channels.back();
    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
    if (!online) channel->state(pva::Channel::CONNECTED);
    QCoreApplication::processEvents();
    if (passive) {
      QVERIFY(!channel->writer); // Passive mode opens no put operation.
      QVERIFY(!pv.put("pva://output", 1));
      QVERIFY(!channel->writer);
      QVERIFY(errors.isEmpty());
      return;
    }
    QVERIFY(channel->writer);
    channel->writer->ready();
    QCoreApplication::processEvents();
    QVERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    QCOMPARE(channel->writer->submitted, QVector<double>{1});
    channel->writer->done();
    QCoreApplication::processEvents();
    QTest::qWait(100);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
  }
};
QTEST_GUILESS_MAIN(PvaTests)
#include "test_pva.moc"
