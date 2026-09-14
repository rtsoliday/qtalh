#include "core/diagnostics.h"
// Non-preemptive CA adapter replacing ALH's Xt input/timer callbacks.
#include "channel_access.h"
#include <alarm.h>
#include <db_access.h>
#include <limits>
#include <stdexcept>
namespace alh {
namespace {
QVector<ChannelAccess*> contexts;
QSet<int> caDescriptors;
bool dispatching = false;
} // namespace

ChannelAccess::ChannelAccess(EngineOptions o, QObject* parent) : QObject(parent), options(o) {
  if (contexts.isEmpty()) {
    int status = ca_context_create(ca_disable_preemptive_callback);
    if (status != ECA_NORMAL)
      throw std::runtime_error(ca_message(status));
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
  p->name = name;
  p->connectionDeadline = monotonicNow() + 1000;
  auto c = p.get();
  channels.push_back(std::move(p));
  channelsByName[name].push_back(c);
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
  for (auto& channel : channels)
    if (channel->name == n && channel->tag == tag && channel->alarm) {
      c = channel.get();
      break;
    }
  if (!c)
    c = add(n);
  // Access callbacks are ignored during Cancel and CA need not repeat them on
  // Add. Defer the check until poll(): an inline alarm here would re-enter
  // Engine::applyMask before it has updated the parent group counts.
  if (c->cancelled)
    c->recheckAccess = true;
  c->cancelled = false;
  c->tag = tag;
  c->alarm = std::move(callback);
  // A disconnect while cancelled was deliberately ignored. Re-arm the startup
  // check when reusing that channel, even if it connected successfully earlier.
  if (!c->id || ca_state(c->id) != cs_conn)
    c->connectionDeadline = monotonicNow() + 1000;
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
  subscribe(c);
  ca_flush_io();
}
void ChannelAccess::prepare(const QString& n) {
  for (auto& c : channels)
    if (c->name == n && !c->cancelled) {
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
void ChannelAccess::connection(connection_handler_args a) {
  auto c = static_cast<Channel*>(ca_puser(a.chid));
  if (!c || c->owner->clearing || c->cancelled)
    return;
  if (c->owner->options.debug)
    debugLog(true, "CA", c->name + (a.op == CA_OP_CONN_UP ? " connected" : " disconnected"));
  if (a.op == CA_OP_CONN_UP) {
    c->connectionDeadline = 0;
    c->owner->subscribe(c);
    access_rights_handler_args ar{};
    ar.chid = a.chid;
    access(ar);
  } else if (a.op == CA_OP_CONN_DOWN) {
    c->haveLast = false; // Recovery must wait for a fresh subscription event.
    if (c->alarm)
      c->alarm({ALARM_NSTATUS, 4, 0, -1, "0"});
    c->owner->numericUnavailable(c, "PV not connected");
  }
}
void ChannelAccess::access(access_rights_handler_args a) {
  auto c = static_cast<Channel*>(ca_puser(a.chid));
  if (!c || c->owner->clearing || c->cancelled || ca_state(a.chid) != cs_conn)
    return;
  c->recheckAccess = false;
  if (c->owner->options.debug)
    debugLog(true, "CA", QString("%1 access read=%2 write=%3").arg(c->name)
        .arg(ca_read_access(a.chid)).arg(ca_write_access(a.chid)));
  if (!ca_read_access(a.chid)) {
    // A cached alarm predates the access gap. Replaying it on recovery can
    // dispatch false commands/outputs before the IOC's current event arrives.
    c->haveLast = false;
    c->owner->numericUnavailable(c, "No read access for PV");
  }
  if (c->alarm) {
    if (!ca_read_access(a.chid))
      c->alarm({ALARM_NSTATUS + 1, 4, 0, -1, "0"});
    else if (c->owner->options.global && !c->owner->options.passive && !ca_write_access(a.chid))
      c->alarm({ALARM_NSTATUS + 2, 4, 0, -1, "0"});
    else if (c->haveLast)
      c->alarm(c->last);
  }
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
  if (ca_write_access(a.chid) && c->owner->pendingSeverity.contains(c->name))
    c->owner->put(c->name, c->owner->pendingSeverity.value(c->name), WriteKind::Severity);
}
void ChannelAccess::update(event_handler_args a) {
  auto c = static_cast<Channel*>(a.usr);
  if (!c || c->owner->clearing || c->cancelled)
    return;
  if (a.status != ECA_NORMAL || !a.dbr) {
    // Possible inherited ALH bug, deferred: alarm monitor failures only report
    // a diagnostic below, leaving the last alarm state and haveLast cache valid.
    // A previously healthy channel can remain available with no ERROR latch.
    // Address invalidation/error state and fresh-sample recovery in a later
    // change; preserve the current alarm behavior here.
    if (c->numeric)
      c->owner->numericUnavailable(c, "Monitor failed: " +
                                          QString::fromLocal8Bit(ca_message(a.status)));
    else
      c->owner->report(a.status, "Monitor " + c->name);
    return;
  }
  if (c->text) {
    // Descriptions require read access only. Preserve the last valid text while
    // disconnected or unreadable instead of replacing it with an alarm value.
    if (ca_read_access(c->id)) {
      auto value = static_cast<const char*>(a.dbr);
      c->text(QString::fromLocal8Bit(value, int(strnlen(value, MAX_STRING_SIZE))));
    }
    return;
  }
  if (c->numeric) {
    if (!ca_read_access(c->id)) {
      c->owner->numericUnavailable(c, "No read access for PV");
      return;
    }
    c->numericError.clear();
    c->numeric(*static_cast<const double*>(a.dbr));
    return;
  }
  auto d = static_cast<const dbr_stsack_string*>(a.dbr);
  c->recheckAccess = false; // This fresh event also checks the current rights below.
  Event e{qBound(0, int(d->status), ALARM_NSTATUS - 1), qBound(0, int(d->severity), 3),
          qBound(0, int(d->acks), 3), int(d->ackt),
          QString::fromLocal8Bit(d->value, int(strnlen(d->value, MAX_STRING_SIZE)))};
  if (!ca_read_access(c->id)) {
    // Do not repopulate the cache from events queued before access was lost.
    c->haveLast = false;
    e = {ALARM_NSTATUS + 1, 4, 0, -1, "0"};
  } else {
    c->last = e;
    c->haveLast = true;
    // Write-only access loss still permits fresh read observations; retain
    // that cache so write access recovery works even without a value change.
    if (c->owner->options.global && !c->owner->options.passive && !ca_write_access(c->id))
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
      if (kind == WriteKind::Severity && status == ECA_NORMAL)
        pendingSeverity.remove(n);
      ca_flush_io();
      return status == ECA_NORMAL;
    }
  debugLog(options.debug, "CA", "write unavailable: " + n);
  if (error)
    error("PV not connected or not writable: " + n);
  return false;
}
void ChannelAccess::cancel(const QString& n, const void* tag) {
  for (auto& c : channels)
    if (c->name == n && c->alarm && (!tag || c->tag == tag)) {
      c->cancelled = true;
      c->recheckAccess = false;
      c->haveLast = false; // A later Add must not replay data from before Cancel.
      if (c->subscription) {
        ca_clear_subscription(c->subscription);
        c->subscription = nullptr;
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
    }
    c->numeric = {};
    if (c->prepared) {
      // prepare() may have reused this input for a severity/heartbeat/ACK PV.
      // Retain that write connection, but release the obsolete input callback.
      c->tag = nullptr;
      c->cancelled = false;
      ++it;
    } else {
      if (c->id)
        ca_clear_channel(c->id);
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
  for (auto& c : channels) {
    if (c->subscription)
      ca_clear_subscription(c->subscription);
    if (c->id)
      ca_clear_channel(c->id);
  }
  ca_flush_io();
  channels.clear();
  channelsByName.clear();
  pendingSeverity.clear();
  initialAckWritten.clear();
  clearing = false;
}
void ChannelAccess::poll() {
  if (!active || dispatching || clearing)
    return;
  dispatching = true;
  ca_poll();
  auto now = monotonicNow();
  for (auto& c : channels) {
    if (!c->recheckAccess || c->cancelled)
      continue;
    c->recheckAccess = false;
    if (c->id && ca_state(c->id) == cs_conn) {
      // Cancel invalidates haveLast; healthy recovery must use fresh IOC data.
      // Keeping this work on the channel also makes cancel/clear discard it.
      access_rights_handler_args rights{};
      rights.chid = c->id;
      access(rights);
    }
  }
  // CA does not send an initial disconnect callback for a PV that never existed.
  for (auto& c : channels)
    if (!c->cancelled && c->connectionDeadline && now >= c->connectionDeadline) {
      c->connectionDeadline = 0;
      if (c->id && ca_state(c->id) == cs_conn)
        continue;
      if (c->alarm)
        c->alarm({ALARM_NSTATUS, 4, 0, -1, "0"});
      numericUnavailable(c.get(), "PV not connected");
    }
  dispatching = false;
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
