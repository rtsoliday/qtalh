#include "core/diagnostics.h"
// pvAccess callbacks run on pvAccess threads. They only extract plain values
// into an inbox; the Qt thread applies them, matching ChannelAccess's
// non-preemptive delivery.
#include "pv_access.h"
#include <alarm.h>
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
#include <pv/createRequest.h>
#include <pv/pvAccess.h>
#include <pv/pvData.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
namespace pva = epics::pvAccess;
namespace pvd = epics::pvData;
namespace alh {
struct PvAccess::Inbox {
  struct Update {
    enum Kind { Connected, Disconnected, Data, Failed, PutReady, PutDone, GetReady, GetDone } kind;
    quint64 channel = 0, serial = 0;
    bool ok = true;
    QString message;
    Update(Kind k, quint64 c, quint64 s = 0, bool success = true, QString text = {})
        : kind(k), channel(c), serial(s), ok(success), message(std::move(text)) {}
    Sample sample;
    pvd::StructureConstPtr structure;
  };
  std::mutex lock;
  PvAccess* owner = nullptr;
  std::deque<Update> updates;
  size_t bytes = 0;
  bool scheduled = false, overflow = false;
  static size_t cost(const Update& u) {
    return sizeof(Update) + 2 * (size_t(u.message.capacity()) + size_t(u.sample.text.capacity()) +
                                size_t(u.sample.alarmMessage.capacity()));
  }
  void post(Update u) {
    std::lock_guard<std::mutex> guard(lock);
    if (!owner)
      return;
    if (!overflow) {
      const auto size = cost(u);
      if (updates.size() >= MaximumPendingUpdates || size > MaximumPendingUpdateBytes - bytes) {
        // Lost data/control events require a fresh subscription/operation, not
        // silent latest-value coalescing of alarm transitions.
        updates.clear(); bytes = 0; overflow = true;
      } else {
        bytes += size;
        updates.push_back(std::move(u));
      }
    }
    if (!scheduled) {
      scheduled = true;
      QMetaObject::invokeMethod(owner, [o = owner] { o->drain(); }, Qt::QueuedConnection);
    }
  }
};
struct PvAccess::Operations {
  quint64 id = 0;
  QString name;
  pva::Channel::shared_pointer channel;
  pva::ChannelRequester::shared_pointer channelRequester;
  pva::Monitor::shared_pointer monitor;
  pva::MonitorRequester::shared_pointer monitorRequester;
  quint64 monitorSerial = 0;
  pva::ChannelPut::shared_pointer put;
  pva::ChannelPutRequester::shared_pointer putRequester;
  pvd::StructureConstPtr putStructure;
  quint64 putSerial = 0;
  bool putReady = false;
  pva::ChannelGet::shared_pointer get;
  pva::ChannelGetRequester::shared_pointer getRequester;
  bool requested = false, answered = false;
  void stopMonitor() {
    if (monitor)
      monitor->destroy();
    monitor.reset();
    monitorRequester.reset();
    monitorSerial = 0;
  }
  void stopPut() {
    // A completion already queued by the old operation must not release the
    // next connection's in-flight write.
    putSerial = 0;
    putReady = false;
    putStructure.reset();
    if (put) put->destroy();
    put.reset();
    putRequester.reset();
  }
  ~Operations() {
    stopMonitor();
    stopPut();
    if (get) get->destroy();
    if (channel) channel->destroy();
  }
};
namespace {
using Update = PvAccess::Inbox::Update;
const QHash<QString, int>& conditions() {
  static const auto table = [] {
    QHash<QString, int> names;
    for (int i = 0; i < ALARM_NSTATUS; ++i)
      names.insert(statusName(i), i);
    return names;
  }();
  return table;
}
// QSRV reports the record's STAT name in alarm.message; other servers may
// send free text, so fall back on the coarse PVA alarm status.
int condition(int severity, int status, const QString& message) {
  const auto found = conditions().constFind(message);
  if (found != conditions().cend())
    return *found;
  if (!severity)
    return NO_ALARM;
  switch (status) {
  case 2: return COMM_ALARM; // DRIVER
  case 6: return UDF_ALARM;  // UNDEFINED
  default: return SOFT_ALARM;
  }
}
QString formatDouble(double v, int precision) {
  return precision >= 0 ? QString::number(v, 'f', qMin(precision, 17)) : QString::number(v);
}
QString scalarText(const pvd::PVScalar& scalar, int precision, double* number, bool* numeric) {
  switch (scalar.getScalar()->getScalarType()) {
  case pvd::pvString: {
    const auto text = QString::fromStdString(scalar.getAs<std::string>());
    *number = text.trimmed().toDouble(numeric);
    return text;
  }
  case pvd::pvBoolean: {
    const bool value = static_cast<const pvd::PVBoolean&>(scalar).get();
    *number = value;
    *numeric = true;
    return value ? "1" : "0";
  }
  case pvd::pvFloat:
  case pvd::pvDouble:
    *number = scalar.getAs<double>();
    *numeric = true;
    return formatDouble(*number, precision);
  default:
    *number = scalar.getAs<double>();
    *numeric = true;
    return QString::fromStdString(scalar.getAs<std::string>());
  }
}
PvAccess::Sample extract(const pvd::PVStructure& data, const pvd::BitSet* changed) {
  PvAccess::Sample s;
  int pvaStatus = 0;
  QString message;
  if (auto severity = data.getSubField<pvd::PVInt>("alarm.severity"))
    s.severity = qBound(0, int(severity->get()), 3);
  if (auto status = data.getSubField<pvd::PVInt>("alarm.status"))
    pvaStatus = status->get();
  if (auto text = data.getSubField<pvd::PVString>("alarm.message"))
    message = QString::fromStdString(text->get());
  s.alarmStatus = pvaStatus;
  s.alarmMessage = message;
  s.status = condition(s.severity, pvaStatus, message);
  auto precisionField = data.getSubField<pvd::PVInt>("display.precision");
  const int precision = precisionField ? precisionField->get() : -1;
  auto value = data.getSubField("value");
  if (!value)
    return s;
  if (changed)
    s.valueChanged = changed->get(0) ||
        [&] {
          const auto bit = changed->nextSetBit(static_cast<pvd::uint32>(value->getFieldOffset()));
          return bit >= 0 && size_t(bit) < value->getNextFieldOffset();
        }();
  switch (value->getField()->getType()) {
  case pvd::scalar:
    s.text = scalarText(static_cast<const pvd::PVScalar&>(*value), precision, &s.number, &s.numeric);
    break;
  case pvd::scalarArray: {
    // Channel Access element count 1: the first element, as ALH displays.
    auto& array = static_cast<const pvd::PVScalarArray&>(*value);
    if (!array.getLength())
      break;
    if (array.getScalarArray()->getElementType() == pvd::pvString) {
      pvd::shared_vector<const std::string> strings;
      array.getAs(strings);
      s.text = QString::fromStdString(strings[0]);
      s.number = s.text.trimmed().toDouble(&s.numeric);
    } else {
      pvd::shared_vector<const double> numbers;
      array.getAs(numbers);
      s.number = numbers[0];
      s.numeric = true;
      s.text = formatDouble(s.number, precision);
    }
    break;
  }
  case pvd::structure: {
    // NTEnum: value.index with value.choices, as DBR_STRING of an enum.
    auto index = data.getSubField<pvd::PVInt>("value.index");
    if (!index)
      break;
    s.number = index->get();
    s.numeric = true;
    s.text = QString::number(index->get());
    if (auto choices = data.getSubField<pvd::PVStringArray>("value.choices")) {
      const auto view = choices->view();
      if (index->get() >= 0 && size_t(index->get()) < view.size())
        s.text = QString::fromStdString(view[index->get()]);
    }
    break;
  }
  default:
    break;
  }
  return s;
}
struct ChannelRequester final : pva::ChannelRequester {
  std::shared_ptr<PvAccess::Inbox> inbox;
  quint64 id;
  ChannelRequester(std::shared_ptr<PvAccess::Inbox> i, quint64 c) : inbox(std::move(i)), id(c) {}
  std::string getRequesterName() override { return "qtalh"; }
  void message(std::string const&, pvd::MessageType) override {}
  void channelCreated(const pvd::Status&, pva::Channel::shared_pointer const&) override {}
  void channelStateChange(pva::Channel::shared_pointer const&,
                          pva::Channel::ConnectionState state) override {
    if (state == pva::Channel::CONNECTED)
      inbox->post({Update::Connected, id});
    else if (state == pva::Channel::DISCONNECTED)
      inbox->post({Update::Disconnected, id});
  }
};
struct MonitorRequester final : pva::MonitorRequester {
  std::shared_ptr<PvAccess::Inbox> inbox;
  quint64 id, serial;
  MonitorRequester(std::shared_ptr<PvAccess::Inbox> i, quint64 c, quint64 s)
      : inbox(std::move(i)), id(c), serial(s) {}
  std::string getRequesterName() override { return "qtalh"; }
  void message(std::string const&, pvd::MessageType) override {}
  void monitorConnect(pvd::Status const& status, pva::MonitorPtr const& monitor,
                      pvd::StructureConstPtr const&) override {
    auto result = status;
    try {
      if (result.isSuccess() && monitor)
        result = monitor->start();
    } catch (const std::exception& e) {
      inbox->post({Update::Failed, id, serial, false, QString::fromLocal8Bit(e.what())});
      return;
    }
    if (!result.isSuccess() || !monitor)
      inbox->post({Update::Failed, id, serial, false, QString::fromStdString(result.getMessage())});
  }
  void monitorEvent(pva::MonitorPtr const& monitor) override {
    while (auto element = monitor->poll()) {
      Update u{Update::Data, id, serial};
      try {
        u.sample = extract(*element->pvStructurePtr, element->changedBitSet.get());
      } catch (const std::exception& e) {
        u = {Update::Failed, id, serial, false, QString::fromLocal8Bit(e.what())};
      }
      monitor->release(element);
      inbox->post(std::move(u));
    }
  }
  void unlisten(pva::MonitorPtr const&) override {
    inbox->post({Update::Failed, id, serial, false, "subscription ended"});
  }
};
struct PutRequester final : pva::ChannelPutRequester {
  std::shared_ptr<PvAccess::Inbox> inbox;
  quint64 id, serial;
  PutRequester(std::shared_ptr<PvAccess::Inbox> i, quint64 c, quint64 s)
      : inbox(std::move(i)), id(c), serial(s) {}
  std::string getRequesterName() override { return "qtalh"; }
  void message(std::string const&, pvd::MessageType) override {}
  void channelPutConnect(const pvd::Status& status, pva::ChannelPut::shared_pointer const&,
                         pvd::Structure::const_shared_pointer const& structure) override {
    Update u{Update::PutReady, id, serial, status.isOK() && structure,
             QString::fromStdString(status.getMessage())};
    u.structure = structure;
    inbox->post(std::move(u));
  }
  void putDone(const pvd::Status& status, pva::ChannelPut::shared_pointer const&) override {
    inbox->post({Update::PutDone, id, serial, status.isOK(), QString::fromStdString(status.getMessage())});
  }
  void getDone(const pvd::Status&, pva::ChannelPut::shared_pointer const&,
               pvd::PVStructure::shared_pointer const&, pvd::BitSet::shared_pointer const&) override {}
};
struct GetRequester final : pva::ChannelGetRequester {
  std::shared_ptr<PvAccess::Inbox> inbox;
  quint64 id;
  GetRequester(std::shared_ptr<PvAccess::Inbox> i, quint64 c) : inbox(std::move(i)), id(c) {}
  std::string getRequesterName() override { return "qtalh"; }
  void message(std::string const&, pvd::MessageType) override {}
  void channelGetConnect(const pvd::Status& status, pva::ChannelGet::shared_pointer const&,
                         pvd::Structure::const_shared_pointer const&) override {
    if (status.isOK())
      inbox->post({Update::GetReady, id});
  }
  void getDone(const pvd::Status& status, pva::ChannelGet::shared_pointer const&,
               pvd::PVStructure::shared_pointer const& data,
               pvd::BitSet::shared_pointer const&) override {
    Update u{Update::GetDone, id, 0, status.isOK() && data};
    if (u.ok) {
      try {
        u.sample = extract(*data, nullptr);
      } catch (const std::exception&) {
        u.ok = false;
      }
    }
    inbox->post(std::move(u));
  }
};
pvd::PVStructure::shared_pointer request(const char* text) {
  return pvd::CreateRequest::create()->createRequest(text);
}
} // namespace

PvAccess::PvAccess(EngineOptions o, QObject* parent)
    : QObject(parent), options(o), inbox(std::make_shared<Inbox>()) {
  inbox->owner = this;
  deadlineTimer.setSingleShot(true);
  connect(&deadlineTimer, &QTimer::timeout, this, [this] { checkDeadlines(); });
}
PvAccess::~PvAccess() {
  {
    std::lock_guard<std::mutex> guard(inbox->lock);
    inbox->owner = nullptr;
    inbox->updates.clear();
  }
  clear();
  provider.reset();
}
size_t PvAccess::pendingUpdateCount() const {
  std::lock_guard<std::mutex> guard(inbox->lock);
  return inbox->updates.size();
}
size_t PvAccess::pendingUpdateBytes() const {
  std::lock_guard<std::mutex> guard(inbox->lock);
  return inbox->bytes;
}
size_t PvAccess::pendingWriteCount(const QString& name) const {
  size_t n = 0;
  for (auto c : channelsByName.value(name)) n += c->puts.size();
  return n;
}
void PvAccess::report(const QString& message) {
  if (error)
    error(message);
}
bool PvAccess::connectProvider() {
  if (provider)
    return true;
  try {
    pva::ClientFactory::start();
    provider = pva::ChannelProviderRegistry::clients()->getProvider("pva");
  } catch (const std::exception& e) {
    report(QString("pvAccess client: ") + e.what());
  }
  if (!provider)
    report("pvAccess client provider is not available");
  return bool(provider);
}
void PvAccess::writeFailed(Channel* c, const QString& reason) {
  // PVA has no access-rights events: a server that rejects every put would
  // otherwise produce one diagnostic per heartbeat or severity write.
  if (c->writeError != reason) {
    c->writeError = reason;
    report("Write " + c->name + ": " + reason);
  }
}
void PvAccess::numericUnavailable(Channel* c, const QString& reason) {
  if (!c->numeric)
    return;
  const auto id = c->id;
  if (c->numericError != reason) {
    c->numericError = reason;
    report(reason + ": " + c->name);
  }
  // A diagnostic can clear the service or remove this Force/CALC input.
  c = channelsById.value(id);
  if (c && !c->cancelled && c->numeric) {
    const auto callback = c->numeric;
    callback(std::numeric_limits<double>::quiet_NaN());
  }
}
PvAccess::Channel* PvAccess::add(const QString& name) {
  debugLog(options.debug, "PVA", "create " + name);
  auto p = std::make_unique<Channel>();
  auto c = p.get();
  c->id = ++nextId;
  const auto id = c->id;
  c->name = name;
  c->connectionDeadline = monotonicNow() + 1000;
  c->ops = std::make_shared<Operations>();
  c->ops->id = c->id;
  c->ops->name = name;
  channels.push_back(std::move(p));
  channelsById.insert(c->id, c);
  channelsByName[name].push_back(c);
  noteDeadline(c);
  createChannel(c);
  return channelsById.value(id);
}
void PvAccess::createChannel(Channel* c) {
  const auto id = c->id;
  const auto name = c->name;
  if (connectProvider() && (c = channelsById.value(id))) {
    try {
      c->ops->channelRequester = std::make_shared<ChannelRequester>(inbox, c->id);
      c->ops->channel = provider->createChannel(name.mid(6).toStdString(), c->ops->channelRequester,
                                                pva::ChannelProvider::PRIORITY_DEFAULT);
    } catch (const std::exception& e) {
      report("Create " + name + ": " + e.what());
    }
  }
}
void PvAccess::subscribe(Channel* c) {
  auto& ops = *c->ops;
  if (c->cancelled || !ops.channel || ops.monitor || (!c->alarm && !c->numeric && !c->text))
    return;
  // Alarm monitors follow value and alarm changes; force inputs use value
  // changes only (see sample()), as Channel Access DBE_VALUE.
  try {
    ops.monitorSerial = ++nextId;
    ops.monitorRequester = std::make_shared<MonitorRequester>(inbox, c->id, ops.monitorSerial);
    ops.monitor = ops.channel->createMonitor(
        ops.monitorRequester, request(c->alarm ? "field(value,alarm,display.precision)" : "field(value)"));
  } catch (const std::exception& e) {
    inbox->post({Update::Failed, c->id, ops.monitorSerial, false, QString::fromLocal8Bit(e.what())});
  }
}
void PvAccess::ensurePut(Channel* c) {
  auto& ops = *c->ops;
  // Passive mode never writes. The setup deadline starts with a queued write
  // (see put()): an idle output, or one whose IOC is down, has none pending.
  if (options.passive || !ops.channel || ops.put)
    return;
  try {
    ops.putSerial = ++nextId;
    ops.putRequester = std::make_shared<PutRequester>(inbox, c->id, ops.putSerial);
    ops.put = ops.channel->createChannelPut(ops.putRequester, request("field(value)"));
  } catch (const std::exception& e) {
    writeFailed(c, e.what());
  }
}
void PvAccess::sendPut(Channel* c) {
  const auto id = c->id;
  const auto owned = c->ops;
  auto& ops = *owned;
  while (c->connected && !c->putBusy && !c->puts.empty() && ops.putReady && ops.put) {
    const auto write = c->puts.front();
    const double v = write.value;
    c->puts.pop_front();
    c->putOverflow = false;
    try {
      auto data = pvd::getPVDataCreate()->createPVStructure(ops.putStructure);
      pvd::BitSet::shared_pointer bits(new pvd::BitSet);
      auto value = data->getSubField("value");
      if (!value)
        throw std::runtime_error("PV has no value field");
      if (auto scalar = std::dynamic_pointer_cast<pvd::PVScalar>(value)) {
        scalar->putFrom<double>(v);
        bits->set(static_cast<pvd::uint32>(scalar->getFieldOffset()));
      } else if (auto array = std::dynamic_pointer_cast<pvd::PVScalarArray>(value)) {
        pvd::shared_vector<double> one(1, v);
        array->putFrom<double>(pvd::freeze(one));
        bits->set(static_cast<pvd::uint32>(array->getFieldOffset()));
      } else if (auto index = data->getSubField<pvd::PVInt>("value.index")) {
        index->put(static_cast<pvd::int32>(v));
        bits->set(static_cast<pvd::uint32>(index->getFieldOffset()));
      } else {
        throw std::runtime_error("unsupported value type");
      }
      debugLog(options.debug, "PVA", QString("write %1 value=%2").arg(c->name).arg(v, 0, 'g', 17));
      c->putBusy = true;
      c->sending = write;
      c->putDeadline = monotonicNow() + qMax(1, writeTimeoutMs);
      noteDeadline(c);
      ops.put->put(data, bits);
    } catch (const std::exception& e) {
      c->putBusy = false;
      c->putDeadline = 0;
      writeFailed(c, e.what());
      c = channelsById.value(id);
      if (!c) return;
    }
  }
}
void PvAccess::monitor(const QString& n, std::function<void(Event)> callback, const void* tag) {
  Channel* c = nullptr;
  // All observers of one name share an ordered alarm stream. Independent
  // subscription queues could replay an old transition after local ACKS was
  // already cleared, incorrectly raising it again.
  const auto named = channelsByName.constFind(n);
  if (named != channelsByName.cend())
    for (auto channel : *named)
      if (channel->alarm) {
        c = channel;
        break;
      }
  if (!c)
    c = add(n);
  if (!c)
    return;
  const auto id = c->id;
  const auto previous = c->observers.constFind(tag);
  const bool needsInitial = previous == c->observers.cend() || previous->cancelled || !previous->delivered;
  const auto revision = ++nextId;
  c->observers[tag] = {std::move(callback), revision, false, !needsInitial};
  c->cancelled = false;
  c->alarm = [this, id](Event event) { deliverAlarm(id, std::move(event)); };
  if (!c->connected)
    c->connectionDeadline = monotonicNow() + 1000;
  noteDeadline(c);
  // A monitor recreated after Cancel delivers a fresh initial sample.
  subscribe(c);
  if (needsInitial && c->haveObservation)
    QTimer::singleShot(0, this, [this, id, tag, revision] {
      auto c = channelsById.value(id);
      if (!c || !c->haveObservation) return;
      auto observer = c->observers.find(tag);
      if (observer == c->observers.end() || observer->revision != revision ||
          observer->cancelled || observer->delivered)
        return;
      observer->delivered = true;
      const auto callback = observer->callback;
      if (callback) callback(c->last);
    });
}
void PvAccess::deliverAlarm(quint64 id, Event event) {
  auto c = channelsById.value(id);
  if (!c || c->cancelled) return;
  c->last = event;
  c->haveObservation = true;
  QVector<QPair<const void*, quint64>> recipients;
  for (auto it = c->observers.cbegin(); it != c->observers.cend(); ++it)
    if (!it->cancelled) recipients.push_back({it.key(), it->revision});
  for (const auto& recipient : recipients) {
    c = channelsById.value(id);
    if (!c) return;
    auto observer = c->observers.find(recipient.first);
    if (observer == c->observers.end() || observer->revision != recipient.second || observer->cancelled)
      continue;
    observer->delivered = true;
    const auto callback = observer->callback;
    if (callback) callback(event);
  }
}
void PvAccess::text(const QString& name, std::function<void(QString)> callback) {
  auto c = add(name);
  if (!c) return;
  c->text = std::move(callback);
  subscribe(c);
}
void PvAccess::number(const QString& n, std::function<void(double)> callback, const void* tag) {
  auto c = add(n);
  if (!c) return;
  c->numeric = std::move(callback);
  c->tag = tag;
  subscribe(c);
}
void PvAccess::prepare(const QString& n) {
  for (auto c : channelsByName.value(n))
    if (!c->cancelled) {
      c->prepared = true;
      ensurePut(c);
      return;
    }
  auto c = add(n);
  if (!c) return;
  c->prepared = true;
  ensurePut(c);
}
void PvAccess::setInitialAckT(const QString& n, bool v) {
  records[n].ackt = v;
}
void PvAccess::configureInitialAckT(const Document& document) {
  // Subgroups before direct channels, as ChannelAccess::configureInitialAckT.
  std::function<void(Node*)> visit = [&](Node* group) {
    if (!group) return;
    for (const auto& child : group->children)
      if (child->group) visit(child.get());
    for (const auto& child : group->children)
      if (!child->group && isPvaName(child->name)) setInitialAckT(child->name, !child->mask[AckT]);
  };
  visit(document.root.get());
}
bool PvAccess::canWrite(const QString& name) const {
  if (options.passive)
    return false;
  for (auto c : channelsByName.value(name))
    if (!c->cancelled && c->connected)
      return true;
  return false;
}
bool PvAccess::put(const QString& n, double v, WriteKind kind) {
  if (options.debug)
    debugLog(true, "PVA", QString("put %1 value=%2 kind=%3%4").arg(n).arg(v, 0, 'g', 17)
        .arg(int(kind)).arg(options.passive ? " blocked (passive)" : ""));
  if (options.passive)
    return false;
  if (kind == WriteKind::Severity)
    pendingSeverity[n] = v;
  for (auto c : channelsByName.value(n)) {
    if (c->cancelled || !c->connected)
      continue;
    if (kind == WriteKind::Acknowledge || kind == WriteKind::AckTransient) {
      // putAcks/putAckt from EPICS dbAccess.c, applied to this runtime's state.
      auto& r = records[n];
      if (kind == WriteKind::Acknowledge) {
        if (int(v) >= r.acks && r.acks) {
          r.acks = 0;
          changeRecord(n);
        }
      } else if (int(v != 0) != r.ackt) {
        r.ackt = v != 0;
        if (!r.ackt && r.acks > r.severity)
          r.acks = r.severity;
        changeRecord(n);
      }
      return true;
    }
    const auto id = c->id;
    ensurePut(c);
    if (!(c = channelsById.value(id)))
      return false;
    if (!c->ops->put)
      return false;
    if (!c->puts.empty() && c->puts.back().kind == kind &&
        (kind == WriteKind::Severity || kind == WriteKind::Heartbeat)) {
      c->puts.back().value = v;
    } else {
      if (c->puts.size() >= MaximumPendingWrites) {
        if (!c->putOverflow) {
          c->putOverflow = true;
          report("Write " + n + ": pending write limit reached; write rejected");
        }
        return false;
      }
      c->puts.push_back({v, kind});
    }
    // A queued severity stays pending until the server confirms it, so a
    // disconnect, refusal, timeout or rejection replays it on connection.
    sendPut(c);
    if ((c = channelsById.value(id)) && !c->putBusy && !c->puts.empty() && !c->putDeadline) {
      // Queued behind an unanswered put setup.
      c->putDeadline = monotonicNow() + qMax(1, writeTimeoutMs);
      noteDeadline(c);
    }
    return true;
  }
  debugLog(options.debug, "PVA", "write unavailable: " + n);
  report("PV not connected or not writable: " + n);
  return false;
}
void PvAccess::cancel(const QString& n, const void* tag) {
  for (auto c : channelsByName.value(n))
    if (c->alarm) {
      bool active = false;
      for (auto it = c->observers.begin(); it != c->observers.end(); ++it) {
        if (!tag || it.key() == tag) it->cancelled = true;
        active |= !it->cancelled;
      }
      c->cancelled = !active;
      if (!active) {
        c->haveLast = c->haveObservation = false;
        c->ops->stopMonitor();
      }
    }
}
void PvAccess::destroy(Channel* c) {
  channelsById.remove(c->id);
  auto names = channelsByName.find(c->name);
  if (names != channelsByName.end()) {
    names->removeAll(c);
    if (names->isEmpty()) channelsByName.erase(names);
  }
  auto found = std::find_if(channels.begin(), channels.end(), [c](const auto& p) { return p.get() == c; });
  if (found != channels.end())
    channels.erase(found);
}
void PvAccess::cancelNumbers(const void* tag) {
  QVector<Channel*> obsolete;
  for (auto& p : channels) {
    auto c = p.get();
    if (!c->numeric || c->tag != tag)
      continue;
    c->ops->stopMonitor();
    c->numeric = {};
    if (c->prepared) {
      // Retain a severity/heartbeat/ACK write connection, as ChannelAccess.
      c->tag = nullptr;
      c->cancelled = false;
    } else {
      obsolete.push_back(c);
    }
  }
  for (auto c : obsolete)
    destroy(c);
}
void PvAccess::clear() {
  deadlineTimer.stop();
  nextDeadline = 0;
  readsByItem.clear();
  reads.clear();
  channelsById.clear();
  channelsByName.clear();
  channels.clear();
  pendingSeverity.clear();
  changedRecords.clear();
  restarting.clear();
  {
    std::lock_guard<std::mutex> guard(inbox->lock);
    inbox->updates.clear(); inbox->bytes = 0; inbox->overflow = false;
  }
  // Acknowledgement state outlives monitors, as an IOC record's ACKS does.
}
void PvAccess::noteDeadline(const Channel* c) {
  qint64 deadline = c->cancelled ? 0 : c->connectionDeadline;
  if (c->putDeadline && (!deadline || c->putDeadline < deadline)) deadline = c->putDeadline;
  if (!deadline || (nextDeadline && deadline >= nextDeadline))
    return;
  nextDeadline = deadline;
  deadlineTimer.start(int(qBound(qint64(0), nextDeadline - monotonicNow(), qint64(1000))));
}
void PvAccess::checkDeadlines() {
  const auto now = monotonicNow();
  if (nextDeadline && now < nextDeadline) {
    deadlineTimer.start(int(qMin<qint64>(nextDeadline - now, 1000)));
    return;
  }
  nextDeadline = 0;
  // A PV that never existed sends no disconnect; report it after one second.
  QVector<quint64> expired;
  QVector<quint64> writes;
  for (auto& c : channels)
    if (c->putDeadline && now >= c->putDeadline) writes << c->id;
  for (auto id : writes)
    if (auto c = channelsById.value(id)) {
      c->putDeadline = 0;
      c->putBusy = c->putOverflow = false;
      c->puts.clear();
      c->ops->stopPut();
      report("Write " + c->name + ": operation timed out; pending writes cancelled (acceptance uncertain)");
    }
  for (auto& c : channels)
    if (!c->cancelled && c->connectionDeadline && now >= c->connectionDeadline) {
      c->connectionDeadline = 0;
      if (!c->connected)
        expired.push_back(c->id);
    }
  for (auto id : expired)
    if (auto c = channelsById.value(id)) {
      if (const auto callback = c->alarm)
        callback({ALARM_NSTATUS, 4, 0, -1, "0"});
      if ((c = channelsById.value(id)))
        numericUnavailable(c, "PV not connected");
    }
  for (auto& c : channels)
    noteDeadline(c.get());
}
void PvAccess::changeRecord(const QString& name) {
  // Deliver as a later monitor event, as the IOC confirms ACKS/ACKT writes.
  const bool idle = changedRecords.isEmpty();
  changedRecords.insert(name);
  if (idle)
    QTimer::singleShot(0, this, [this] { publishRecords(); });
}
void PvAccess::publishRecords() {
  const auto changed = std::exchange(changedRecords, {});
  QVector<quint64> ids;
  for (auto& c : channels)
    if (c->alarm && changed.contains(c->name))
      ids.push_back(c->id);
  for (auto id : ids) {
    auto c = channelsById.value(id);
    if (!c || c->cancelled || !c->haveLast || !c->alarm)
      continue;
    const auto& r = records[c->name];
    c->last.acks = r.acks;
    c->last.ackt = r.ackt;
    const auto callback = c->alarm;
    callback(c->last);
  }
}
void PvAccess::connected(Channel* c) {
  debugLog(options.debug, "PVA", c->name + " connected");
  c->connected = true;
  c->connectionDeadline = 0;
  const auto id = c->id;
  if (c->prepared)
    ensurePut(c);
  if (!(c = channelsById.value(id)))
    return;
  if (pendingSeverity.contains(c->name))
    put(c->name, pendingSeverity.value(c->name), WriteKind::Severity);
}
void PvAccess::disconnected(Channel* c) {
  debugLog(options.debug, "PVA", c->name + " disconnected");
  c->connected = false;
  c->haveLast = false;
  c->ops->stopPut();
  const bool writing = c->putBusy || !c->puts.empty();
  c->putBusy = false;
  c->putDeadline = 0;
  c->putOverflow = false;
  c->writeError.clear();
  c->puts.clear();
  const auto id = c->id;
  if (writing) {
    report("Write " + c->name + ": PV disconnected");
    if (!(c = channelsById.value(id))) return;
  }
  if (c->cancelled)
    return;
  if (const auto callback = c->alarm)
    callback({ALARM_NSTATUS, 4, 0, -1, "0"});
  if ((c = channelsById.value(id)))
    numericUnavailable(c, "PV not connected");
}
void PvAccess::monitorFailed(Channel* c, const QString& message) {
  if (c->cancelled)
    return;
  c->haveLast = false;
  const auto id = c->id;
  if (const auto callback = c->alarm)
    callback({ALARM_NSTATUS, 4, 0, -1, "0"});
  if (!(c = channelsById.value(id)))
    return;
  if (c->numeric)
    numericUnavailable(c, "Monitor failed: " + message);
  else
    report("Monitor " + c->name + ": " + message);
}
void PvAccess::sample(Channel* c, const Sample& s) {
  if (c->cancelled || !c->connected)
    return;
  if (const auto callback = c->text) {
    callback(s.text);
    return;
  }
  if (c->numeric) {
    // Alarm-only updates must not advance stateful CALC inputs.
    if (!s.valueChanged)
      return;
    if (!s.numeric) {
      numericUnavailable(c, "PV value is not numeric");
      return;
    }
    c->numericError.clear();
    const auto callback = c->numeric;
    callback(s.number);
    return;
  }
  if (!c->alarm)
    return;
  // recGblResetAlarms: an alarm change raises ACKS unless ACKT retains a
  // higher unacknowledged severity.
  auto& r = records[c->name];
  if (r.status != s.status || r.severity != s.severity ||
      r.alarmStatus != s.alarmStatus || r.alarmMessage != s.alarmMessage) {
    if (!r.ackt || s.severity >= r.acks)
      r.acks = s.severity;
    r.status = s.status;
    r.severity = s.severity;
    r.alarmStatus = s.alarmStatus;
    r.alarmMessage = s.alarmMessage;
  }
  c->last = {s.status, s.severity, r.acks, r.ackt, s.text};
  c->haveLast = true;
  const auto callback = c->alarm;
  callback(c->last);
}
void PvAccess::readStrings(const QStringList& names,
                           std::function<void(QHash<QString, QString>)> done, int timeoutMs) {
  auto read = std::make_unique<Read>();
  read->id = ++nextId;
  read->done = std::move(done);
  if (connectProvider())
    for (const auto& name : names) {
      auto ops = std::make_shared<Operations>();
      ops->id = ++nextId;
      ops->name = name;
      try {
        ops->channelRequester = std::make_shared<ChannelRequester>(inbox, ops->id);
        ops->channel = provider->createChannel(name.mid(6).toStdString(), ops->channelRequester,
                                               pva::ChannelProvider::PRIORITY_DEFAULT);
        ops->getRequester = std::make_shared<GetRequester>(inbox, ops->id);
        ops->get = ops->channel->createChannelGet(ops->getRequester,
                                                  request("field(value,display.precision)"));
      } catch (const std::exception&) {
        continue;
      }
      readsByItem.insert(ops->id, read.get());
      read->items.push_back(std::move(ops));
    }
  read->pending = int(read->items.size());
  const auto id = read->id;
  const bool empty = !read->pending;
  reads.push_back(std::move(read));
  QTimer::singleShot(empty ? 0 : timeoutMs, this, [this, id] { finishRead(id); });
}
void PvAccess::finishRead(quint64 id) {
  auto found = std::find_if(reads.begin(), reads.end(), [id](const auto& r) { return r->id == id; });
  if (found == reads.end())
    return;
  auto owned = std::move(*found);
  reads.erase(found);
  for (auto& item : owned->items)
    readsByItem.remove(item->id);
  owned->items.clear();
  if (owned->done)
    owned->done(owned->values);
}
void PvAccess::recoverInbox() {
  {
    std::lock_guard<std::mutex> guard(inbox->lock);
    inbox->updates.clear(); inbox->bytes = 0; inbox->overflow = false;
  }
  report("PVA update buffer overflow: observations lost; requesting fresh channel state");
  QVector<quint64> readIds;
  for (const auto& r : reads) readIds << r->id;
  for (auto id : readIds) finishRead(id);
  QVector<quint64> ids;
  restarting.clear();
  for (const auto& c : channels) ids << c->id;
  for (auto id : ids) {
    auto c = channelsById.value(id);
    if (!c) continue;
    disconnected(c); // Publish an observation gap before any fresh sample.
    if (!(c = channelsById.value(id))) continue;
    c->ops.reset(); // Subsequent callbacks carry an obsolete channel id.
    c->ops = std::make_shared<Operations>();
    channelsById.remove(id);
    c->id = ++nextId;
    if (c->alarm) {
      const auto freshId = c->id;
      c->alarm = [this, freshId](Event event) { deliverAlarm(freshId, std::move(event)); };
    }
    c->ops->id = c->id;
    c->ops->name = c->name;
    c->haveObservation = false;
    c->connectionDeadline = 0;
    channelsById.insert(c->id, c);
    restarting.push_back(c->id);
  }
  if (!restarting.empty() && !restartScheduled) {
    restartScheduled = true;
    QTimer::singleShot(100, this, [this] { resumeChannels(); });
  }
}
void PvAccess::resumeChannels() {
  restartScheduled = false;
  // Large configurations must not reproduce the overflow by reconnecting all
  // channels in one GUI turn. Leave time to drain connection/initial events.
  for (int i = 0; i < 64 && !restarting.empty() && pendingUpdateCount() < 256; ++i) {
    const auto id = restarting.front();
    restarting.pop_front();
    auto c = channelsById.value(id);
    if (!c) continue;
    c->connectionDeadline = monotonicNow() + 1000;
    noteDeadline(c);
    createChannel(c);
    if ((c = channelsById.value(id))) subscribe(c);
  }
  if (!restarting.empty()) {
    restartScheduled = true;
    QTimer::singleShot(10, this, [this] { resumeChannels(); });
  }
}
void PvAccess::drain() {
  std::vector<Inbox::Update> updates;
  bool overflow;
  {
    std::lock_guard<std::mutex> guard(inbox->lock);
    overflow = inbox->overflow;
    for (int i = 0; i < 256 && !inbox->updates.empty(); ++i) {
      inbox->bytes -= Inbox::cost(inbox->updates.front());
      updates.push_back(std::move(inbox->updates.front()));
      inbox->updates.pop_front();
    }
  }
  if (overflow) recoverInbox();
  for (auto& u : updates) {
    if (auto read = readsByItem.value(u.channel)) {
      auto item = std::find_if(read->items.begin(), read->items.end(),
                               [&](const auto& i) { return i->id == u.channel; });
      if (item == read->items.end())
        continue;
      auto& ops = **item;
      if (u.kind == Update::GetReady && !ops.requested) {
        // One get per item: a reconnect must not answer this field twice.
        ops.requested = true;
        ops.get->get();
      } else if (u.kind == Update::GetDone && !ops.answered) {
        ops.answered = true;
        if (u.ok)
          read->values[ops.name] = u.sample.text.trimmed();
        if (--read->pending == 0)
          QTimer::singleShot(0, this, [this, id = read->id] { finishRead(id); });
      }
      continue;
    }
    auto c = channelsById.value(u.channel);
    if (!c)
      continue;
    auto& ops = *c->ops;
    switch (u.kind) {
    case Update::Connected:
      connected(c);
      break;
    case Update::Disconnected:
      disconnected(c);
      break;
    case Update::Data:
      if (u.serial == ops.monitorSerial)
        sample(c, u.sample);
      break;
    case Update::Failed:
      if (u.serial == ops.monitorSerial)
        monitorFailed(c, u.message);
      break;
    case Update::PutReady:
      if (u.serial != ops.putSerial)
        break;
      ops.putReady = u.ok;
      ops.putStructure = u.structure;
      c->putDeadline = 0;
      if (u.ok)
        sendPut(c);
      else {
        // Release a rejected operation so a later explicit write can retry.
        // Keeping it would silently accumulate writes behind putReady=false.
        c->puts.clear();
        c->putBusy = false;
        c->putOverflow = false;
        ops.stopPut();
        writeFailed(c, u.message);
      }
      break;
    case Update::PutDone:
      if (u.serial != ops.putSerial)
        break;
      c->putBusy = false;
      c->putDeadline = 0;
      debugLog(options.debug, "PVA", "write " + c->name + ": " + (u.ok ? "done" : u.message));
      if (!u.ok) {
        writeFailed(c, u.message);
        if (!(c = channelsById.value(u.channel)))
          break;
      } else {
        c->writeError.clear();
        if (c->sending.kind == WriteKind::Severity && pendingSeverity.contains(c->name) &&
            pendingSeverity.value(c->name) == c->sending.value)
          pendingSeverity.remove(c->name); // A newer requested severity stays pending.
      }
      sendPut(c);
      break;
    default:
      break;
    }
  }
  {
    std::lock_guard<std::mutex> guard(inbox->lock);
    if (inbox->updates.empty() && !inbox->overflow)
      inbox->scheduled = false;
    else
      QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
  }
}
} // namespace alh
