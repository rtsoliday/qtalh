#include "notifications.h"
#include <QDateTime>
#include <QTimeZone>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <algorithm>
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
// Engine paths escape literal slashes and backslashes in group/channel names.
// Decode one component at a time so those characters cannot create extra groups.
QStringList pathComponents(const QString& path) {
  QStringList parts;
  QString part;
  for (int i = path.startsWith('/') ? 1 : 0; i < path.size(); ++i) {
    const auto c = path[i];
    if (c == '\\' && i + 1 < path.size() && (path[i + 1] == '/' || path[i + 1] == '\\'))
      part += path[++i];
    else if (c == '/') {
      parts << part;
      part.clear();
    } else
      part += c;
  }
  parts << part;
  return parts;
}
struct NotificationTextNode {
  QString name;
  const NotificationAlarm* alarm = nullptr;
  QVector<NotificationTextNode> children;
};
QString namedValue(const QString& name, int value) {
  const auto number = QString::number(value);
  return name == number ? number : name + " (" + number + ")";
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
    require(QRegularExpression(pvWildcard(r.wildcard)).isValid(),
            "Invalid notification PV wildcard");
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
namespace {
QStringList directiveWords(const QString& value) {
  return value.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}
bool directiveId(const QString& id) {
  static const QRegularExpression valid("^[A-Za-z0-9_.-]{1,64}$");
  return valid.match(id).hasMatch();
}
bool environmentName(const QString& name) {
  static const QRegularExpression valid("^[A-Za-z_][A-Za-z0-9_]*$");
  return valid.match(name).hasMatch();
}
// One parsed $NOTIFY value: severity, stages and options.
struct NotifyDirective {
  int severity = 2, cooldown = 1800;
  bool resolution = false;
  QString name, wildcard = "*";
  QVector<NotificationStage> stages;
};
NotifyDirective parseNotify(const QString& value) {
  const auto words = directiveWords(value);
  if (words.size() < 2)
    throw ParseError("$NOTIFY needs a minimum severity and at least one destination");
  NotifyDirective result;
  try {
    result.severity = severityValue(words[0]);
  } catch (const ParseError&) {
    result.severity = 0;
  }
  if (result.severity < 1)
    throw ParseError("$NOTIFY severity must be MINOR, MAJOR, INVALID or ERROR");
  for (int i = 1; i < words.size(); ++i) {
    const auto& word = words[i];
    if (word == "resolution") {
      result.resolution = true;
    } else if (word.startsWith("pv=")) {
      result.wildcard = word.mid(3);
      if (result.wildcard.isEmpty()) throw ParseError("$NOTIFY pv= needs a PV wildcard");
    } else if (word.startsWith("cooldown=")) {
      bool ok = false;
      result.cooldown = word.mid(9).toInt(&ok);
      if (!ok || result.cooldown < 0) throw ParseError("$NOTIFY cooldown= needs whole seconds");
    } else if (word.startsWith("name=")) {
      result.name = word.mid(5);
      if (result.name.isEmpty() || result.name.size() > 120)
        throw ParseError("$NOTIFY name= needs 1 to 120 characters");
    } else if (word.contains('=')) {
      throw ParseError("Unknown $NOTIFY option " + word);
    } else {
      // [delay:]destination[,destination...]; only the first stage may omit its delay.
      NotificationStage stage;
      stage.id = QString("s%1").arg(result.stages.size() + 1);
      QString targets = word;
      const int colon = word.indexOf(':');
      if (colon >= 0) {
        bool ok = false;
        stage.delaySeconds = word.left(colon).toInt(&ok);
        if (!ok || stage.delaySeconds < 0) throw ParseError("$NOTIFY stage delay must be whole seconds: " + word);
        targets = word.mid(colon + 1);
      } else if (!result.stages.isEmpty()) {
        throw ParseError("$NOTIFY escalation stages need a delay, as in 600:pager");
      } else {
        stage.delaySeconds = 0;
      }
      if (!result.stages.isEmpty() && stage.delaySeconds <= result.stages.last().delaySeconds)
        throw ParseError("$NOTIFY stage delays must increase");
      for (const auto& target : targets.split(',')) {
        if (!directiveId(target)) throw ParseError("Invalid $NOTIFY destination name: " + target);
        if (stage.destinations.contains(target)) throw ParseError("Repeated $NOTIFY destination: " + target);
        stage.destinations << target;
      }
      result.stages << stage;
    }
  }
  if (result.stages.isEmpty()) throw ParseError("$NOTIFY needs at least one destination");
  if (!QRegularExpression(pvWildcard(result.wildcard)).isValid())
    throw ParseError("$NOTIFY pv= has an invalid wildcard");
  return result;
}
NotificationDestination parseDestination(const QString& name, const QString& value) {
  const auto words = directiveWords(value);
  const bool email = name == "NOTIFY_EMAIL";
  if (words.size() < 2)
    throw ParseError(email ? "$NOTIFY_EMAIL needs a name and recipients"
                           : "$NOTIFY_WEBHOOK needs a name and a URL or env:VARIABLE");
  NotificationDestination d;
  d.id = words[0];
  if (!directiveId(d.id)) throw ParseError("Invalid notification destination name: " + d.id);
  d.name = d.id;
  d.kind = email ? "email" : "webhook";
  if (email) {
    for (const auto& recipient : words[1].split(','))
      if (recipient.isEmpty()) throw ParseError("$NOTIFY_EMAIL has an empty recipient");
      else d.recipients << recipient;
  } else if (words[1].startsWith("env:")) {
    d.urlEnvironment = words[1].mid(4);
    if (!environmentName(d.urlEnvironment)) throw ParseError("Invalid environment variable: " + words[1]);
  } else if (words[1].startsWith("https://") || words[1].startsWith("http://")) {
    // Refuse at load what every delivery would refuse; env: URLs are checked then.
    const auto problem = webhookUrlProblem(words[1]);
    if (!problem.isEmpty()) throw ParseError("$NOTIFY_WEBHOOK " + words[1] + ": " + problem);
    d.url = words[1];
  } else {
    throw ParseError("$NOTIFY_WEBHOOK needs an http(s) URL or env:VARIABLE");
  }
  for (int i = 2; i < words.size(); ++i) {
    const auto& word = words[i];
    if (email && word.startsWith("from=") && word.size() > 5) d.sender = word.mid(5);
    else if (email && word.startsWith("program=") && word.size() > 8) d.program = word.mid(8);
    else if (!email && word.startsWith("bearer=env:") && environmentName(word.mid(11)))
      d.bearerEnvironment = word.mid(11);
    else throw ParseError("Unknown or invalid " + name + " option " + word);
  }
  return d;
}
} // namespace
QString webhookUrlProblem(const QString& text) {
  const QUrl url(text);
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment())
    return "Invalid webhook URL (credentials and fragments are not allowed)";
  const auto host = url.host().toLower();
  const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
  if (url.scheme() != "https" && !(url.scheme() == "http" && loopback))
    return "Webhook requires HTTPS (HTTP is allowed only for loopback testing)";
  return {};
}
bool isNotificationDirective(const QString& name) {
  return name == "NOTIFY" || name == "NOTIFY_EMAIL" || name == "NOTIFY_WEBHOOK";
}
void checkNotificationDirective(const QString& name, const QString& value) {
  if (name == "NOTIFY") parseNotify(value);
  else parseDestination(name, value);
}
QStringList notificationDestinationIds(const QString& value) {
  QStringList result;
  for (const auto& stage : parseNotify(value).stages)
    for (const auto& id : stage.destinations)
      if (!result.contains(id)) result << id;
  return result;
}
NotificationSettings configurationNotifications(const Document& doc, const QString& configuration,
                                                const QString& sender, const QString& program) {
  NotificationSettings result;
  if (!doc.root) return result;
  QSet<QString> destinations;
  for (auto n : doc.nodes())
    for (const auto& d : n->directives)
      if (d.key == "NOTIFY_EMAIL" || d.key == "NOTIFY_WEBHOOK") {
        auto destination = parseDestination(d.key, d.value);
        if (destinations.contains(destination.id))
          throw ParseError("Notification destination " + destination.id + " is defined twice");
        destinations << destination.id;
        destination.id = "file:" + destination.id;
        if (destination.kind == "email") {
          if (destination.sender.isEmpty()) destination.sender = sender;
          if (destination.program.isEmpty()) destination.program = program;
        }
        result.destinations << destination;
      }
  for (auto n : doc.nodes()) {
    int count = 0;
    for (const auto& d : n->directives) {
      if (d.key != "NOTIFY") continue;
      const auto parsed = parseNotify(d.value);
      NotificationSubscription r;
      // Stable across reloads, so running episodes survive unchanged subscriptions.
      r.id = "file:" + Engine::nodeIdentity(n) + '#' + QString::number(++count);
      r.name = parsed.name.isEmpty() ? n->label().left(120) : parsed.name;
      r.configuration = configuration;
      r.scope = Engine::nodeIdentity(n);
      r.wildcard = parsed.wildcard;
      r.minimumSeverity = parsed.severity;
      r.cooldownSeconds = parsed.cooldown;
      r.resolution = parsed.resolution;
      r.stages = parsed.stages;
      for (auto& stage : r.stages)
        for (auto& target : stage.destinations) {
          if (!destinations.contains(target))
            throw ParseError("$NOTIFY on " + n->name + " uses undefined destination " + target);
          target = "file:" + target;
        }
      result.subscriptions << r;
    }
  }
  return result;
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
  // Append user/configuration values literally. QString::arg applied repeatedly
  // can interpret placeholders inside names or PV values as format directives.
  QString text = "QtALH " + e.kind + ": " + e.subscriptionName +
                 "\nConfiguration: " + e.configuration + "\n\n";
  NotificationTextNode root;
  for (const auto& a : e.alarms) {
    auto parts = a.state.path.isEmpty() ? QStringList{a.state.pv} : pathComponents(a.state.path);
    auto* parent = &root;
    for (int i = 0; i + 1 < parts.size(); ++i) {
      auto child = std::find_if(parent->children.begin(), parent->children.end(),
                                [&](const NotificationTextNode& n) {
                                  return !n.alarm && n.name == parts[i];
                                });
      if (child == parent->children.end()) {
        parent->children.append(NotificationTextNode{parts[i], nullptr, {}});
        parent = &parent->children.last();
      } else
        parent = &*child;
    }
    parent->children.append(NotificationTextNode{parts.last(), &a, {}});
  }
  std::function<void(const NotificationTextNode&, int)> append =
      [&](const NotificationTextNode& node, int depth) {
        const QString indent(depth * 2, ' ');
        text += indent + node.name;
        if (!node.alarm) {
          text += ":\n";
          for (const auto& child : node.children)
            append(child, depth + 1);
          return;
        }
        const auto& s = node.alarm->state;
        if (node.name != s.pv)
          text += " (" + s.pv + ")";
        text += "\n" + indent + "  Value: " + s.value +
                "; severity: " + namedValue(severityName(s.severity), s.severity) +
                "; status: " + namedValue(statusName(s.status), s.status) +
                "; unacknowledged severity: " + namedValue(severityName(s.unack), s.unack) + "\n";
        if (e.kind == "resolution")
          text += indent + (s.severity
                      ? "  Episode acknowledged; alarm was still active at acknowledgement.\n"
                      : "  Alarm cleared / acknowledged for this episode.\n");
      };
  for (const auto& child : root.children)
    append(child, 0);
  text += "\nDelivery details: stage " + e.stage + "; message ID " + e.id + "\n";
  return text;
}
const NotificationSubscription* NotificationPolicy::subscription(const QString& id) const {
  for (const auto& r : settings_.subscriptions)
    if (r.id == id)
      return &r;
  return nullptr;
}
bool NotificationPolicy::match(const NotificationSubscription& r, const QRegularExpression& wildcard,
                               const ChannelUpdate& s) const {
  return r.enabled && r.configuration == configuration_ &&
         (r.scope.isEmpty() || s.ancestors.contains(r.scope)) && wildcard.match(s.pv).hasMatch();
}
int NotificationPolicy::matchCount(const NotificationSubscription& r,
                                   const QVector<ChannelUpdate>& all) const {
  int n = 0;
  auto copy = r;
  copy.enabled = true;
  const QRegularExpression wildcard(pvWildcard(r.wildcard));
  for (const auto& s : all)
    if (match(copy, wildcard, s))
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
    if (!keep.contains(it->subscription)) {
      forget(*it);
      it = episodes.erase(it);
    } else
      ++it;
  for (auto it = current.begin(); it != current.end();)
    if (!episodes.contains(it.value()))
      it = current.erase(it);
    else
      ++it;
  // Repeat suppression outlives edits; only removed subscriptions release it.
  QSet<QString> subscriptions;
  for (const auto& r : s.subscriptions) subscriptions.insert(r.id);
  for (auto it = cooldown.begin(); it != cooldown.end();)
    if (!subscriptions.contains(it.key())) it = cooldown.erase(it);
    else ++it;
  QSet<QString> destinations;
  for (const auto& d : s.destinations) destinations.insert(d.id);
  for (auto it = destinationNext.begin(); it != destinationNext.end();)
    if (!destinations.contains(it.key())) it = destinationNext.erase(it);
    else ++it;
  settings_ = s;
  configuration_ = configuration;
  wildcards.clear();
  for (const auto& r : settings_.subscriptions)
    wildcards << QRegularExpression(pvWildcard(r.wildcard));
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
    resolvedEnded.clear();
    queue.clear();
    throttled_ = false;
    batchDeadline.clear();
  }
}
void NotificationPolicy::end(Episode& e, bool resolved) {
  e.active = false;
  e.resolved = resolved;
  if (resolved)
    resolvedEnded[e.state.identity] << e.id;
  current.remove(token(e.subscription, e.state.identity));
}
void NotificationPolicy::forget(const Episode& e) {
  auto it = resolvedEnded.find(e.state.identity);
  if (it == resolvedEnded.end())
    return;
  it->remove(e.id);
  if (it->isEmpty())
    resolvedEnded.erase(it);
}
void NotificationPolicy::observe(const ChannelUpdate& s, qint64 now) {
  // Ended episodes are absent from current. Cancel their resolutions when
  // suppression arrives, even if it clears before the transport pump can run.
  if (s.suppressed && !channels.value(s.identity).suppressed)
    for (const auto& id : resolvedEnded.take(s.identity)) {
      auto it = episodes.find(id);
      if (it != episodes.end() && !it->active)
        it->resolved = false;
    }
  channels[s.identity] = s;
  if (!enabled_)
    return;
  if (!matches.contains(s.identity)) {
    QStringList ids;
    for (int i = 0; i < settings_.subscriptions.size(); ++i)
      if (match(settings_.subscriptions[i], wildcards[i], s))
        ids << settings_.subscriptions[i].id;
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
      // Global communication errors now have a local acknowledgement latch.
      // Keep the episode and its clock paused across a gap even when unack is
      // nonzero; only a fresh IOC observation may resume delivery.
      if (!s.initialized || (!s.available && !s.suppressed &&
                            (s.unack == 0 || s.communicationError))) {
        it->waiting = true;
        continue;
      }
      it->waiting = false;
      if (!eligible)
        end(*it, s.available && !s.suppressed && s.unack == 0);
    } else if (eligible && !s.communicationError) {
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
  for (auto& deadlines : cooldown)
    for (auto it = deadlines.begin(); it != deadlines.end();)
      if (!channels.contains(it.key().first)) it = deadlines.erase(it);
      else ++it;
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
  pruneDeadlines(now);
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
      const auto deadlines = cooldown.constFind(r->id);
      if (!resolution && deadlines != cooldown.cend() &&
          deadlines->value(qMakePair(e.state.identity, t), 0) > now)
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
        // Release only this envelope's own entry; a stale alarm envelope must
        // not cancel the episode's queued or in-flight resolution.
        if (e.kind == "resolution")
          it->resolutionQueued.remove(e.destination);
        else
          it->queued.remove(token(e.stage, e.destination));
      }
      e.alarms.removeAt(i);
    } else {
      // A resolution belongs to the ended episode, even if the PV has since
      // entered another alarm episode or been reloaded.
      // Keep the queued value snapshot; state and acknowledgement are revalidated.
      auto& state = e.alarms[i].state;
      auto value = std::move(state.value);
      state = it->state;
      state.value = std::move(value);
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
    // The pump retries every 250 ms; skip per-alarm checks (and copies) for
    // envelopes that are not yet due or whose destination is still paced.
    if (queue[i].ready > now || destinationNext.value(queue[i].destination, 0) > now) {
      ++i;
      continue;
    }
    const bool resolution = queue[i].kind == "resolution";
    bool waiting = false;
    for (const auto& a : queue[i].alarms) {
      const auto channel = channels.constFind(a.state.identity);
      const auto episode = resolution ? episodes.cend() : episodes.constFind(a.episode);
      if (channel == channels.cend() || !channel->initialized ||
          (episode != episodes.cend() && episode->waiting)) {
        waiting = true;
        break;
      }
    }
    if (!waiting) {
      e = queue.takeAt(i);
      return true;
    }
    ++i;
  }
  return false;
}
void NotificationPolicy::attempted(const NotificationEnvelope& e, qint64 now) {
  destinationNext[e.destination] = now + 10000;
  if (!nextExpiry || now + 10000 < nextExpiry) nextExpiry = now + 10000;
  if (e.attempts > 1 || e.kind == "resolution")
    return;
  auto r = subscription(e.subscription);
  if (!r)
    return;
  for (const auto& a : e.alarms)
    cooldown[e.subscription][qMakePair(a.state.identity, token(e.stage, e.destination))] =
        now + qint64(r->cooldownSeconds) * 1000;
  const auto until = now + qint64(r->cooldownSeconds) * 1000;
  if (!nextExpiry || until < nextExpiry) nextExpiry = until;
}
void NotificationPolicy::pruneDeadlines(qint64 now) {
  if (nextExpiry && now < nextExpiry) return;
  nextExpiry = 0;
  auto prune = [&](auto& deadlines) {
    for (auto it = deadlines.begin(); it != deadlines.end();)
      if (it.value() <= now) it = deadlines.erase(it);
      else {
        if (!nextExpiry || it.value() < nextExpiry) nextExpiry = it.value();
        ++it;
      }
  };
  prune(destinationNext);
  for (auto it = cooldown.begin(); it != cooldown.end();) {
    prune(it.value());
    if (it->isEmpty()) it = cooldown.erase(it);
    else ++it;
  }
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
    if (!it->active && it->queued.isEmpty() && it->resolutionQueued.isEmpty() && !resolution) {
      forget(*it);
      it = episodes.erase(it);
    } else
      ++it;
  }
}
} // namespace alh
