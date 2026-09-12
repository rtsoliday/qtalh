#include "notifications.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <stdexcept>
namespace alh {
namespace {
void fail(const QString& s) { throw std::runtime_error(s.toStdString()); }
QByteArray readSettings(const QString& path) {
  QFile f(path);
  if (!f.exists())
    return {};
  if (!f.open(QIODevice::ReadOnly))
    fail("Cannot read notification settings");
  if (f.size() > 4 * 1024 * 1024)
    fail("Notification settings exceed 4 MiB");
  return f.readAll();
}
QByteArray digest(const QByteArray& b) {
  return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}
QString endpoint(const NotificationDestination& d) {
  return d.urlEnvironment.isEmpty()
             ? d.url
             : QString::fromUtf8(qgetenv(d.urlEnvironment.toUtf8().constData()));
}
bool safeHeader(const QString& s) {
  return !s.contains('\n') && !s.contains('\r') && !s.contains(QChar(0));
}
} // namespace
NotificationStore::NotificationStore(QString path)
    : path_(path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                 "/notifications.json"
                           : path) {}
NotificationSettings NotificationStore::load() {
  auto bytes = readSettings(path_);
  auto settings = !QFileInfo::exists(path_) ? NotificationSettings{} : parseNotifications(bytes);
  revision = digest(bytes);
  loaded = true;
  return settings;
}
void NotificationStore::save(const NotificationSettings& s) {
  validateNotifications(s);
  if (!loaded)
    fail("Reload settings before saving");
  if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
    fail("Cannot create notification settings directory");
  QLockFile lock(path_ + ".lock");
  if (!lock.tryLock(0))
    fail("Notification settings are being saved by another runtime; reload and try again");
  if (digest(readSettings(path_)) != revision)
    fail("Notification settings changed in another runtime. Reload before saving; your edits have "
         "not been saved.");
  QByteArray bytes = QJsonDocument(notificationJson(s)).toJson();
  if (bytes.size() > 4 * 1024 * 1024)
    fail("Notification settings exceed 4 MiB");
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly))
    fail("Cannot open notification settings for saving");
  f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (f.write(bytes) != bytes.size() || !f.commit())
    fail("Cannot save notification settings");
  revision = digest(bytes);
}
NotificationService::NotificationService(QObject* parent, QString path)
    : QObject(parent), store(path), network(this) {
  clock.start();
  monotonicNow = [this] { return clock.elapsed(); };
  policy.activity = [this](const QString& s) { record(s); };
  try {
    policy.configure(store.load(), {});
  } catch (const std::exception& e) {
    settingsError = QString::fromUtf8(e.what());
    record("Failed: " + settingsError);
  }
  timer.setInterval(250);
  connect(&timer, &QTimer::timeout, this, [this] { pump(); });
  timer.start();
}
NotificationService::~NotificationService() {
  detach();
  // Disconnect callbacks before member destruction can tear down their policy state.
  for (auto reply : network.findChildren<QNetworkReply*>()) {
    reply->disconnect(this);
    reply->abort();
  }
  // Kill only our outstanding mail children.
  for (auto p : findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly)) {
    p->disconnect(this);
    if (p->state() != QProcess::NotRunning)
      p->kill();
  }
}
void NotificationService::record(const QString& s) {
  QString line = QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + s;
  activity << line;
  while (activity.size() > 1000)
    activity.removeFirst();
  if (operation)
    operation("Notification " + s);
}
QVector<ChannelUpdate> NotificationService::channels() const {
  QVector<ChannelUpdate> all;
  if (engine)
    for (auto n : nodes)
      all << engine->channelUpdate(n);
  return all;
}
void NotificationService::detach() {
  observer.reset();
  engine = nullptr;
  nodes.clear();
}
void NotificationService::attach(Engine* e, const QVector<Node*>& n, const QString& file) {
  detach();
  engine = e;
  nodes = n;
  auto canonical = notificationConfiguration(file);
  if (canonical != configuration) {
    policy.enable(false);
    configuration = canonical;
  }
  policy.configure(policy.settings(), configuration);
  QHash<QString, int> counts;
  for (auto n : nodes)
    ++counts[Engine::nodeIdentity(n)];
  unique.clear();
  for (auto it = counts.begin(); it != counts.end(); ++it)
    if (it.value() == 1)
      unique << it.key();
  policy.reconcile(channels());
  watch();
}
void NotificationService::watch() {
  if (!engine)
    return;
  observer.reset();
  if (!policy.enabled())
    return;
  observer = engine->observe([this](const AlarmObservation& observation) {
    const auto& s = observation.after;
    if (unique.contains(s.identity))
      policy.observe(s, monotonicNow());
  });
}
void NotificationService::enable(bool enabled) {
  policy.enable(enabled);
  watch();
  if (enabled)
    for (const auto& s : channels())
      if (unique.contains(s.identity))
        policy.observe(s, monotonicNow());
  record(enabled ? "Enabled in this runtime" : "Paused; pending notifications cancelled");
}
void NotificationService::apply(const NotificationSettings& s) {
  policy.configure(s, configuration);
  for (const auto& channel : channels())
    if (unique.contains(channel.identity))
      policy.observe(channel, monotonicNow());
  settingsError.clear();
  record("Settings applied");
}
void NotificationService::reload() { apply(store.load()); }
QString NotificationService::status() const {
  return QString("Notifications: %1; %2 pending; %3 submitting; %4 failed%5")
      .arg(policy.enabled() ? "enabled" : "paused")
      .arg(policy.pending())
      .arg(inFlight)
      .arg(failed)
      .arg(policy.throttled() ? "; throttled" : "");
}
void NotificationService::pump() {
  if (!engine)
    return;
  if (inFlight >= 4)
    return;
  policy.tick(monotonicNow(), 1000 - inFlight);
  NotificationEnvelope e;
  while (inFlight < 4 && policy.take(e, monotonicNow()))
    dispatch(e);
}
QString NotificationService::validateWebhook(const NotificationDestination& d) {
  QUrl url(endpoint(d));
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment())
    return "Invalid webhook URL (credentials and fragments are not allowed)";
  const auto host = url.host().toLower();
  bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
  if (url.scheme() != "https" && !(url.scheme() == "http" && loopback))
    return "Webhook requires HTTPS (HTTP is allowed only for loopback testing)";
  if (!d.bearerEnvironment.isEmpty()) {
    auto bearer = qgetenv(d.bearerEnvironment.toUtf8().constData());
    if (bearer.isEmpty() || bearer.contains('\r') || bearer.contains('\n') || bearer.contains('\0'))
      return "Bearer environment variable is missing or invalid";
  }
  return {};
}
QByteArray NotificationService::mailMessage(const NotificationDestination& d,
                                            const NotificationEnvelope& e) {
  if (d.sender.isEmpty() || d.recipients.isEmpty() || !safeHeader(d.sender))
    fail("Invalid email headers");
  for (const auto& r : d.recipients)
    if (r.isEmpty() || !safeHeader(r))
      fail("Invalid email headers");
  auto encoded = [](const QString& s) {
    QByteArray result;
    QString chunk;
    auto append = [&] {
      if (!result.isEmpty())
        result += "\r\n ";
      result += "=?UTF-8?B?" + chunk.toUtf8().toBase64() + "?=";
      chunk.clear();
    };
    for (int i = 0; i < s.size(); ++i) {
      chunk += s[i];
      if (s[i].isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate())
        chunk += s[++i];
      if (chunk.toUtf8().size() >= 39)
        append();
    }
    if (!chunk.isEmpty())
      append();
    return result;
  };
  QByteArray result =
      "From: " + d.sender.toUtf8() + "\r\nTo: " + d.recipients.join(", ").toUtf8() +
      "\r\nSubject: " + encoded("QtALH " + e.kind + ": " + e.subscriptionName.left(80)) +
      "\r\nDate: " +
      QDateTime::currentDateTimeUtc().toString("ddd, dd MMM yyyy HH:mm:ss +0000").toLatin1() +
      "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; "
      "charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
  auto body = notificationText(e).toUtf8().toBase64();
  for (int i = 0; i < body.size(); i += 76)
    result += body.mid(i, 76) + "\r\n";
  return result;
}
void NotificationService::sendTest(const QString& destination) {
  if (inFlight >= 4 || !policy.destinationReady(destination, monotonicNow())) {
    record(
        "Throttled: test not sent; wait for an available delivery slot and destination interval");
    return;
  }
  NotificationEnvelope e;
  e.id = notificationId();
  e.subscriptionName = "Synthetic notification test";
  e.configuration = configuration;
  e.destination = destination;
  e.stage = "test";
  e.kind = "test";
  e.test = true;
  ChannelUpdate s;
  s.pv = "QTALH:TEST";
  s.path = "Synthetic test / QTALH:TEST";
  s.value = "Test only";
  s.severity = s.unack = 2;
  s.initialized = true;
  s.observedAt = QDateTime::currentMSecsSinceEpoch();
  e.alarms << NotificationAlarm{notificationId(), s};
  dispatch(e);
}
void NotificationService::dispatch(NotificationEnvelope e) {
  NotificationDestination d;
  bool found = false;
  for (const auto& candidate : policy.settings().destinations)
    if (candidate.id == e.destination) {
      d = candidate;
      found = true;
      break;
    }
  ++e.attempts;
  ++inFlight;
  policy.attempted(e, monotonicNow());
  if (!found) {
    finish(e, false, false, "Destination no longer exists");
    return;
  }
  record(QString("Submitting: %1; message %2; attempt %3").arg(d.name, e.id).arg(e.attempts));
  if (d.kind == "email") {
    QByteArray message;
    try {
      message = mailMessage(d, e);
    } catch (const std::exception&) {
      finish(e, false, false, "Invalid email headers");
      return;
    }
    auto p = new QProcess(this);
    auto timeout = new QTimer(p);
    timeout->setSingleShot(true);
    timeout->setInterval(transportTimeoutMs);
    auto done = std::make_shared<bool>(false);
    auto complete = [this, p, e, done](bool ok, bool retry, const QString& reason) {
      if (*done)
        return;
      *done = true;
      p->deleteLater();
      finish(e, ok, retry, reason);
    };
    connect(p, &QProcess::started, p, [p, message] {
      p->write(message);
      p->closeWriteChannel();
    }); // Discard bounded chunks, never log mailer output (it can contain secrets).
    connect(p, &QProcess::readyReadStandardError, p, [p] { p->readAllStandardError(); });
    connect(p, &QProcess::readyReadStandardOutput, p, [p] { p->readAllStandardOutput(); });
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [complete](int code, QProcess::ExitStatus status) {
              complete(status == QProcess::NormalExit && code == 0,
                       status == QProcess::NormalExit && code == 75,
                       status == QProcess::NormalExit ? QString("Mailer exit %1").arg(code)
                                                      : "Mailer crashed; submission uncertain");
            });
    connect(p, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart)
        complete(false, false, "Mailer could not start");
    });
    connect(timeout, &QTimer::timeout, this, [p, complete] {
      p->kill();
      complete(false, false, "Mailer timeout; submission uncertain");
    });
    p->start(d.program, d.arguments);
    timeout->start();
    return;
  }
  auto problem = validateWebhook(d);
  if (!problem.isEmpty()) {
    finish(e, false, false, problem);
    return;
  }
  QNetworkRequest request{QUrl(endpoint(d))};
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("X-QtALH-Message-ID", e.id.toUtf8());
  if (!d.bearerEnvironment.isEmpty())
    request.setRawHeader("Authorization",
                         "Bearer " + qgetenv(d.bearerEnvironment.toUtf8().constData()));
  auto reply = network.post(request, notificationPayload(e));
  reply->setReadBufferSize(65536);
  auto timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  timeout->setInterval(transportTimeoutMs);
  auto tooLarge = std::make_shared<bool>(false);
  auto bytes = std::make_shared<qint64>(0);
  connect(reply, &QNetworkReply::readyRead, reply, [reply, bytes, tooLarge] {
    *bytes += reply->readAll().size();
    if (*bytes > 65536) {
      *tooLarge = true;
      reply->abort();
    }
  });
  connect(timeout, &QTimer::timeout, reply, [reply] { reply->abort(); });
  connect(reply, &QNetworkReply::finished, this, [this, reply, e, tooLarge] {
    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool ok = code >= 200 && code < 300 && reply->error() == QNetworkReply::NoError && !*tooLarge;
    bool retry = !ok && !*tooLarge && (code == 0 || code == 429 || code >= 500);
    int delay = reply->rawHeader("Retry-After").toInt();
    if (!delay) {
      auto date = QDateTime::fromString(QString::fromLatin1(reply->rawHeader("Retry-After")),
                                        Qt::RFC2822Date);
      if (date.isValid())
        delay = int(QDateTime::currentDateTimeUtc().secsTo(date));
    }
    finish(e, ok, retry,
           *tooLarge ? "Webhook response exceeded 64 KiB"
           : code    ? QString("HTTP %1").arg(code)
                     : "Webhook network/TLS failure or timeout; acceptance uncertain",
           qBound(0, delay, 600));
    reply->deleteLater();
  });
  timeout->start();
}
void NotificationService::finish(NotificationEnvelope e, bool accepted, bool retryable,
                                 const QString& reason, int retryAfter) {
  --inFlight;
  if (!accepted && retryable && e.attempts < 3 && !e.test && policy.revalidate(e)) {
    e.ready = monotonicNow() + qint64(qMax(e.attempts == 1 ? 30 : 120, retryAfter)) * 1000;
    policy.retry(e);
    record("Queued retry: message " + e.id + "; " + reason);
  } else {
    policy.completed(e, accepted);
    if (!accepted)
      ++failed;
    record((accepted ? QString("Accepted/submitted: ") : QString("Failed: ")) + "message " + e.id +
           "; " + reason);
  }
}
} // namespace alh
