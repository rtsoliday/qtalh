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
  p->connectionDeadline = QDateTime::currentMSecsSinceEpoch() + 1000;
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
  for (auto& channel : channels)
    if (channel->name == n && channel->tag == tag && channel->cancelled && channel->alarm) {
      c = channel.get();
      break;
    }
  if (!c)
    c = add(n);
  c->cancelled = false;
  c->tag = tag;
  c->alarm = std::move(callback);
  // A disconnect while cancelled was deliberately ignored. Re-arm the startup
  // check when reusing that channel, even if it connected successfully earlier.
  if (!c->id || ca_state(c->id) != cs_conn)
    c->connectionDeadline = QDateTime::currentMSecsSinceEpoch() + 1000;
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
  Channel* c = nullptr;
  for (auto& channel : channels)
    if (channel->name == n && channel->tag == tag && channel->cancelled && channel->numeric) {
      c = channel.get();
      break;
    }
  if (!c)
    c = add(n);
  c->cancelled = false;
  c->numeric = std::move(callback);
  c->tag = tag;
  c->numericError.clear();
  if (!c->id || ca_state(c->id) != cs_conn)
    c->connectionDeadline = QDateTime::currentMSecsSinceEpoch() + 1000;
  subscribe(c);
  ca_flush_io();
}
void ChannelAccess::prepare(const QString& n) {
  for (auto& c : channels)
    if (c->name == n && !c->cancelled)
      return;
  add(n);
  ca_flush_io();
}
void ChannelAccess::setInitialAckT(const QString& n, bool v) {
  initialAckT[n] = v;
}
void ChannelAccess::subscribe(Channel* c) {
  if (!c->id || c->cancelled || c->subscription || ca_state(c->id) != cs_conn ||
      (!c->alarm && !c->numeric && !c->text))
    return;
  chtype type = c->numeric ? DBR_DOUBLE : c->text ? DBR_STRING : DBR_STSACK_STRING;
  report(ca_create_subscription(type, 1, c->id, DBE_VALUE | DBE_ALARM, update, c, &c->subscription),
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
    if (c->alarm)
      c->alarm({ALARM_NSTATUS, 4, 0, -1, "0"});
    c->owner->numericUnavailable(c, "PV not connected");
  }
}
void ChannelAccess::access(access_rights_handler_args a) {
  auto c = static_cast<Channel*>(ca_puser(a.chid));
  if (!c || c->owner->clearing || c->cancelled || ca_state(a.chid) != cs_conn)
    return;
  if (c->owner->options.debug)
    debugLog(true, "CA", QString("%1 access read=%2 write=%3").arg(c->name)
        .arg(ca_read_access(a.chid)).arg(ca_write_access(a.chid)));
  if (!ca_read_access(a.chid))
    c->owner->numericUnavailable(c, "No read access for PV");
  if (c->alarm) {
    if (!ca_read_access(a.chid))
      c->alarm({ALARM_NSTATUS + 1, 4, 0, -1, "0"});
    else if (c->owner->options.global && !c->owner->options.passive && !ca_write_access(a.chid))
      c->alarm({ALARM_NSTATUS + 2, 4, 0, -1, "0"});
    else if (c->haveLast)
      c->alarm(c->last);
  }
  // Retry a startup setting when write access arrives after connection. Once
  // applied, never overwrite subsequent operator/IOC changes on access recovery.
  auto& o = c->owner->options;
  if (o.caputAckT && o.global && !o.passive && !c->initialAckWritten && c->alarm &&
      c->owner->initialAckT.contains(c->name) && ca_write_access(c->id)) {
    c->initialAckWritten =
        c->owner->put(c->name, c->owner->initialAckT[c->name], WriteKind::AckTransient);
  }
  // Connection-up also dispatches this path. Coalesce failed severity writes
  // by PV name, and replay only when connection and write access are available.
  if (ca_write_access(a.chid) && c->owner->pendingSeverity.contains(c->name))
    c->owner->put(c->name, c->owner->pendingSeverity.value(c->name), WriteKind::Severity);
}
void ChannelAccess::update(event_handler_args a) {
  auto c = static_cast<Channel*>(a.usr);
  if (!c || c->owner->clearing || c->cancelled)
    return;
  if (a.status != ECA_NORMAL || !a.dbr) {
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
  Event e{qBound(0, int(d->status), ALARM_NSTATUS - 1), qBound(0, int(d->severity), 3),
          qBound(0, int(d->acks), 3), int(d->ackt),
          QString::fromLocal8Bit(d->value, int(strnlen(d->value, MAX_STRING_SIZE)))};
  c->last = e;
  c->haveLast = true;
  if (!ca_read_access(c->id))
    e = {ALARM_NSTATUS + 1, 4, 0, -1, "0"};
  else if (c->owner->options.global && !c->owner->options.passive && !ca_write_access(c->id))
    e = {ALARM_NSTATUS + 2, 4, 0, -1, "0"};
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
      if (kind == WriteKind::Value || kind == WriteKind::Severity)
        status = ca_put(DBR_DOUBLE, c->id, &v);
      else {
        unsigned short s = static_cast<unsigned short>(v);
        status = ca_put(kind == WriteKind::Acknowledge ? DBR_PUT_ACKS : DBR_PUT_ACKT, c->id, &s);
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
      if (c->subscription) {
        ca_clear_subscription(c->subscription);
        c->subscription = nullptr;
      }
    }
}
void ChannelAccess::cancelNumbers(const void* tag) {
  for (auto& c : channels)
    if (c->numeric && c->tag == tag) {
      c->cancelled = true;
      if (c->subscription) {
        ca_clear_subscription(c->subscription);
        c->subscription = nullptr;
      }
    }
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
  clearing = false;
}
void ChannelAccess::poll() {
  if (!active || dispatching || clearing)
    return;
  dispatching = true;
  ca_poll();
  auto now = QDateTime::currentMSecsSinceEpoch();
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
