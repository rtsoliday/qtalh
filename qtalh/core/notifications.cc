#include "notifications.h"
#include <QDateTime>
#include <QTimeZone>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <stdexcept>
namespace alh {
namespace {
constexpr auto sep = '\x1f';
QString token(const QString& a, const QString& b) { return a + sep + b; }
// PV wildcards describe names, not filesystem paths. Keep separators literal
// and let * and ? span them on every platform (including Qt 5).
QString pvWildcard(const QString& pattern) {
  QString regex;
  for (int i = 0; i < pattern.size(); ++i) {
    const auto c = pattern[i];
    if (c == '*')
      regex += ".*";
    else if (c == '?')
      regex += '.';
    else if (c == '[') {
      int start = i + 1;
      const bool negate = start < pattern.size() && pattern[start] == '!';
      if (negate)
        ++start;
      int end = start;
      if (end < pattern.size() && pattern[end] == ']')
        ++end;
      while (end < pattern.size() && pattern[end] != ']')
        ++end;
      if (end == pattern.size()) {
        regex += "\\[";
        continue;
      }
      regex += negate ? "[^" : "[";
      for (int j = start; j < end; ++j) {
        if (pattern[j] == '\\' || pattern[j] == '^' || pattern[j] == ']') regex += '\\';
        regex += pattern[j];
      }
      regex += ']';
      i = end;
    } else
      regex += QRegularExpression::escape(QString(c));
  }
  return QRegularExpression::anchoredPattern(regex);
}
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
QJsonArray strings(const QStringList& list) { return QJsonArray::fromStringList(list); }
QStringList list(const QJsonValue& v) {
  QStringList r;
  for (auto x : v.toArray())
    r << x.toString();
  return r;
}
QJsonObject destinationJson(const NotificationDestination& d) {
  return {{"id", d.id},
          {"name", d.name},
          {"kind", d.kind},
          {"program", d.program},
          {"arguments", strings(d.arguments)},
          {"from", d.sender},
          {"recipients", strings(d.recipients)},
          {"url", d.url},
          {"urlEnvironment", d.urlEnvironment},
          {"bearerEnvironment", d.bearerEnvironment}};
}
QJsonObject ruleJson(const NotificationSubscription& r) {
  QJsonArray stages;
  for (const auto& s : r.stages)
    stages.append(QJsonObject{
        {"id", s.id}, {"delaySeconds", s.delaySeconds}, {"destinations", strings(s.destinations)}});
  return {{"id", r.id},
          {"name", r.name},
          {"configuration", r.configuration},
          {"scope", r.scope},
          {"wildcard", r.wildcard},
          {"minimumSeverity", r.minimumSeverity},
          {"cooldownSeconds", r.cooldownSeconds},
          {"enabled", r.enabled},
          {"resolution", r.resolution},
          {"stages", stages}};
}
QByteArray signature(const NotificationSubscription& r, const NotificationSettings& s) {
  QJsonArray a;
  a.append(ruleJson(r));
  for (const auto& stage : r.stages)
    for (const auto& id : stage.destinations)
      for (const auto& d : s.destinations)
        if (d.id == id)
          a.append(destinationJson(d));
  return QJsonDocument(a).toJson(QJsonDocument::Compact);
}
} // namespace
QString notificationId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString notificationConfiguration(const QString& file) {
  if (file.isEmpty())
    return {};
  QFileInfo f(file);
  return f.canonicalFilePath().isEmpty() ? f.absoluteFilePath() : f.canonicalFilePath();
}
QJsonObject notificationJson(const NotificationSettings& s) {
  QJsonArray d, r;
  for (const auto& x : s.destinations)
    d.append(destinationJson(x));
  for (const auto& x : s.subscriptions)
    r.append(ruleJson(x));
  return {{"version", 1}, {"destinations", d}, {"subscriptions", r}};
}
NotificationSettings parseNotifications(const QByteArray& bytes) {
  QJsonParseError e;
  auto doc = QJsonDocument::fromJson(bytes, &e);
  require(e.error == QJsonParseError::NoError && doc.isObject(), "Invalid notification JSON");
  auto o = doc.object();
  require(o.value("version").toInt() == 1, "Unsupported notification settings version");
  require(o.value("destinations").isArray() && o.value("subscriptions").isArray(),
          "Missing notification settings arrays");
  NotificationSettings s;
  for (auto v : o.value("destinations").toArray()) {
    auto x = v.toObject();
    NotificationDestination d;
    d.id = x.value("id").toString();
    d.name = x.value("name").toString();
    d.kind = x.value("kind").toString();
    d.program = x.value("program").toString();
    d.arguments = list(x.value("arguments"));
    d.sender = x.value("from").toString();
    d.recipients = list(x.value("recipients"));
    d.url = x.value("url").toString();
    d.urlEnvironment = x.value("urlEnvironment").toString();
    d.bearerEnvironment = x.value("bearerEnvironment").toString();
    s.destinations << d;
  }
  for (auto v : o.value("subscriptions").toArray()) {
    auto x = v.toObject();
    NotificationSubscription r;
    r.id = x.value("id").toString();
    r.name = x.value("name").toString();
    r.configuration = x.value("configuration").toString();
    r.scope = x.value("scope").toString();
    r.wildcard = x.value("wildcard").toString("*");
    r.minimumSeverity = x.value("minimumSeverity").toInt(2);
    r.cooldownSeconds = x.value("cooldownSeconds").toInt(1800);
    r.enabled = x.value("enabled").toBool(true);
    r.resolution = x.value("resolution").toBool(false);
    for (auto vstage : x.value("stages").toArray()) {
      auto xstage = vstage.toObject();
      r.stages << NotificationStage{xstage.value("id").toString(),
                                    xstage.value("delaySeconds").toInt(-1),
                                    list(xstage.value("destinations"))};
    }
    s.subscriptions << r;
  }
  validateNotifications(s);
  return s;
}
void validateNotifications(const NotificationSettings& s) {
  QSet<QString> ids;
  auto validId = [](const QString& v) { return !v.isEmpty() && !v.contains(sep); };
  auto header = [](const QString& v) {
    return !v.contains('\r') && !v.contains('\n') && !v.contains(QChar(0));
  };
  for (const auto& d : s.destinations) {
    require(validId(d.id) && !ids.contains(d.id), "Destination IDs must be unique");
    ids << d.id;
    require(!d.name.trimmed().isEmpty() && d.name.size() <= 120 && header(d.name),
            "Destination name must be a single line of at most 120 characters");
    require(d.kind == "email" || d.kind == "webhook", "Unknown destination type");
    if (d.kind == "email") {
      require(!d.program.isEmpty(), "Choose a sendmail-compatible executable");
      require(!d.sender.isEmpty() && header(d.sender) && !d.recipients.isEmpty(),
              "Email requires a valid From and recipients");
      for (const auto& r : d.recipients)
        require(!r.trimmed().isEmpty() && header(r), "Invalid email recipient header");
    } else
      require(d.url.isEmpty() != d.urlEnvironment.isEmpty(),
              "Specify a webhook URL OR URL environment variable");
    for (const auto& env : {d.urlEnvironment, d.bearerEnvironment})
      require(env.isEmpty() || QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(env).hasMatch(),
              "Invalid environment variable name");
  }
  QSet<QString> rules;
  for (const auto& r : s.subscriptions) {
    require(validId(r.id) && !rules.contains(r.id), "Subscription IDs must be unique");
    rules << r.id;
    require(!r.name.trimmed().isEmpty() && r.name.size() <= 120 && header(r.name) &&
                !r.configuration.isEmpty(),
            "Subscription name and saved configuration are required");
    require(r.minimumSeverity >= 1 && r.minimumSeverity <= 4 && r.cooldownSeconds >= 0,
            "Invalid subscription severity or cooldown");
    require(!r.stages.isEmpty(), "A subscription needs an initial stage");
    QSet<QString> stages;
    int previous = -1;
    for (const auto& stage : r.stages) {
      require(validId(stage.id) && !stages.contains(stage.id) && stage.id != "resolution",
              "Stage IDs must be unique");
      stages << stage.id;
      require(stage.delaySeconds >= 0 && stage.delaySeconds > previous,
              "Stage delays must increase from episode start");
      previous = stage.delaySeconds;
      require(!stage.destinations.isEmpty(), "Each stage needs a destination");
      QSet<QString> seen;
      for (const auto& id : stage.destinations) {
        require(ids.contains(id) && !seen.contains(id), "Unknown or duplicate stage destination");
        seen << id;
      }
    }
  }
}
QByteArray notificationPayload(const NotificationEnvelope& e) {
  QJsonArray alarms;
  for (const auto& a : e.alarms) {
    const auto& s = a.state;
    alarms.append(QJsonObject{
        {"episodeId", a.episode},
        {"channel", s.pv},
        {"path", s.path},
        {"value", s.value},
        {"severity", s.severity},
        {"status", s.status},
        {"unacknowledgedSeverity", s.unack},
        {"observedAt",
         QDateTime::fromMSecsSinceEpoch(s.observedAt, QTimeZone(0)).toUTC().toString(Qt::ISODateWithMs)}});
  }
  return QJsonDocument(
             QJsonObject{{"version", 1},
                         {"messageId", e.id},
                         {"configuration", e.configuration},
                         {"subscription", e.subscriptionName},
                         {"subscriptionId", e.subscription},
                         {"kind", e.kind},
                         {"stage", e.stage},
                         {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                         {"alarms", alarms}})
      .toJson(QJsonDocument::Compact);
}
QString notificationText(const NotificationEnvelope& e) {
  QString text = QString("QtALH %1: %2\nConfiguration: %3\nStage: %4\nMessage ID: %5\n\n")
                     .arg(e.kind, e.subscriptionName, e.configuration, e.stage, e.id);
  for (const auto& a : e.alarms) {
    const auto& s = a.state;
    text += QString("%1 (%2)\n  Value: %3; severity: %4; status: %5; unacknowledged severity: %6\n")
                .arg(s.path, s.pv, s.value)
                .arg(s.severity)
                .arg(s.status)
                .arg(s.unack);
    if (e.kind == "resolution")
      text += s.severity ? "  Episode acknowledged; alarm was still active at acknowledgement.\n"
                         : "  Alarm cleared / acknowledged for this episode.\n";
  }
  return text;
}
const NotificationSubscription* NotificationPolicy::subscription(const QString& id) const {
  for (const auto& r : settings_.subscriptions)
    if (r.id == id)
      return &r;
  return nullptr;
}
bool NotificationPolicy::match(const NotificationSubscription& r, const ChannelUpdate& s) const {
  return r.enabled && r.configuration == configuration_ &&
         (r.scope.isEmpty() || s.ancestors.contains(r.scope)) &&
         QRegularExpression(pvWildcard(r.wildcard))
             .match(s.pv)
             .hasMatch();
}
int NotificationPolicy::matchCount(const NotificationSubscription& r,
                                   const QVector<ChannelUpdate>& all) const {
  int n = 0;
  auto copy = r;
  copy.enabled = true;
  for (const auto& s : all)
    if (match(copy, s))
      ++n;
  return n;
}
void NotificationPolicy::configure(const NotificationSettings& s, const QString& configuration) {
  validateNotifications(s);
  QSet<QString> keep;
  for (const auto& r : s.subscriptions)
    if (auto old = subscription(r.id))
      if (configuration_ == configuration && signature(*old, settings_) == signature(r, s))
        keep << r.id;
  for (auto it = episodes.begin(); it != episodes.end();)
    if (!keep.contains(it->subscription))
      it = episodes.erase(it);
    else
      ++it;
  for (auto it = current.begin(); it != current.end();)
    if (!episodes.contains(it.value()))
      it = current.erase(it);
    else
      ++it;
  settings_ = s;
  configuration_ = configuration;
  matches.clear();
  batchDeadline.clear();
  for (int i = queue.size() - 1; i >= 0; --i)
    if (!keep.contains(queue[i].subscription))
      queue.removeAt(i);
}
void NotificationPolicy::enable(bool on) {
  enabled_ = on;
  if (!on) {
    episodes.clear();
    current.clear();
    queue.clear();
    throttled_ = false;
    batchDeadline.clear();
  }
}
void NotificationPolicy::end(Episode& e, bool resolved) {
  e.active = false;
  e.resolved = resolved;
  current.remove(token(e.subscription, e.state.identity));
}
void NotificationPolicy::observe(const ChannelUpdate& s, qint64 now) {
  // Ended episodes are absent from current. Cancel their resolutions when
  // suppression arrives, even if it clears before the transport pump can run.
  if (s.suppressed && !channels.value(s.identity).suppressed)
    for (auto& episode : episodes)
      if (!episode.active && episode.state.identity == s.identity)
        episode.resolved = false;
  channels[s.identity] = s;
  if (!enabled_)
    return;
  if (!matches.contains(s.identity)) {
    QStringList ids;
    for (const auto& r : settings_.subscriptions)
      if (match(r, s))
        ids << r.id;
    matches[s.identity] = ids.toVector();
  }
  for (const auto& id : matches[s.identity]) {
    auto r = subscription(id);
    if (!r)
      continue;
    QString key = token(id, s.identity);
    auto it = episodes.find(current.value(key));
    bool eligible = s.initialized && !s.suppressed && s.unack >= r->minimumSeverity;
    if (it != episodes.end()) {
      it->state = s;
      // Global disconnect/access errors carry ACKS=0. Keep the episode and
      // its escalation clock, but wait for a fresh state before dispatching.
      if (!s.initialized || (!s.available && !s.suppressed && s.unack == 0)) {
        it->waiting = true;
        continue;
      }
      it->waiting = false;
      if (!eligible)
        end(*it, s.available && !s.suppressed && s.unack == 0);
    } else if (eligible) {
      Episode e;
      e.id = notificationId();
      e.subscription = id;
      e.state = s;
      e.started = now;
      episodes[e.id] = e;
      current[key] = e.id;
    }
  }
}
void NotificationPolicy::reconcile(const QVector<ChannelUpdate>& all) {
  QHash<QString, int> counts;
  for (const auto& s : all)
    ++counts[s.identity];
  channels.clear();
  matches.clear();
  for (const auto& s : all)
    if (counts[s.identity] == 1)
      channels[s.identity] = s;
  for (auto it = episodes.begin(); it != episodes.end(); ++it) {
    if (!channels.contains(it->state.identity))
      end(*it, false);
    else if (it->active) {
      it->waiting = true;
      it->state = channels[it->state.identity];
    }
  }
}
void NotificationPolicy::tick(qint64 now, int queueLimit) {
  if (!enabled_)
    return;
  struct Group {
    NotificationEnvelope envelope;
  };
  QMap<QString, Group> groups;
  for (auto it = episodes.begin(); it != episodes.end(); ++it) {
    auto& e = *it;
    if (!e.active && channels.value(e.state.identity).suppressed)
      e.resolved = false;
    auto r = subscription(e.subscription);
    if (!r)
      continue;
    auto add = [&](const QString& stage, const QString& dest, bool resolution, int delay) {
      auto t = token(stage, dest);
      if (resolution ? (e.resolutionDone.contains(dest) || e.resolutionQueued.contains(dest))
                     : (e.done.contains(t) || e.queued.contains(t)))
        return;
      if (!resolution && cooldown.value(token(token(r->id, e.state.identity), t), 0) > now)
        return;
      QString gkey =
          token(token(token(r->id, QString::number(delay).rightJustified(12, '0')), stage), dest);
      auto& g = groups[gkey].envelope;
      g.subscription = r->id;
      g.subscriptionName = r->name;
      g.configuration = configuration_;
      g.stage = stage;
      g.destination = dest;
      g.kind = resolution ? "resolution" : "alarm";
      g.alarms << NotificationAlarm{e.id, e.state};
    };
    if (e.active && !e.waiting) {
      for (const auto& stage : r->stages)
        if (now - e.started >= qint64(stage.delaySeconds) * 1000)
          for (const auto& dest : stage.destinations)
            add(stage.id, dest, false, stage.delaySeconds);
    } else if (!e.active && e.resolved && r->resolution && channels.contains(e.state.identity) &&
               channels[e.state.identity].initialized && !channels[e.state.identity].suppressed)
      for (const auto& dest : e.accepted)
        add("resolution", dest, true, 0);
  }
  const bool wasThrottled = throttled_;
  throttled_ = false;
  QSet<QString> live;
  for (auto it = groups.begin(); it != groups.end(); ++it) {
    live << it.key();
    if (!batchDeadline.contains(it.key()))
      batchDeadline[it.key()] = now + 10000;
    if (now < batchDeadline[it.key()])
      continue;
    auto source = it->envelope;
    int pos = 0;
    while (pos < source.alarms.size() && queue.size() < queueLimit) {
      auto e = source;
      e.id = notificationId();
      e.alarms.clear();
      const int overhead = notificationPayload(e).size();
      int payloadSize = overhead;
      while (pos < source.alarms.size() && e.alarms.size() < 100) {
        auto a = source.alarms[pos]; // Bound individual values so one PV cannot exhaust the queue.
        a.state.value = a.state.value.left(4096);
        auto single = e;
        single.alarms = {a};
        const int alarmSize = notificationPayload(single).size() - overhead + 1;
        if (payloadSize + alarmSize > 256 * 1024) {
          if (e.alarms.isEmpty()) {
            ++pos;
            if (activity)
              activity("Failed: alarm exceeds envelope size limit");
            auto& episode = episodes[a.episode];
            if (e.kind == "resolution")
              episode.resolutionDone << e.destination;
            else
              episode.done << token(e.stage, e.destination);
          }
          break;
        }
        payloadSize += alarmSize;
        e.alarms << a;
        ++pos;
      }
      if (e.alarms.isEmpty())
        continue;
      for (const auto& a : e.alarms) {
        auto& episode = episodes[a.episode];
        if (e.kind == "resolution")
          episode.resolutionQueued << e.destination;
        else
          episode.queued << token(e.stage, e.destination);
      }
      queue << e;
      if (activity)
        activity("Queued: " + e.subscriptionName + " (" + QString::number(e.alarms.size()) +
                 " alarms)");
    }
    if (pos < source.alarms.size())
      throttled_ = true;
  }
  if (throttled_ && !wasThrottled && activity)
    activity("Throttled: queue capacity reached; due alarms retained");
  for (auto it = batchDeadline.begin(); it != batchDeadline.end();)
    if (!live.contains(it.key()))
      it = batchDeadline.erase(it);
    else
      ++it;
  collect();
}
bool NotificationPolicy::revalidate(NotificationEnvelope& e) {
  if (e.test)
    return true;
  if (!enabled_)
    return false;
  for (int i = e.alarms.size() - 1; i >= 0; --i) {
    auto it = episodes.find(e.alarms[i].episode);
    bool valid =
        it != episodes.end() &&
        (e.kind == "resolution"
             ? (!it->active && it->resolved && it->accepted.contains(e.destination) &&
                channels.contains(it->state.identity) && !channels[it->state.identity].suppressed)
             : it->active);
    if (!valid) {
      if (it != episodes.end()) {
        it->queued.remove(token(e.stage, e.destination));
        it->resolutionQueued.remove(e.destination);
      }
      e.alarms.removeAt(i);
    } else {
      // A resolution belongs to the ended episode, even if the PV has since
      // entered another alarm episode or been reloaded.
      auto updated = it->state;
      // Keep the queued value snapshot; state and acknowledgement are revalidated.
      updated.value = e.alarms[i].state.value;
      e.alarms[i].state = updated;
    }
  }
  return !e.alarms.isEmpty();
}
bool NotificationPolicy::take(NotificationEnvelope& e, qint64 now) {
  for (int i = 0; i < queue.size();) {
    if (!revalidate(queue[i])) {
      if (activity)
        activity("Cancelled: " + queue[i].subscriptionName);
      queue.removeAt(i);
      continue;
    }
    bool waiting = false;
    for (const auto& a : queue[i].alarms)
      if (!channels.value(a.state.identity).initialized ||
          (queue[i].kind != "resolution" && episodes.value(a.episode).waiting))
        waiting = true;
    if (!waiting && queue[i].ready <= now &&
        destinationNext.value(queue[i].destination, 0) <= now) {
      e = queue.takeAt(i);
      return true;
    }
    ++i;
  }
  return false;
}
void NotificationPolicy::attempted(const NotificationEnvelope& e, qint64 now) {
  destinationNext[e.destination] = now + 10000;
  if (e.attempts > 1 || e.kind == "resolution")
    return;
  auto r = subscription(e.subscription);
  if (!r)
    return;
  for (const auto& a : e.alarms)
    cooldown[token(token(e.subscription, a.state.identity), token(e.stage, e.destination))] =
        now + qint64(r->cooldownSeconds) * 1000;
}
void NotificationPolicy::completed(const NotificationEnvelope& e, bool accepted) {
  for (const auto& a : e.alarms) {
    auto it = episodes.find(a.episode);
    if (it == episodes.end())
      continue;
    if (e.kind == "resolution") {
      it->resolutionQueued.remove(e.destination);
      it->resolutionDone << e.destination;
    } else {
      auto t = token(e.stage, e.destination);
      it->queued.remove(t);
      it->done << t;
      if (accepted)
        it->accepted << e.destination;
    }
  }
  collect();
}
void NotificationPolicy::retry(NotificationEnvelope e) { queue << e; }
QSet<QString> NotificationPolicy::activeChannels() const {
  QSet<QString> s;
  for (const auto& e : episodes)
    if (e.active)
      s << e.state.identity;
  return s;
}
void NotificationPolicy::collect() {
  for (auto it = episodes.begin(); it != episodes.end();) {
    auto r = subscription(it->subscription);
    bool resolution =
        it->resolved && r && r->resolution && (it->accepted - it->resolutionDone).size();
    if (!it->active && it->queued.isEmpty() && it->resolutionQueued.isEmpty() && !resolution)
      it = episodes.erase(it);
    else
      ++it;
  }
}
} // namespace alh
