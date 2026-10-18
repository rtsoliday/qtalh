#include "core/diagnostics.h"
// Non-preemptive CA adapter replacing ALH's Xt input/timer callbacks.
#include "channel_access.h"
#include <alarm.h>
#include <db_access.h>
#include <QCoreApplication>
#include <QPointer>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace alh {
namespace {
QVector<ChannelAccess*> contexts;
QSet<int> caDescriptors;
bool dispatching = false;
// ALH's alCaException reports at most this many exceptions per CA context.
constexpr int maxExceptions = 25;
quintptr contextGeneration = 0;
int exceptions = 0;
// CA calls this on its own threads for asynchronous errors, such as an IOC
// rejecting a put. Without a handler CA prints them and aborts the process on
// error severities. Report them on the Qt thread instead, as ALH does.
void caException(exception_handler_args args) {
  const char* message = ca_message(args.stat);
  auto text = QString("Channel Access Exception: %1  Context: %2")
                  .arg(message ? message : "Unavailable",
                       args.ctx ? QString::fromLocal8Bit(args.ctx) : QString("Unavailable"));
  if (args.chid)
    text += "  PV: " + QString::fromLocal8Bit(ca_name(args.chid));
  const auto generation = reinterpret_cast<quintptr>(args.usr);
  auto app = QCoreApplication::instance();
  if (!app) {
    qWarning("QtALH: %s", qPrintable(text));
    return;
  }
  QMetaObject::invokeMethod(app, [text, generation] {
    // Exceptions of a destroyed context, or beyond the limit, are only printed.
    bool delivered = false;
    if (generation == contextGeneration && exceptions <= maxExceptions) {
      const auto report = ++exceptions > maxExceptions
          ? QString("Channel Access Exception: Too many exceptions [%1]").arg(maxExceptions)
          : text;
      for (auto self : QVector<ChannelAccess*>(contexts))
        if (contexts.contains(self) && self->error) {
          self->error(report);
          delivered = report == text;
        }
    }
    if (!delivered)
      qWarning("QtALH: %s", qPrintable(text));
  }, Qt::QueuedConnection);
}
} // namespace

