// Exercise PVA callback ordering and failures without a network server.
#include "services/pv_access.h"
#include "core/model.h"
#include "core/engine.h"
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
#include <alarm.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
namespace pva = epics::pvAccess;
namespace pvd = epics::pvData;
using namespace alh;
namespace {
// A "value" field followed by the alarm fields of an NTScalar.
pvd::StructureConstPtr withAlarm(pvd::FieldBuilderPtr value, bool display = false) {
  value = value->addNestedStructure("alarm")
      ->add("severity", pvd::pvInt)->add("status", pvd::pvInt)
      ->add("message", pvd::pvString)->endNested();
  if (display)
    value = value->addNestedStructure("display")->add("precision", pvd::pvInt)->endNested();
  return value->createStructure();
}
pvd::StructureConstPtr scalarType(pvd::ScalarType type = pvd::pvDouble, bool display = false) {
  return withAlarm(pvd::getFieldCreate()->createFieldBuilder()->add("value", type), display);
}
pvd::StructureConstPtr arrayType(pvd::ScalarType type) {
  return withAlarm(pvd::getFieldCreate()->createFieldBuilder()->addArray("value", type));
}
pvd::StructureConstPtr enumType() {
  return withAlarm(pvd::getFieldCreate()->createFieldBuilder()
      ->addNestedStructure("value")->add("index", pvd::pvInt)->addArray("choices", pvd::pvString)
      ->endNested());
}
pvd::PVStructurePtr create(const pvd::StructureConstPtr& type) {
  return pvd::getPVDataCreate()->createPVStructure(type);
}
template <typename Array, typename T>
void putArray(const pvd::PVStructurePtr& data, const char* field, std::initializer_list<T> values) {
  typename Array::svector items;
  for (const auto& value : values) items.push_back(value);
  data->getSubFieldT<Array>(field)->replace(pvd::freeze(items));
}
struct TestMonitor final : pva::Monitor, std::enable_shared_from_this<TestMonitor> {
  pva::MonitorRequester::shared_pointer requester;
  std::deque<pva::MonitorElementPtr> queue;
  pvd::Status startResult;
  std::atomic<bool> destroyed{false}; // Set on the Qt thread; read by posting threads.
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
  // Marks the named fields as changed, or the whole structure (bit 0).
  void postData(const pvd::PVStructurePtr& data, std::initializer_list<const char*> changed = {}) {
    auto element = std::make_shared<pva::MonitorElement>(data);
    if (!changed.size()) element->changedBitSet->set(0);
    for (auto field : changed)
      element->changedBitSet->set(static_cast<pvd::uint32>(data->getSubFieldT(field)->getFieldOffset()));
    queue.push_back(element);
    requester->monitorEvent(shared_from_this());
  }
  static pvd::PVStructurePtr scalar(double value, int severity = 0, const std::string& message = {},
                                    int status = 0) {
    auto data = create(scalarType());
    data->getSubField<pvd::PVDouble>("value")->put(value);
    data->getSubField<pvd::PVInt>("alarm.severity")->put(severity);
    data->getSubField<pvd::PVInt>("alarm.status")->put(status);
    data->getSubField<pvd::PVString>("alarm.message")->put(message);
    return data;
  }
  void post(double value, int severity = 0, const std::string& message = {}, int status = 0) {
    postData(scalar(value, severity, message, status));
  }
  void postText(const std::string& value, int severity = 0) {
    auto data = create(scalarType(pvd::pvString));
    data->getSubField<pvd::PVString>("value")->put(value);
    data->getSubField<pvd::PVInt>("alarm.severity")->put(severity);
    postData(data);
  }
};
struct TestPut final : pva::ChannelPut, std::enable_shared_from_this<TestPut> {
  pva::ChannelPutRequester::shared_pointer requester;
  std::weak_ptr<pva::Channel> channel;
  QVector<double> submitted;
  pvd::PVStructurePtr lastData;
  pvd::BitSetPtr lastBits;
  bool destroyed = false;
  pva::Channel::shared_pointer getChannel() override { return channel.lock(); }
  void lock() override {}
  void unlock() override {}
  void cancel() override {}
  void lastRequest() override {}
  void destroy() override { destroyed = true; }
  void get() override {}
  void put(const pvd::PVStructurePtr& data, const pvd::BitSetPtr& bits) override {
    lastData = data;
    lastBits = bits;
    if (auto value = data->getSubField<pvd::PVDouble>("value")) submitted << value->get();
  }
  void ready(pvd::StructureConstPtr type = scalarType()) {
    requester->channelPutConnect(pvd::Status::Ok, shared_from_this(), type);
  }
  void refuse() { requester->channelPutConnect(pvd::Status::error("write refused"), shared_from_this(), {}); }
  void done() { requester->putDone(pvd::Status::Ok, shared_from_this()); }
  // QSRV rejects a put denied by access security when it completes.
  void reject() { requester->putDone(pvd::Status::error("Put not permitted"), shared_from_this()); }
  void rejectWithoutReason() { requester->putDone(pvd::Status::error(""), shared_from_this()); }
};
struct TestGet final : pva::ChannelGet, std::enable_shared_from_this<TestGet> {
  pva::ChannelGetRequester::shared_pointer requester;
  std::weak_ptr<pva::Channel> channel;
  int requests = 0;
  pva::Channel::shared_pointer getChannel() override { return channel.lock(); }
  void cancel() override {}
  void lastRequest() override {}
  void destroy() override {}
  void get() override { ++requests; }
  void ready() { requester->channelGetConnect(pvd::Status::Ok, shared_from_this(), scalarType()); }
  void answer(const pvd::PVStructurePtr& data) {
    requester->getDone(pvd::Status::Ok, shared_from_this(), data, std::make_shared<pvd::BitSet>());
  }
};
struct TestChannel final : pva::Channel, std::enable_shared_from_this<TestChannel> {
  pva::ChannelProvider::shared_pointer provider;
  pva::ChannelRequester::shared_pointer requester;
  std::string name;
  std::shared_ptr<TestMonitor> monitor;
  std::shared_ptr<TestPut> writer;
  std::shared_ptr<TestGet> reader;
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
  pva::ChannelGet::shared_pointer createChannelGet(const pva::ChannelGetRequester::shared_pointer& callback,
                                                  const pvd::PVStructurePtr&) override {
    reader = std::make_shared<TestGet>();
    reader->requester = callback;
    reader->channel = shared_from_this();
    return reader;
  }
  void state(ConnectionState state) { requester->channelStateChange(shared_from_this(), state); }
};
struct TestProvider final : pva::ChannelProvider, std::enable_shared_from_this<TestProvider> {
  std::vector<std::shared_ptr<TestChannel>> channels;
  bool connectOnCreate = true; // false: a server that never answers the search.
  std::string refuse; // createChannel returns no channel for this name.
  std::shared_ptr<TestChannel> channel(const std::string& name) const {
    for (auto it = channels.rbegin(); it != channels.rend(); ++it)
      if ((*it)->name == name) return *it;
    return {};
  }
  std::string getProviderName() override { return "pva"; }
  void destroy() override {}
  pva::ChannelFind::shared_pointer channelFind(const std::string&,
                                             const pva::ChannelFindRequester::shared_pointer&) override { return {}; }
  pva::Channel::shared_pointer createChannel(const std::string& name,
      const pva::ChannelRequester::shared_pointer& requester, short, const std::string&) override {
    if (name == refuse)
      return {};
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
// Server values of the types extract() and sendPut() convert.
pvd::PVStructurePtr valueOf(const QString& kind) {
  pvd::PVStructurePtr data;
  if (kind == "precision") {
    data = create(scalarType(pvd::pvDouble, true));
    data->getSubField<pvd::PVDouble>("value")->put(3.14159);
    data->getSubField<pvd::PVInt>("display.precision")->put(2);
  } else if (kind == "double") {
    data = create(scalarType());
    data->getSubField<pvd::PVDouble>("value")->put(2.5);
  } else if (kind == "int") {
    data = create(scalarType(pvd::pvInt));
    data->getSubField<pvd::PVInt>("value")->put(-7);
  } else if (kind == "boolean") {
    data = create(scalarType(pvd::pvBoolean));
    data->getSubField<pvd::PVBoolean>("value")->put(true);
  } else if (kind.endsWith("string")) {
    data = create(scalarType(pvd::pvString));
    data->getSubField<pvd::PVString>("value")->put(kind == "numeric string" ? " 42 " : "OPEN");
  } else if (kind == "double array") {
    data = create(arrayType(pvd::pvDouble));
    putArray<pvd::PVDoubleArray>(data, "value", {4.5, 9.0});
  } else if (kind == "string array") {
    data = create(arrayType(pvd::pvString));
    putArray<pvd::PVStringArray>(data, "value", {std::string("7"), std::string("x")});
  } else if (kind == "empty array") {
    data = create(arrayType(pvd::pvDouble));
  } else {
    data = create(enumType());
    data->getSubField<pvd::PVInt>("value.index")->put(kind == "enum" ? 1 : 5);
    putArray<pvd::PVStringArray>(data, "value.choices", {std::string("Off"), std::string("On")});
  }
  return data;
}
QString writtenText(const pvd::PVStructurePtr& data) {
  if (auto index = data->getSubField<pvd::PVInt>("value.index"))
    return QString("index=%1").arg(index->get());
  if (auto array = data->getSubField<pvd::PVScalarArray>("value")) {
    pvd::shared_vector<const std::string> items;
    array->getAs(items);
    QStringList parts;
    for (const auto& item : items) parts << QString::fromStdString(item);
    return "[" + parts.join(',') + "]";
  }
  return QString::fromStdString(data->getSubFieldT<pvd::PVScalar>("value")->getAs<std::string>());
}
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
  void callbackCanDestroyService_data() {
    QTest::addColumn<QString>("mode");
    for (const auto mode : {"alarm", "numeric", "text", "disconnect", "monitor failure",
                            "numeric diagnostic", "write rejection", "write timeout",
                            "write conversion", "overflow diagnostic", "overflow read",
                            "creation diagnostic", "connection deadline"})
      QTest::newRow(mode) << QString(mode);
  }
  void callbackCanDestroyService() {
    QFETCH(QString, mode);
    // Replacing a configuration can release the adapter from a notification.
    // Both the recipient loop and the inbox batch must stop using it then.
    auto pv = std::make_unique<PvAccess>();
    int calls = 0, first = 0, second = 0;
    auto destroy = [&] { ++calls; pv.reset(); };
    auto release = [&](Event) { ++calls; pv.reset(); };
    std::shared_ptr<TestMonitor> monitor;
    qint64 now = 0;
    if (mode == "creation diagnostic") {
      provider->refuse = "alarm";
      pv->error = [&](const QString&) { destroy(); };
      pv->monitor("pva://alarm", release);
    } else if (mode.startsWith("write")) {
      pv->error = [&](const QString&) { destroy(); };
      pv->writeTimeoutMs = 1;
      pv->prepare("pva://output");
      auto writer = provider->channels.back()->writer;
      writer->ready(mode == "write conversion" ? pvd::getFieldCreate()->createStructure()
                                                : scalarType());
      QTRY_VERIFY(pv->canWrite("pva://output"));
      QCoreApplication::processEvents(); // Deliver the queued put setup.
      pv->put("pva://output", 1);
      if (mode == "write rejection") writer->reject();
    } else if (mode.startsWith("overflow")) {
      if (mode == "overflow diagnostic")
        pv->error = [&](const QString&) { destroy(); };
      else
        pv->readStrings({"pva://pending"}, [&](QHash<QString, QString>) { destroy(); }, 60000);
      pv->monitor("pva://alarm", [](Event) {});
      monitor = provider->channels.back()->monitor;
      for (size_t i = 0; i <= PvAccess::MaximumPendingUpdates; ++i) monitor->post(double(i));
    } else if (mode == "numeric" || mode == "numeric diagnostic") {
      if (mode == "numeric diagnostic") pv->error = [&](const QString&) { destroy(); };
      pv->number("pva://input", [&](double) { destroy(); });
      monitor = provider->channels.back()->monitor;
      if (mode == "numeric") monitor->post(1);
      else monitor->postText("not a number");
    } else if (mode == "text") {
      pv->text("pva://text", [&](QString) { destroy(); });
      monitor = provider->channels.back()->monitor;
      monitor->postText("text");
    } else {
      if (mode == "connection deadline") {
        provider->connectOnCreate = false;
        pv->monotonicNow = [&] { return now; };
      }
      pv->monitor("pva://alarm", release, &first);
      pv->monitor("pva://alarm", release, &second);
      auto channel = provider->channels.back();
      monitor = channel->monitor;
      if (mode == "connection deadline") now = 2000;
      else if (mode == "disconnect") channel->state(pva::Channel::DISCONNECTED);
      else if (mode == "monitor failure") monitor->fail();
      else {
        monitor->post(1);
        monitor->post(2); // Work queued behind the destructive callback.
      }
    }
    QTRY_VERIFY(!pv);
    QCOMPARE(calls, 1);
    if (monitor) {
      QVERIFY(monitor->destroyed);
      monitor->post(3); // A server may finish posting after destroy().
    }
    QCoreApplication::processEvents();
    QCOMPARE(calls, 1);
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
    // Under the count limit, but twice the byte budget.
    QTest::newRow("bytes") << int(PvAccess::MaximumPendingUpdates / 2)
                           << int(2 * PvAccess::MaximumPendingUpdateBytes / PvAccess::MaximumPendingUpdates);
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
  void oversizedUpdate_data() {
    QTest::addColumn<bool>("value");
    QTest::newRow("alarm message") << false;
    QTest::newRow("value") << true;
  }
  void oversizedUpdate() {
    QFETCH(bool, value);
    PvAccess pv;
    int overflows = 0;
    pv.error = [&](const QString& s) { if (s.contains("buffer overflow")) ++overflows; };
    Event last;
    pv.monitor("pva://big", [&](Event e) { last = e; });
    const std::string big(PvAccess::MaximumPendingUpdateBytes / 2 + 100, 'x');
    // A server sends its current value to every fresh subscription. One
    // update larger than the whole budget must not start overflow recovery,
    // which would resubscribe and overflow again without end.
    for (int round = 0; round < 3; ++round) {
      auto monitor = provider->channels.back()->monitor;
      if (value) monitor->postText(big, 2);
      else monitor->post(round, 2, big);
      QTest::qWait(250); // Recovery resubscribes after 100 ms.
    }
    QCOMPARE(overflows, 0);
    QCOMPARE(provider->channels.size(), size_t(1));
    QCOMPARE(last.severity, 2);
    if (value) {
      QVERIFY(!last.value.isEmpty() && last.value.size() < int(big.size()));
      QVERIFY(QString::fromStdString(big).startsWith(last.value));
    } else {
      QCOMPARE(last.value, QString("2"));
    }
    QCOMPARE(pv.pendingUpdateBytes(), size_t(0));
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
  void neverConnectedChannelsReportAfterDeadline() {
    // A PV no server answers sends no disconnect: the one-second connection
    // deadline reports it, writes are refused, and a late connection recovers.
    provider->connectOnCreate = false;
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    Event alarm;
    alarm.severity = -1;
    double input = 0;
    pv.monitor("pva://alarm", [&](Event e) { alarm = e; });
    pv.number("pva://input", [&](double v) { input = v; });
    pv.prepare("pva://output");
    QVERIFY(!pv.canWrite("pva://output"));
    QVERIFY(!pv.put("pva://output", 1));
    QVERIFY(errors.join(' ').contains("PV not connected or not writable: pva://output"));
    QTRY_COMPARE_WITH_TIMEOUT(alarm.severity, 4, 3000);
    QCOMPARE(alarm.status, int(ALARM_NSTATUS));
    QTRY_VERIFY(std::isnan(input));
    QVERIFY(errors.join(' ').contains("PV not connected: pva://input"));
    for (const auto& channel : provider->channels) channel->state(pva::Channel::CONNECTED);
    provider->channel("alarm")->monitor->post(1);
    provider->channel("input")->monitor->post(5);
    QTRY_COMPARE(alarm.value, QString("1"));
    QCOMPARE(alarm.severity, 0);
    QTRY_COMPARE(input, 5.0);
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 2));
    auto output = provider->channel("output");
    QVERIFY(output->writer);
    output->writer->ready();
    QTRY_COMPARE(output->writer->submitted, QVector<double>{2});
  }
  void forceInputsShareOneChannel() {
    // Inputs reading one PV share its channel. One added later receives the
    // latest value after the call returns, as a new monitor would.
    PvAccess pv;
    int a = 0, b = 0, c = 0;
    QVector<double> first, second, third;
    pv.number("pva://input", [&](double v) { first << v; }, &a);
    pv.number("pva://input", [&](double v) { second << v; }, &b);
    QCOMPARE(provider->channels.size(), size_t(1));
    auto monitor = provider->channels.back()->monitor;
    monitor->post(5);
    QTRY_COMPARE(second, QVector<double>{5});
    QCOMPARE(first, QVector<double>{5});
    pv.number("pva://input", [&](double v) { third << v; }, &c);
    QVERIFY(third.isEmpty());
    QTRY_COMPARE(third, QVector<double>{5});
    QCOMPARE(provider->channels.size(), size_t(1));
    // Cancelling one input keeps the monitor for the others.
    pv.cancelNumbers(&a);
    monitor->post(6);
    QTRY_COMPARE(third, (QVector<double>{5, 6}));
    QCOMPARE(second, (QVector<double>{5, 6}));
    QCOMPARE(first, QVector<double>{5});
    // An unconnected PV is reported once, however many inputs read it.
    provider->connectOnCreate = false;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    int unavailable = 0;
    pv.number("pva://missing", [&](double v) { unavailable += std::isnan(v); }, &a);
    pv.number("pva://missing", [&](double v) { unavailable += std::isnan(v); }, &b);
    QTRY_COMPARE_WITH_TIMEOUT(unavailable, 2, 3000);
    QCOMPARE(errors.filter("pva://missing").size(), 1);
    QCOMPARE(provider->channels.size(), size_t(2));
  }
  void forceInputIgnoresAlarmOnlyUpdates() {
    // Force/CALC inputs follow value changes in the changed bitset only.
    PvAccess pv;
    QVector<double> values;
    pv.number("pva://input", [&](double v) { values << v; });
    auto monitor = provider->channels.back()->monitor;
    monitor->post(5);
    QTRY_COMPARE(values, QVector<double>{5});
    monitor->postData(TestMonitor::scalar(5, 2, "HIGH", 3), {"alarm"});
    monitor->postData(TestMonitor::scalar(5), {"alarm.severity", "alarm.status", "alarm.message"});
    monitor->postData(TestMonitor::scalar(6), {"value"});
    QTRY_COMPARE(values, (QVector<double>{5, 6}));
  }
  void valueExtraction_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("text");
    QTest::addColumn<double>("number"); // NaN: not numeric.
    const double none = std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("double with precision") << "precision" << "3.14" << 3.14159;
    QTest::newRow("double") << "double" << "2.5" << 2.5;
    QTest::newRow("int") << "int" << "-7" << -7.0;
    QTest::newRow("boolean") << "boolean" << "1" << 1.0;
    QTest::newRow("numeric string") << "numeric string" << " 42 " << 42.0;
    QTest::newRow("text string") << "text string" << "OPEN" << none;
    QTest::newRow("double array") << "double array" << "4.5" << 4.5;
    QTest::newRow("string array") << "string array" << "7" << 7.0;
    QTest::newRow("empty array") << "empty array" << "" << none;
    QTest::newRow("enum") << "enum" << "On" << 1.0;
    QTest::newRow("enum out of range") << "enum out of range" << "5" << 5.0;
  }
  void valueExtraction() {
    QFETCH(QString, kind); QFETCH(QString, text); QFETCH(double, number);
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    Event alarm;
    int alarms = 0, inputs = 0;
    double input = 0;
    pv.monitor("pva://alarm", [&](Event e) { alarm = e; ++alarms; });
    pv.number("pva://input", [&](double v) { input = v; ++inputs; });
    provider->channel("alarm")->monitor->postData(valueOf(kind));
    provider->channel("input")->monitor->postData(valueOf(kind));
    QTRY_COMPARE(alarms, 1);
    QTRY_COMPARE(inputs, 1);
    QCOMPARE(alarm.value, text);
    if (std::isnan(number)) {
      QVERIFY(std::isnan(input));
      QVERIFY(errors.join(' ').contains("PV value is not numeric: pva://input"));
    } else {
      QCOMPARE(input, number);
      QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
    }
  }
  void writeConversions_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<double>("value");
    QTest::addColumn<QString>("written");
    QTest::newRow("int") << "int" << 3.0 << "3";
    QTest::newRow("string") << "string" << 2.5 << "2.5";
    QTest::newRow("double array") << "double array" << 2.5 << "[2.5]";
    QTest::newRow("enum") << "enum" << 2.0 << "index=2";
  }
  void writeConversions() {
    QFETCH(QString, kind); QFETCH(double, value); QFETCH(QString, written);
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto writer = provider->channels.back()->writer;
    writer->ready(kind == "int" ? scalarType(pvd::pvInt)
                  : kind == "string" ? scalarType(pvd::pvString)
                  : kind == "double array" ? arrayType(pvd::pvDouble) : enumType());
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", value));
    QTRY_VERIFY(writer->lastData);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
    QCOMPARE(writtenText(writer->lastData), written);
    // Only the written field is marked changed.
    const auto field = writer->lastData->getSubFieldT(kind == "enum" ? "value.index" : "value");
    QCOMPARE(writer->lastBits->cardinality(), pvd::uint32(1));
    QVERIFY(writer->lastBits->get(static_cast<pvd::uint32>(field->getFieldOffset())));
  }
  void alarmStatusFromMessage_data() {
    QTest::addColumn<int>("severity");
    QTest::addColumn<int>("pvaStatus");
    QTest::addColumn<QString>("message");
    QTest::addColumn<int>("status");
    // QSRV sends the STAT name; other servers may send free text.
    QTest::newRow("STAT name") << 2 << 3 << "HIHI" << int(HIHI_ALARM);
    QTest::newRow("driver text") << 2 << 2 << "link down" << int(COMM_ALARM);
    QTest::newRow("undefined text") << 3 << 6 << "never processed" << int(UDF_ALARM);
    QTest::newRow("other text") << 1 << 3 << "too warm" << int(SOFT_ALARM);
    QTest::newRow("no alarm text") << 0 << 0 << "all clear" << int(NO_ALARM);
  }
  void alarmStatusFromMessage() {
    QFETCH(int, severity); QFETCH(int, pvaStatus); QFETCH(QString, message); QFETCH(int, status);
    PvAccess pv;
    Event last;
    last.status = -1;
    pv.monitor("pva://alarm", [&](Event e) { last = e; });
    provider->channels.back()->monitor->post(1, severity, message.toStdString(), pvaStatus);
    QTRY_COMPARE(last.status, status);
    QCOMPARE(last.severity, severity);
  }
  void rejectedCompletionIsReportedAndWritesContinue() {
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto writer = provider->channels.back()->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    QVERIFY(pv.put("pva://output", 2));
    writer->reject();
    QTRY_VERIFY(errors.join(' ').contains("Write pva://output: Put not permitted"));
    QTRY_COMPARE(writer->submitted, (QVector<double>{1, 2}));
    writer->done();
    QCoreApplication::processEvents();
    QVERIFY(pv.put("pva://output", 3));
    QCOMPARE(writer->submitted, (QVector<double>{1, 2, 3}));
    QVERIFY(!writer->destroyed);
  }
  void rejectionWithoutReasonIsReported() {
    // A server rejection with an empty message is reported once per outage,
    // like any other rejection, instead of never.
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    auto writer = provider->channels.back()->writer;
    writer->ready();
    QTRY_VERIFY(pv.canWrite("pva://output"));
    QVERIFY(pv.put("pva://output", 1));
    writer->rejectWithoutReason();
    QTRY_COMPARE(errors, QStringList{"Write pva://output: write rejected; no reason given"});
    QVERIFY(pv.put("pva://output", 2));
    writer->rejectWithoutReason();
    QCoreApplication::processEvents();
    QCOMPARE(errors.size(), 1);
    QVERIFY(pv.put("pva://output", 3));
    writer->done();
    QCoreApplication::processEvents();
    QVERIFY(pv.put("pva://output", 4));
    writer->rejectWithoutReason();
    QTRY_COMPARE(errors.size(), 2);
  }
  void readStringsIssuesOneGetPerChannel() {
    PvAccess pv;
    int calls = 0;
    QHash<QString, QString> result;
    const auto done = [&](QHash<QString, QString> values) { ++calls; result = values; };
    pv.readStrings({"pva://text", "pva://number"}, done, 60000);
    auto text = provider->channel("text")->reader, number = provider->channel("number")->reader;
    QVERIFY(text && number);
    text->ready();
    number->ready();
    number->ready(); // A reconnect must not request the field twice.
    QTRY_COMPARE(text->requests, 1);
    QTRY_COMPARE(number->requests, 1);
    auto open = create(scalarType(pvd::pvString));
    open->getSubField<pvd::PVString>("value")->put("  open  ");
    text->answer(open);
    QCoreApplication::processEvents();
    QCOMPARE(calls, 0); // Waits for every channel.
    number->answer(TestMonitor::scalar(2.5));
    number->answer(TestMonitor::scalar(9)); // A duplicate answer is ignored.
    QTRY_COMPARE(calls, 1); // Long before the 60 s timeout.
    QCOMPARE(number->requests, 1);
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.value("pva://text"), QString("open"));
    QCOMPARE(result.value("pva://number"), QString("2.5"));
    // A channel that never answers ends the read at its timeout.
    pv.readStrings({"pva://text", "pva://silent"}, done, 100);
    text = provider->channel("text")->reader;
    text->ready();
    QTRY_COMPARE(text->requests, 1);
    text->answer(open);
    QTRY_COMPARE(calls, 2);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.value("pva://text"), QString("open"));
  }
  void inboxOverflowFinishesReadsInFlight() {
    PvAccess pv;
    int calls = 0;
    pv.readStrings({"pva://text"}, [&](QHash<QString, QString>) { ++calls; }, 60000);
    pv.monitor("pva://alarm", [](Event) {});
    auto monitor = provider->channel("alarm")->monitor;
    for (size_t i = 0; i <= PvAccess::MaximumPendingUpdates; ++i) monitor->post(double(i));
    QTRY_COMPARE(calls, 1); // Lost answers end the read instead of waiting 60 s.
  }
  void destructionDiscardsLateCallbacks() {
    // pvAccess threads may still call back after the service is destroyed,
    // whether or not a drain of the shared inbox was already queued.
    int calls = 0;
    for (const bool queued : {true, false}) {
      auto pv = std::make_unique<PvAccess>();
      pv->monitor("pva://alarm", [&](Event) { ++calls; });
      auto channel = provider->channels.back();
      auto monitor = channel->monitor;
      monitor->post(1);
      QTRY_VERIFY(calls > 0);
      calls = 0;
      if (queued) monitor->post(2);
      pv.reset();
      QVERIFY(monitor->destroyed);
      monitor->post(3);
      channel->state(pva::Channel::DISCONNECTED);
      QTest::qWait(20);
      QCOMPARE(calls, 0);
    }
  }
  void providerReturnsNoChannel() {
    // A provider may report a creation failure by returning no channel.
    provider->refuse = "missing";
    PvAccess pv;
    QStringList errors;
    pv.error = [&](const QString& m) { errors << m; };
    int calls = 0;
    QHash<QString, QString> result{{"stale", "x"}};
    pv.readStrings({"pva://missing", "pva://text"}, [&](QHash<QString, QString> v) {
      ++calls;
      result = v;
    }, 60000);
    auto text = provider->channel("text")->reader;
    QVERIFY(text);
    text->ready();
    QTRY_COMPARE(text->requests, 1);
    auto open = create(scalarType(pvd::pvString));
    open->getSubField<pvd::PVString>("value")->put("open");
    text->answer(open);
    QTRY_COMPARE(calls, 1); // The refused name does not hold the read open.
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.value("pva://text"), QString("open"));
    Event alarm;
    alarm.severity = -1;
    pv.monitor("pva://missing", [&](Event e) { alarm = e; });
    QVERIFY(errors.join(' ').contains(
        "Create pva://missing: pvAccess provider did not create the channel"));
    QTRY_COMPARE_WITH_TIMEOUT(alarm.severity, 4, 3000);
  }
  void unavailableOutputReportedOncePerOutage() {
    // ChannelAccess reports an unavailable output once per outage
    // (F-306). A pva:// output whose server is down must not report on every
    // write attempt; the pending severity must still replay on connection.
    provider->connectOnCreate = false;
    PvAccess pv({true});
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    pv.prepare("pva://output");
    const auto unavailable = [&] {
      return errors.count("PV not connected or not writable: pva://output");
    };
    for (int i = 1; i <= 5; ++i)
      QVERIFY(!pv.put("pva://output", i % 3, WriteKind::Severity));
    QVERIFY(!pv.put("pva://output", 1, WriteKind::Acknowledge));
    QVERIFY(!pv.put("pva://output", 7));
    QCOMPARE(unavailable(), 1);
    // The outage ends on connection; the latest severity is replayed.
    auto channel = provider->channel("output");
    channel->state(pva::Channel::CONNECTED);
    QTRY_VERIFY(channel->writer);
    channel->writer->ready();
    QTRY_COMPARE(channel->writer->submitted, QVector<double>{2});
    channel->writer->done();
    QCoreApplication::processEvents();
    // A second outage is reported once more.
    channel->state(pva::Channel::DISCONNECTED);
    QCoreApplication::processEvents();
    for (int i = 0; i < 4; ++i)
      QVERIFY(!pv.put("pva://output", i % 2, WriteKind::Severity));
    QCOMPARE(unavailable(), 2);
    // clear() (reload/close) forgets the outage, as it forgets pendingSeverity.
    pv.clear();
    pv.prepare("pva://output");
    QVERIFY(!pv.put("pva://output", 1, WriteKind::Severity));
    // A severity written while the new channel's first connection is pending
    // is reported only if it misses the connection deadline.
    QCOMPARE(unavailable(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(unavailable(), 3, 5000);
  }
  void unavailableSeverityOutputEngineOncePerOutage() {
    // At engine level: global active mode writes $SEVRPV on every channel
    // and group severity change. With the pva:// output down, 5 alarm cycles
    // must produce one diagnostic, as the CA test in test_ioc asserts.
    provider->connectOnCreate = false;
    auto d = parseConfig("GROUP NULL root\n$SEVRPV pva://out\n"
                         "CHANNEL root pva://alarm\n$SEVRPV pva://out\n");
    PvAccess pv({true});
    Engine e(d, {true}, &pv);
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    e.start();
    auto n = d.channels()[0];
    auto alarm = provider->channel("alarm");
    QVERIFY(alarm && alarm->monitor);
    alarm->state(pva::Channel::CONNECTED);
    const auto post = [&](int severity) {
      alarm->monitor->post(severity, severity);
      QTRY_COMPARE(e.state(n).severity, severity);
    };
    post(0);
    for (int i = 0; i < 5; ++i) { post(2); post(0); }
    const auto unavailable = [&] { return errors.count("PV not connected or not writable: pva://out"); };
    QTRY_COMPARE_WITH_TIMEOUT(unavailable(), 1, 5000); // At the connection deadline.
    for (int i = 0; i < 3; ++i) { post(2); post(0); }
    QCOMPARE(unavailable(), 1);
  }
  void severityOutputConnectingIsNotReported() {
    // The first $SEVRPV writes after a load can precede the output's
    // connection. Like ALH, report nothing then: the value is replayed when
    // the output connects.
    provider->connectOnCreate = false;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pva://alarm\n$SEVRPV pva://out\n");
    PvAccess pv({true});
    Engine e(d, {true}, &pv);
    QStringList errors;
    pv.error = [&](const QString& s) { errors << s; };
    e.start();
    auto n = d.channels()[0];
    auto alarm = provider->channel("alarm"), out = provider->channel("out");
    QVERIFY(alarm && alarm->monitor && out);
    alarm->state(pva::Channel::CONNECTED);
    alarm->monitor->post(2, 2);
    QTRY_COMPARE(e.state(n).severity, 2);
    out->state(pva::Channel::CONNECTED);
    QTRY_VERIFY(out->writer);
    out->writer->ready();
    QTRY_COMPARE(out->writer->submitted, QVector<double>{2});
    QTest::qWait(1200); // Past the connection deadline.
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
  }
  void destructionWhileServerThreadPosts() {
    auto pv = std::make_unique<PvAccess>();
    std::atomic<int> calls{0};
    pv->monitor("pva://alarm", [&](Event) { ++calls; });
    auto monitor = provider->channels.back()->monitor;
    std::atomic<bool> stop{false};
    std::thread server([&] {
      for (int i = 0; !stop; ++i) {
        monitor->post(i % 3, i % 3);
        if (i % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    struct Join { // Also on a failed check's early return.
      std::atomic<bool>& stop;
      std::thread& thread;
      ~Join() { stop = true; if (thread.joinable()) thread.join(); }
    } join{stop, server};
    QTRY_VERIFY(calls > 20);
    pv.reset();
    const int seen = calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stop = true;
    server.join();
    QTest::qWait(20);
    QCOMPARE(int(calls), seen);
    QVERIFY(monitor->destroyed);
  }
};
QTEST_GUILESS_MAIN(PvaTests)
#include "test_pva.moc"
