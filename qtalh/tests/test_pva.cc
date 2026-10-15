// Exercise PVA callback ordering and failures without a network server.
#include "services/pv_access.h"
#include <QtTest>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <pv/clientFactory.h>
#include <pv/pvAccess.h>
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
};
QTEST_GUILESS_MAIN(PvaTests)
#include "test_pva.moc"