ChannelAccess::ChannelAccess(EngineOptions o, QObject* parent) : QObject(parent), options(o) {
  if (contexts.isEmpty()) {
    int status = ca_context_create(ca_disable_preemptive_callback);
    if (status != ECA_NORMAL)
      throw std::runtime_error(ca_message(status));
    exceptions = 0;
    ca_add_exception_event(caException, reinterpret_cast<void*>(++contextGeneration));
  }
  contexts.push_back(this);
  active = true;
  ca_add_fd_registration(fd, nullptr);
  for (int descriptor : caDescriptors) {
    if (sockets.contains(descriptor))
      continue;
    auto notifier = new QSocketNotifier(descriptor, QSocketNotifier::Read, this);
    sockets[descriptor] = notifier;
    connect(notifier, &QSocketNotifier::activated, this, [this] { poll(); });
  }
  timer.setInterval(100);
  connect(&timer, &QTimer::timeout, this, [this] { poll(); });
  timer.start();
}
ChannelAccess::~ChannelAccess() {
  timer.stop();
  clear();
  contexts.removeAll(this);
  if (active && contexts.isEmpty()) {
    ca_add_fd_registration(nullptr, nullptr);
    ca_context_destroy();
    caDescriptors.clear();
  }
  active = false;
  qDeleteAll(sockets);
}
void ChannelAccess::report(int status, const QString& context) {
  if (status != ECA_NORMAL && error)
    error(context + ": " + ca_message(status));
}
void ChannelAccess::numericUnavailable(Channel* c, const QString& reason) {
  if (!c->numeric)
    return;
  if (c->numericError != reason) {
    c->numericError = reason;
    if (error)
      error(reason + ": " + c->name);
  }
  // CALC inputs must become unavailable, while scalar force masks retain the
  // last valid value until the connection recovers.
  c->numeric(std::numeric_limits<double>::quiet_NaN());
}
ChannelAccess::Channel* ChannelAccess::add(const QString& name) {
  debugLog(options.debug, "CA", "create " + name);
  auto p = std::make_unique<Channel>();
  p->owner = this;
  p->serial = ++nextSerial;
  p->name = name;
  p->connectionDeadline = monotonicNow() + 1000;
  auto c = p.get();
  channels.push_back(std::move(p));
  channelsByName[name].push_back(c);
  channelsBySerial.insert(c->serial, c);
  noteDeadline(c);
  report(
      ca_create_channel(name.toLocal8Bit().constData(), connection, c, CA_PRIORITY_DEFAULT, &c->id),
      "Create " + name);
  if (c->id)
    report(ca_replace_access_rights_event(c->id, access), "Access callback " + name);
  return c;
}
void ChannelAccess::monitor(const QString& n, std::function<void(Event)> callback,
                            const void* tag) {
  Channel* c = nullptr;
  // A startup force can add monitoring before Engine::start reaches this row.
  // Reuse active as well as cancelled monitors for the same owner/PV pair;
  // other owners of the same PV must keep their independent subscriptions.
  // The name index preserves creation order, avoiding a scan of every channel.
  const auto named = channelsByName.constFind(n);
  if (named != channelsByName.cend())
    for (auto channel : *named)
      if (channel->tag == tag && channel->alarm) {
        c = channel;
        break;
      }
  if (!c)
    c = add(n);
  // Access callbacks are ignored during Cancel and CA need not repeat them on
  // Add. Defer the check until poll(): an inline alarm here would re-enter
  // Engine::applyMask before it has updated the parent group counts.
  if (c->cancelled)
    c->recheckAccess = recheckPending = true;
  c->cancelled = false;
  c->tag = tag;
  c->alarm = std::move(callback);
  // A disconnect while cancelled was deliberately ignored. Re-arm the startup
  // check when reusing that channel, even if it connected successfully earlier.
  if (!c->id || ca_state(c->id) != cs_conn)
    c->connectionDeadline = monotonicNow() + 1000;
  noteDeadline(c);
  subscribe(c);
  ca_flush_io();
}
void ChannelAccess::text(const QString& name, std::function<void(QString)> callback) {
  auto c = add(name);
  c->text = std::move(callback);
  subscribe(c);
  ca_flush_io();
}
void ChannelAccess::number(const QString& n, std::function<void(double)> callback,
                           const void* tag) {
  auto c = add(n);
  c->numeric = std::move(callback);
  c->tag = tag;
  c->numericError.clear();
  if (!c->id || ca_state(c->id) != cs_conn)
    c->connectionDeadline = monotonicNow() + 1000;
  noteDeadline(c);
  subscribe(c);
  ca_flush_io();
}
void ChannelAccess::prepare(const QString& n) {
  for (auto c : channelsByName.value(n))
    if (!c->cancelled) {
      c->prepared = true;
      return;
    }
  add(n)->prepared = true;
  ca_flush_io();
}
void ChannelAccess::setInitialAckT(const QString& n, bool v) {
  initialAckTargets.insert(n);
  // ACKT belongs to the record, even when CA addresses .VAL or another field.
  // Unrelated server-side aliases cannot be resolved from the client PV name.
  initialAckT[n.section('.', 0, 0)] = v;
}
void ChannelAccess::configureInitialAckT(const Document& document) {
  initialAckT.clear();
  initialAckTargets.clear();
  // alPutGblAckT visits subgroups before direct channels. Retain the final
  // setting for each record, independent of declaration/connection order or
  // serialization, including rows addressing different fields of that record.
  std::function<void(Node*)> visit = [&](Node* group) {
    if (!group) return;
    for (const auto& child : group->children)
      if (child->group) visit(child.get());
    for (const auto& child : group->children)
      if (!child->group) setInitialAckT(child->name, !child->mask[AckT]);
  };
  visit(document.root.get());
}
void ChannelAccess::subscribe(Channel* c) {
  if (!c->id || c->cancelled || c->subscription || ca_state(c->id) != cs_conn ||
      (!c->alarm && !c->numeric && !c->text))
    return;
  chtype type = c->numeric ? DBR_DOUBLE : c->text ? DBR_STRING : DBR_STSACK_STRING;
  // Force inputs follow numeric value changes, as in ALH. Alarm-only events
  // must not advance stateful CALCs; connection/access callbacks still mark
  // inputs unavailable and subscription recovery supplies a fresh value.
  const long events = c->numeric ? DBE_VALUE : DBE_VALUE | DBE_ALARM;
  report(ca_create_subscription(type, 1, c->id, events, update, c, &c->subscription),
         "Subscribe " + c->name);
}
void ChannelAccess::defer(Callback callback) {
  std::lock_guard<std::mutex> guard(callbackLock);
  callbacks.push_back(std::move(callback));
}
void ChannelAccess::dispatch() {
  std::vector<Callback> batch;
  {
    std::lock_guard<std::mutex> guard(callbackLock);
    batch.swap(callbacks);
  }
  // A callback can clear channels or destroy this adapter (for example by
  // replacing the configuration); later entries must then be discarded.
  QPointer<ChannelAccess> self(this);
  for (const auto& callback : batch) {
    if (!self)
      return;
    if (callback.kind == Callback::ReadConnection || callback.kind == Callback::ReadValue) {
      auto read = std::find_if(reads.begin(), reads.end(),
                               [&](const auto& r) { return r->id == callback.read; });
      if (read == reads.end() ||
          std::none_of((*read)->items.begin(), (*read)->items.end(),
                       [&](const auto& item) { return item.get() == callback.item; }))
        continue;
      if (callback.kind == Callback::ReadConnection)
        readConnected(callback.item, callback.op);
      else
        readCompleted(callback.item, callback);
      continue;
    }
    auto c = channelsBySerial.value(callback.channel);
    if (!c)
      continue;
    if (callback.kind == Callback::Connection)
      connected(c, callback.op);
    else if (callback.kind == Callback::Access)
      rightsChanged(c);
    else if (c->epoch == callback.epoch)
      updated(c, callback);
  }
}
void ChannelAccess::connection(connection_handler_args a) {
  auto c = static_cast<Channel*>(ca_puser(a.chid));
  if (!c)
    return;
  Callback callback;
  callback.kind = Callback::Connection;
  callback.channel = c->serial;
  callback.op = a.op;
  c->owner->defer(std::move(callback));
}
void ChannelAccess::access(access_rights_handler_args a) {
  auto c = static_cast<Channel*>(ca_puser(a.chid));
  if (!c)
    return;
  Callback callback;
  callback.kind = Callback::Access;
  callback.channel = c->serial;
  c->owner->defer(std::move(callback));
}
void ChannelAccess::update(event_handler_args a) {
  auto c = static_cast<Channel*>(a.usr);
  if (!c)
    return;
  Callback callback;
  callback.kind = Callback::Update;
  callback.channel = c->serial;
  callback.epoch = c->epoch;
  callback.status = a.status;
  callback.type = a.type;
  // The DBR buffer belongs to CA and is valid only during this callback.
  if (a.status == ECA_NORMAL && a.dbr) {
    callback.data = true;
    if (a.type == DBR_STRING) {
      auto value = static_cast<const char*>(a.dbr);
      callback.text = QString::fromLocal8Bit(value, int(strnlen(value, MAX_STRING_SIZE)));
    } else if (a.type == DBR_DOUBLE) {
      callback.number = *static_cast<const double*>(a.dbr);
    } else if (a.type == DBR_STSACK_STRING) {
      auto d = static_cast<const dbr_stsack_string*>(a.dbr);
      callback.event = {qBound(0, int(d->status), ALARM_NSTATUS - 1), qBound(0, int(d->severity), 3),
                        qBound(0, int(d->acks), 3), int(d->ackt),
                        QString::fromLocal8Bit(d->value, int(strnlen(d->value, MAX_STRING_SIZE)))};
    } else {
      return;
    }
  }
  c->owner->defer(std::move(callback));
}
void ChannelAccess::connected(Channel* c, long op) {
  if (clearing || c->cancelled)
    return;
  if (options.debug)
    debugLog(true, "CA", c->name + (op == CA_OP_CONN_UP ? " connected" : " disconnected"));
  if (op == CA_OP_CONN_UP) {
    c->connectionDeadline = 0;
    subscribe(c);
    rightsChanged(c);
  } else if (op == CA_OP_CONN_DOWN) {
    c->haveLast = false; // Recovery must wait for a fresh subscription event.
    if (c->alarm)
      c->alarm({ALARM_NSTATUS, 4, 0, -1, "0"});
    // The alarm callback may have cancelled or removed this channel.
    if ((c = channelsBySerial.value(c->serial)))
      numericUnavailable(c, "PV not connected");
  }
}
void ChannelAccess::rightsChanged(Channel* c) {
  if (clearing || c->cancelled || !c->id || ca_state(c->id) != cs_conn)
    return;
  const auto a = c->id;
  c->recheckAccess = false;
  if (options.debug)
    debugLog(true, "CA", QString("%1 access read=%2 write=%3").arg(c->name)
        .arg(ca_read_access(a)).arg(ca_write_access(a)));
  if (!ca_read_access(a)) {
    // A cached alarm predates the access gap. Replaying it on recovery can
    // dispatch false commands/outputs before the IOC's current event arrives.
    c->haveLast = false;
    c->owner->numericUnavailable(c, "No read access for PV");
  }
  if (c->alarm) {
    if (!ca_read_access(a))
      c->alarm({ALARM_NSTATUS + 1, 4, 0, -1, "0"});
    else if (c->owner->options.global && !c->owner->options.passive && !ca_write_access(a))
      c->alarm({ALARM_NSTATUS + 2, 4, 0, -1, "0"});
    else if (c->haveLast)
      c->alarm(c->last);
  }
  // Write access ends an outage; a later unavailable write is reported again.
  if (ca_write_access(a))
    c->owner->writeUnavailable.remove(c->name);
  // Retry a startup setting when write access arrives after connection. Once
  // applied through any configured field, mark the whole record complete so
  // later field connections cannot overwrite operator/IOC changes. Keep using
  // a configured target's own connection and write rights; auxiliary .DESC or
  // output connections must not submit the startup setting on its behalf.
  auto& o = c->owner->options;
  const auto record = c->name.section('.', 0, 0);
  if (o.caputAckT && o.global && !o.passive && !c->owner->initialAckWritten.contains(record) &&
      c->owner->initialAckTargets.contains(c->name) && ca_write_access(c->id)) {
    if (c->owner->put(c->name, c->owner->initialAckT[record], WriteKind::AckTransient))
      c->owner->initialAckWritten.insert(record);
  }
  // Connection-up also dispatches this path. Coalesce failed severity writes
  // by PV name, and replay only when connection and write access are available.
  // Possible inherited ALH bug, deferred: successful severity writes are no
  // longer pending, so an output IOC restart can leave SEVRPV at its startup
  // value while the monitored alarm stays unchanged. Address republishing the
  // current desired severity on reconnect in a later change; retain behavior now.
  if (ca_write_access(a) && c->owner->pendingSeverity.contains(c->name))
    c->owner->put(c->name, c->owner->pendingSeverity.value(c->name), WriteKind::Severity);
}
void ChannelAccess::updated(Channel* c, const Callback& a) {
  if (clearing || c->cancelled)
    return;
  if (!a.data) {
    // Possible inherited ALH bug, deferred: alarm monitor failures only report
    // a diagnostic below, leaving the last alarm state and haveLast cache valid.
    // A previously healthy channel can remain available with no ERROR latch.
    // Address invalidation/error state and fresh-sample recovery in a later
    // change; preserve the current alarm behavior here.
    if (c->numeric)
      numericUnavailable(c, "Monitor failed: " + QString::fromLocal8Bit(ca_message(a.status)));
    else
      report(a.status, "Monitor " + c->name);
    return;
  }
  if (c->text) {
    // Descriptions require read access only. Preserve the last valid text while
    // disconnected or unreadable instead of replacing it with an alarm value.
    if (a.type == DBR_STRING && ca_read_access(c->id))
      c->text(a.text);
    return;
  }
  if (c->numeric) {
    if (a.type != DBR_DOUBLE)
      return;
    if (!ca_read_access(c->id)) {
      numericUnavailable(c, "No read access for PV");
      return;
    }
    c->numericError.clear();
    c->numeric(a.number);
    return;
  }
  if (a.type != DBR_STSACK_STRING)
    return;
  c->recheckAccess = false; // This fresh event also checks the current rights below.
  Event e = a.event;
  if (!ca_read_access(c->id)) {
    // Do not repopulate the cache from events queued before access was lost.
    c->haveLast = false;
    e = {ALARM_NSTATUS + 1, 4, 0, -1, "0"};
  } else {
    c->last = e;
    c->haveLast = true;
    // Write-only access loss still permits fresh read observations; retain
    // that cache so write access recovery works even without a value change.
    if (options.global && !options.passive && !ca_write_access(c->id))
      e = {ALARM_NSTATUS + 2, 4, 0, -1, "0"};
  }
  if (c->alarm)
    c->alarm(e);
}
bool ChannelAccess::canWrite(const QString& name) const {
  if (options.passive)
    return false;
  for (auto c : channelsByName.value(name))
    if (!c->cancelled && c->id && ca_state(c->id) == cs_conn && ca_write_access(c->id))
      return true;
  return false;
}
bool ChannelAccess::put(const QString& n, double v, WriteKind kind) {
  if (options.debug)
    debugLog(true, "CA", QString("put %1 value=%2 kind=%3%4").arg(n).arg(v, 0, 'g', 17)
        .arg(int(kind)).arg(options.passive ? " blocked (passive)" : ""));
  if (options.passive)
    return false;
  if (kind == WriteKind::Severity)
    pendingSeverity[n] = v;
  for (auto c : channelsByName.value(n))
    if (!c->cancelled && c->id && ca_state(c->id) == cs_conn && ca_write_access(c->id)) {
      int status;
      if (kind == WriteKind::Value)
        status = ca_put(DBR_DOUBLE, c->id, &v);
      else if (kind == WriteKind::Severity || kind == WriteKind::Heartbeat) {
        short value = static_cast<short>(v);
        status = ca_put(DBR_SHORT, c->id, &value);
      } else {
        // ACKPV retains ALH's enum conversion, including signed-short values.
        unsigned short s = static_cast<unsigned short>(static_cast<short>(v));
        const auto type = kind == WriteKind::AckValue ? DBR_ENUM
                          : kind == WriteKind::Acknowledge ? DBR_PUT_ACKS : DBR_PUT_ACKT;
        status = ca_put(type, c->id, &s);
      }
      debugLog(options.debug, "CA", "write " + n + ": " + ca_message(status));
      report(status, "Write " + n);
      if (status == ECA_NORMAL)
        writeUnavailable.remove(n);
      if (kind == WriteKind::Severity && status == ECA_NORMAL)
        pendingSeverity.remove(n);
      ca_flush_io();
      return status == ECA_NORMAL;
    }
  debugLog(options.debug, "CA", "write unavailable: " + n);
  // Each report adds an operation-log record and popup text. Automatic SEVRPV
  // writes repeat on every alarm transition, so report each outage once; it
  // ends when the PV becomes writable or a write to it succeeds.
  if (error && !writeUnavailable.contains(n)) {
    writeUnavailable.insert(n);
    error("PV not connected or not writable: " + n);
  }
  return false;
}
void ChannelAccess::cancel(const QString& n, const void* tag) {
  for (auto c : channelsByName.value(n))
    if (c->alarm && (!tag || c->tag == tag)) {
      c->cancelled = true;
      c->recheckAccess = false;
      c->haveLast = false; // A later Add must not replay data from before Cancel.
      if (c->subscription) {
        ca_clear_subscription(c->subscription);
        c->subscription = nullptr;
        ++c->epoch;
      }
    }
}
void ChannelAccess::cancelNumbers(const void* tag) {
  for (auto it = channels.begin(); it != channels.end();) {
    auto c = it->get();
    if (!c->numeric || c->tag != tag) {
      ++it;
      continue;
    }
    c->cancelled = true;
    if (c->subscription) {
      ca_clear_subscription(c->subscription);
      c->subscription = nullptr;
      ++c->epoch;
    }
    c->numeric = {};
    if (c->prepared) {
      // prepare() may have reused this input for a severity/heartbeat/ACK PV.
      // Retain that write connection, but release the obsolete input callback.
      c->tag = nullptr;
      c->cancelled = false;
      noteDeadline(c);
      ++it;
    } else {
      if (c->id)
        ca_clear_channel(c->id);
      channelsBySerial.remove(c->serial);
      auto names = channelsByName.find(c->name);
      if (names != channelsByName.end()) {
        names->removeAll(c);
        if (names->isEmpty()) channelsByName.erase(names);
      }
      it = channels.erase(it);
    }
  }
  ca_flush_io();
}
void ChannelAccess::clear() {
  if (!active)
    return;
  clearing = true;
  for (auto& read : reads)
    for (auto& item : read->items)
      if (item->id)
        ca_clear_channel(item->id);
  reads.clear();
  for (auto& c : channels) {
    if (c->subscription)
      ca_clear_subscription(c->subscription);
    if (c->id)
      ca_clear_channel(c->id);
  }
  ca_flush_io();
  channels.clear();
  channelsByName.clear();
  channelsBySerial.clear();
  {
    std::lock_guard<std::mutex> guard(callbackLock);
    callbacks.clear();
  }
  recheckPending = false;
  nextDeadline = 0;
  pendingSeverity.clear();
  writeUnavailable.clear();
  initialAckWritten.clear();
  clearing = false;
}
void ChannelAccess::poll() {
  if (!active || dispatching || clearing)
    return;
  dispatching = true;
  ca_poll();
  // Adapters share one CA context, so ca_poll() may have queued work for any of them.
  QVector<QPointer<ChannelAccess>> adapters;
  for (auto adapter : contexts)
    adapters << adapter;
  QPointer<ChannelAccess> self(this);
  for (const auto& adapter : adapters)
    if (adapter)
      adapter->dispatch();
  if (!self) {
    dispatching = false;
    return;
  }
  auto now = monotonicNow();
  // Scan the channels only when an access recheck or connection deadline is
  // pending; the periodic poll otherwise costs nothing per channel.
  if (recheckPending) {
    recheckPending = false;
    for (auto& c : channels) {
      if (!c->recheckAccess || c->cancelled)
        continue;
      c->recheckAccess = false;
      if (c->id && ca_state(c->id) == cs_conn) {
        // Cancel invalidates haveLast; healthy recovery must use fresh IOC data.
        // Keeping this work on the channel also makes cancel/clear discard it.
        rightsChanged(c.get());
      }
    }
  }
  // CA does not send an initial disconnect callback for a PV that never existed.
  if (nextDeadline && now >= nextDeadline) {
    nextDeadline = 0;
    for (auto& c : channels)
      if (!c->cancelled && c->connectionDeadline && now >= c->connectionDeadline) {
        c->connectionDeadline = 0;
        if (c->id && ca_state(c->id) == cs_conn)
          continue;
        if (c->alarm)
          c->alarm({ALARM_NSTATUS, 4, 0, -1, "0"});
        numericUnavailable(c.get(), "PV not connected");
      }
    for (auto& c : channels)
      noteDeadline(c.get());
  }
  dispatching = false;
}
void ChannelAccess::readStrings(const QStringList& names,
                                std::function<void(QHash<QString, QString>)> done, int timeoutMs) {
  auto read = std::make_unique<Read>();
  read->done = std::move(done);
  read->id = ++nextRead;
  read->pending = names.size();
  for (const auto& name : names) {
    auto item = std::make_unique<ReadItem>();
    item->owner = this;
    item->read = read.get();
    item->name = name;
    if (ca_create_channel(name.toLocal8Bit().constData(), readConnection, item.get(),
                          CA_PRIORITY_DEFAULT, &item->id) != ECA_NORMAL) {
      --read->pending;
      item->id = nullptr;
    }
    read->items.push_back(std::move(item));
  }
  ca_flush_io();
  const auto id = read->id;
  const bool empty = !read->pending;
  reads.push_back(std::move(read));
  QTimer::singleShot(empty ? 0 : timeoutMs, this, [this, id] { finishRead(id); });
}
void ChannelAccess::finishRead(quint64 id) {
  // Completion and timeout both arrive here; only the first finishes the read.
  auto found = std::find_if(reads.begin(), reads.end(), [id](const auto& r) { return r->id == id; });
  if (found == reads.end())
    return;
  auto owned = std::move(*found);
  reads.erase(found);
  for (auto& item : owned->items)
    if (item->id)
      ca_clear_channel(item->id);
  ca_flush_io();
  if (owned->done)
    owned->done(owned->values);
}
void ChannelAccess::readConnection(connection_handler_args a) {
  auto item = static_cast<ReadItem*>(ca_puser(a.chid));
  if (!item)
    return;
  Callback callback;
  callback.kind = Callback::ReadConnection;
  callback.item = item;
  callback.read = item->read->id;
  callback.op = a.op;
  item->owner->defer(std::move(callback));
}
void ChannelAccess::readConnected(ReadItem* item, long op) {
  if (clearing || item->requested || op != CA_OP_CONN_UP || !item->id ||
      ca_state(item->id) != cs_conn)
    return;
  if (!ca_read_access(item->id))
    return;
  // One get per item: a reconnect must not complete this field twice and
  // consume another field's pending slot.
  item->requested = true;
  if (ca_array_get_callback(DBR_STRING, 1, item->id, readValue, item) != ECA_NORMAL) {
    item->requested = false;
    return;
  }
  ca_flush_io();
}
void ChannelAccess::readValue(event_handler_args a) {
  auto item = static_cast<ReadItem*>(a.usr);
  if (!item)
    return;
  Callback callback;
  callback.kind = Callback::ReadValue;
  callback.item = item;
  callback.read = item->read->id;
  callback.status = a.status;
  if (a.status == ECA_NORMAL && a.dbr) {
    auto value = static_cast<const char*>(a.dbr);
    callback.data = true;
    callback.text = QString::fromLocal8Bit(value, int(strnlen(value, MAX_STRING_SIZE)));
  }
  item->owner->defer(std::move(callback));
}
void ChannelAccess::readCompleted(ReadItem* item, const Callback& a) {
  if (clearing)
    return;
  auto read = item->read;
  if (a.data)
    read->values[item->name] = a.text.trimmed();
  // Channels are cleared outside their own callback.
  if (--read->pending == 0)
    QTimer::singleShot(0, this, [this, id = read->id] { finishRead(id); });
}
void ChannelAccess::noteDeadline(const Channel* c) {
  // Deadlines cleared by a connection leave this stale; poll() then rescans once.
  if (!c->cancelled && c->connectionDeadline &&
      (!nextDeadline || c->connectionDeadline < nextDeadline))
    nextDeadline = c->connectionDeadline;
}
void ChannelAccess::fd(void* data, int descriptor, int opened) {
  Q_UNUSED(data);
  if (opened)
    caDescriptors.insert(descriptor);
  else
    caDescriptors.remove(descriptor);
  for (auto self : contexts) {
    if (opened) {
      if (self->sockets.contains(descriptor))
        continue;
      auto notifier = new QSocketNotifier(descriptor, QSocketNotifier::Read, self);
      self->sockets[descriptor] = notifier;
      connect(notifier, &QSocketNotifier::activated, self, [self] { self->poll(); });
    } else {
      auto n = self->sockets.take(descriptor);
      if (n) {
        n->setEnabled(false);
        n->deleteLater();
      }
    }
  }
}
} // namespace alh
