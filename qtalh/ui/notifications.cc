#include "window.h"
#include <QtWidgets>
namespace alh {
namespace {
QSpinBox* seconds(int value, QWidget* parent) {
  auto box = new QSpinBox(parent);
  box->setRange(0, 31536000);
  box->setValue(value);
  box->setSuffix(" s");
  return box;
}
QDialogButtonBox* buttons(QDialog& d, QVBoxLayout* l) {
  auto b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  l->addWidget(b);
  QObject::connect(b, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(b, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  return b;
}
bool destinationEditor(QWidget* parent, NotificationDestination& d) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Notification destination");
  dialog.setObjectName("notificationDestinationEditor");
  auto l = new QVBoxLayout(&dialog);
  auto f = new QFormLayout;
  l->addLayout(f);
  auto name = new QLineEdit(d.name);
  name->setObjectName("destinationName");
  auto kind = new QComboBox;
  kind->addItems({"email", "webhook"});
  kind->setCurrentText(d.kind);
  f->addRow("Name", name);
  f->addRow("Type", kind);
  auto stack = new QStackedWidget;
  l->addWidget(stack);
  auto mail = new QWidget;
  auto mf = new QFormLayout(mail);
  auto program = new QLineEdit(d.program);
  program->setObjectName("sendmailProgram");
  if (program->text().isEmpty())
    program->setText(
        QStandardPaths::findExecutable("sendmail", {"/usr/sbin", "/usr/lib", "/usr/bin"}));
  auto args = new QPlainTextEdit(d.arguments.join('\n'));
  args->setMaximumHeight(70);
  auto from = new QLineEdit(d.sender);
  from->setObjectName("emailFrom");
  auto recipients = new QLineEdit(d.recipients.join(", "));
  recipients->setObjectName("emailRecipients");
  mf->addRow("Sendmail executable", program);
  mf->addRow("Arguments (one per line)", args);
  mf->addRow("From", from);
  mf->addRow("Recipients (comma separated)", recipients);
  mf->addRow(new QLabel("Uses a local sendmail-compatible program."));
  stack->addWidget(mail);
  auto web = new QWidget;
  auto wf = new QFormLayout(web);
  auto url = new QLineEdit(d.url);
  auto env = new QLineEdit(d.urlEnvironment);
  auto bearer = new QLineEdit(d.bearerEnvironment);
  url->setObjectName("webhookUrl");
  env->setObjectName("webhookUrlEnvironment");
  bearer->setObjectName("webhookBearerEnvironment");
  wf->addRow("HTTPS URL", url);
  wf->addRow("OR URL environment variable", env);
  wf->addRow("Bearer token environment variable", bearer);
  auto note = new QLabel("Generic JSON. Use environment variables for secret URLs and "
                         "tokens.\nHTTP is allowed only for loopback testing.");
  note->setWordWrap(true);
  wf->addRow(note);
  stack->addWidget(web);
  stack->setCurrentIndex(kind->currentIndex());
  QObject::connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), stack,
                   &QStackedWidget::setCurrentIndex);
  buttons(dialog, l);
  dialog.resize(600, 360);
  while (dialog.exec() == QDialog::Accepted) {
    auto copy = d;
    copy.name = name->text().trimmed();
    copy.kind = kind->currentText();
    copy.program = program->text().trimmed();
    copy.arguments = args->toPlainText().split('\n', Qt::SkipEmptyParts);
    copy.sender = from->text().trimmed();
    copy.recipients = recipients->text().split(',', Qt::SkipEmptyParts);
    for (auto& r : copy.recipients)
      r = r.trimmed();
    copy.url = url->text().trimmed();
    copy.urlEnvironment = env->text().trimmed();
    copy.bearerEnvironment = bearer->text().trimmed();
    try {
      NotificationSettings test;
      test.destinations << copy;
      validateNotifications(test);
      if (copy.kind == "webhook") {
        auto error = NotificationService::validateWebhook(copy);
        if (!error.isEmpty())
          throw std::runtime_error(error.toStdString());
      }
      d = copy;
      return true;
    } catch (const std::exception& e) {
      QMessageBox::warning(&dialog, "Destination", e.what());
    }
  }
  return false;
}
bool stageEditor(QWidget* parent, NotificationStage& s, const NotificationSettings& settings) {
  QDialog d(parent);
  d.setWindowTitle("Notification stage");
  auto l = new QVBoxLayout(&d);
  l->addWidget(new QLabel("Delay since the unacknowledged episode started:"));
  auto delay = seconds(s.delaySeconds, &d);
  l->addWidget(delay);
  auto list = new QListWidget;
  l->addWidget(list);
  for (const auto& dest : settings.destinations) {
    auto item = new QListWidgetItem(dest.name, list);
    item->setData(Qt::UserRole, dest.id);
    item->setCheckState(s.destinations.contains(dest.id) ? Qt::Checked : Qt::Unchecked);
  }
  buttons(d, l);
  if (d.exec() != QDialog::Accepted)
    return false;
  s.delaySeconds = delay->value();
  s.destinations.clear();
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->checkState() == Qt::Checked)
      s.destinations << list->item(i)->data(Qt::UserRole).toString();
  return true;
}
bool subscriptionEditor(QWidget* parent, NotificationSubscription& r,
                        const NotificationSettings& settings, Document& doc,
                        NotificationService& service) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Notification subscription");
  dialog.setObjectName("notificationSubscriptionEditor");
  auto l = new QVBoxLayout(&dialog);
  auto f = new QFormLayout;
  l->addLayout(f);
  auto name = new QLineEdit(r.name);
  name->setObjectName("subscriptionName");
  auto enabled = new QCheckBox("Subscription enabled");
  enabled->setChecked(r.enabled);
  auto scope = new QComboBox;
  scope->setObjectName("subscriptionScope");
  scope->addItem("Whole configuration", QString());
  for (auto n : doc.nodes())
    scope->addItem(Engine::channelPath(n), Engine::nodeIdentity(n));
  int index = scope->findData(r.scope);
  if (index < 0) {
    scope->addItem("Missing scope (choose a replacement)", r.scope);
    index = scope->count() - 1;
  }
  scope->setCurrentIndex(index);
  auto wildcard = new QLineEdit(r.wildcard);
  wildcard->setObjectName("subscriptionWildcard");
  auto severity = new QComboBox;
  for (int i = 1; i <= 4; ++i)
    severity->addItem(severityName(i), i);
  severity->setCurrentIndex(r.minimumSeverity - 1);
  auto cooldown = seconds(r.cooldownSeconds, &dialog);
  auto resolution = new QCheckBox("Send resolution to destinations that accepted an alarm");
  resolution->setChecked(r.resolution);
  auto count = new QLabel;
  count->setObjectName("subscriptionMatchCount");
  f->addRow("Name", name);
  f->addRow(enabled);
  f->addRow("Scope", scope);
  f->addRow("PV wildcard", wildcard);
  f->addRow(count);
  f->addRow("Minimum unacknowledged severity", severity);
  f->addRow("Repeat suppression per stage/destination", cooldown);
  f->addRow(resolution);
  auto previewCount = [&] {
    auto candidate = r;
    candidate.scope = scope->currentData().toString();
    candidate.wildcard = wildcard->text();
    count->setText(QString("Matches %1 channels in this configuration")
                       .arg(service.policy.matchCount(candidate, service.channels())));
  };
  QObject::connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
                   previewCount);
  QObject::connect(wildcard, &QLineEdit::textChanged, &dialog, previewCount);
  previewCount();
  l->addWidget(new QLabel("Stages send once each; delays are measured from episode start."));
  auto stages = r.stages;
  auto list = new QListWidget;
  list->setObjectName("notificationStages");
  l->addWidget(list);
  auto refresh = [&] {
    list->clear();
    for (const auto& s : stages) {
      QStringList destinations;
      for (const auto& id : s.destinations)
        for (const auto& d : settings.destinations)
          if (id == d.id)
            destinations << d.name;
      list->addItem(QString("%1 s — %2").arg(s.delaySeconds).arg(destinations.join(", ")));
    }
  };
  refresh();
  auto row = new QHBoxLayout;
  l->addLayout(row);
  auto add = new QPushButton("Add stage");
  auto edit = new QPushButton("Edit stage");
  auto remove = new QPushButton("Remove stage");
  for (auto b : {add, edit, remove})
    row->addWidget(b);
  QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
    NotificationStage s{notificationId(),
                        stages.isEmpty()     ? 60
                        : stages.size() == 1 ? qMax(600, stages.last().delaySeconds + 1)
                                             : stages.last().delaySeconds + 600,
                        {}};
    if (stageEditor(&dialog, s, settings)) {
      stages << s;
      refresh();
    }
  });
  QObject::connect(edit, &QPushButton::clicked, &dialog, [&] {
    int i = list->currentRow();
    if (i >= 0 && stageEditor(&dialog, stages[i], settings))
      refresh();
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
    int i = list->currentRow();
    if (i >= 0) {
      stages.removeAt(i);
      refresh();
    }
  });
  buttons(dialog, l);
  dialog.resize(720, 620);
  while (dialog.exec() == QDialog::Accepted) {
    auto copy = r;
    copy.name = name->text().trimmed();
    copy.enabled = enabled->isChecked();
    copy.scope = scope->currentData().toString();
    copy.wildcard = wildcard->text();
    copy.minimumSeverity = severity->currentData().toInt();
    copy.cooldownSeconds = cooldown->value();
    copy.resolution = resolution->isChecked();
    copy.stages = stages;
    std::sort(copy.stages.begin(), copy.stages.end(),
              [](const auto& a, const auto& b) { return a.delaySeconds < b.delaySeconds; });
    try {
      auto test = settings;
      test.subscriptions = {copy};
      validateNotifications(test);
      r = copy;
      return true;
    } catch (const std::exception& e) {
      QMessageBox::warning(&dialog, "Subscription", e.what());
    }
  }
  return false;
}
} // namespace
void Window::showNotifications() {
  if (!notifications)
    return;
  if (notificationDialog) {
    notificationDialog->show();
    notificationDialog->raise();
    return;
  }
  auto dialog = new QDialog(this);
  notificationDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Notifications");
  dialog->setObjectName("notificationsDialog");
  auto layout = new QVBoxLayout(dialog);
  auto enabled = new QCheckBox("Enable notifications in this runtime");
  enabled->setObjectName("notificationsEnabled");
  enabled->setChecked(notifications->policy.enabled());
  layout->addWidget(enabled);
  auto status = new QLabel(notifications->status());
  status->setObjectName("notificationStatus");
  layout->addWidget(status);
  auto note = new QLabel("Personal settings. Every runtime starts paused; enabling multiple "
                         "runtimes can send duplicate messages.");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto tabs = new QTabWidget;
  layout->addWidget(tabs);
  auto rulesPage = new QWidget;
  auto rl = new QVBoxLayout(rulesPage);
  auto rules = new QListWidget;
  rules->setObjectName("notificationSubscriptions");
  auto setupHint = new QLabel;
  setupHint->setObjectName("notificationSetupHint");
  setupHint->setWordWrap(true);
  rl->addWidget(setupHint);
  rl->addWidget(rules);
  auto destinationsPage = new QWidget;
  auto dl = new QVBoxLayout(destinationsPage);
  auto destinations = new QListWidget;
  destinations->setObjectName("notificationDestinations");
  auto destinationHint = new QLabel(
      "First add an email or webhook destination, then create a subscription to choose "
      "which alarms to send and when.");
  destinationHint->setWordWrap(true);
  dl->addWidget(destinationHint);
  dl->addWidget(destinations);
  auto activity = new QPlainTextEdit;
  activity->setObjectName("notificationActivity");
  activity->setReadOnly(true);
  tabs->addTab(destinationsPage, "Destinations");
  tabs->addTab(rulesPage, "Subscriptions");
  tabs->addTab(activity, "Activity");
  auto draft = std::make_shared<NotificationSettings>(notifications->policy.settings());
  tabs->setCurrentWidget(draft->destinations.isEmpty() ? destinationsPage : rulesPage);
  auto dirty = std::make_shared<bool>(false);
  auto save = new QPushButton("Save and Apply");
  save->setObjectName("saveNotifications");
  auto test = new QPushButton("Send Test");
  test->setObjectName("testNotification");
  auto refresh = [=] {
    setupHint->setText(draft->destinations.isEmpty()
        ? "Add a destination first. Add subscription will guide you through creating one."
        : "Add a subscription, then add a stage and check the destinations that should receive it.");
    rules->clear();
    for (const auto& r : draft->subscriptions)
      if (r.configuration == notificationConfiguration(doc.filename)) {
        auto item = new QListWidgetItem(QString("%1%2 — %3 stages")
                                            .arg(r.enabled ? "" : "[Disabled] ", r.name)
                                            .arg(r.stages.size()),
                                        rules);
        item->setData(Qt::UserRole, r.id);
      }
    destinations->clear();
    for (const auto& d : draft->destinations) {
      auto item = new QListWidgetItem(d.name + " (" + d.kind + ")", destinations);
      item->setData(Qt::UserRole, d.id);
    }
    test->setEnabled(!*dirty);
  };
  refresh();
  auto ruleRow = new QHBoxLayout;
  rl->addLayout(ruleRow);
  auto addRule = new QPushButton("Add subscription");
  addRule->setObjectName("addNotificationSubscription");
  auto editRule = new QPushButton("Edit");
  auto removeRule = new QPushButton("Remove");
  auto preview = new QPushButton("Preview message");
  for (auto b : {addRule, editRule, removeRule, preview})
    ruleRow->addWidget(b);
  auto editSubscription = [=](bool add) {
    NotificationSubscription r;
    r.id = notificationId();
    r.configuration = notificationConfiguration(doc.filename);
    if (r.configuration.isEmpty()) {
      QMessageBox::warning(dialog, "Subscription",
                           "Save the alarm configuration before adding subscriptions.");
      return;
    }
    if (add && draft->destinations.isEmpty()) {
      NotificationDestination destination;
      destination.id = notificationId();
      if (!destinationEditor(dialog, destination))
        return;
      draft->destinations << destination;
      *dirty = true;
      refresh();
    }
    int at = -1;
    if (!add) {
      if (!rules->currentItem())
        return;
      for (int i = 0; i < draft->subscriptions.size(); ++i)
        if (draft->subscriptions[i].id == rules->currentItem()->data(Qt::UserRole).toString())
          at = i;
      if (at < 0)
        return;
      r = draft->subscriptions[at];
    }
    if (subscriptionEditor(dialog, r, *draft, doc, *notifications)) {
      if (at < 0)
        draft->subscriptions << r;
      else
        draft->subscriptions[at] = r;
      *dirty = true;
      refresh();
    }
  };
  connect(addRule, &QPushButton::clicked, dialog, [=] { editSubscription(true); });
  connect(editRule, &QPushButton::clicked, dialog, [=] { editSubscription(false); });
  connect(removeRule, &QPushButton::clicked, dialog, [=] {
    if (!rules->currentItem())
      return;
    auto id = rules->currentItem()->data(Qt::UserRole).toString();
    for (int i = draft->subscriptions.size() - 1; i >= 0; --i)
      if (draft->subscriptions[i].id == id)
        draft->subscriptions.removeAt(i);
    *dirty = true;
    refresh();
  });
  connect(preview, &QPushButton::clicked, dialog, [=] {
    if (!rules->currentItem())
      return;
    for (const auto& r : draft->subscriptions)
      if (r.id == rules->currentItem()->data(Qt::UserRole).toString()) {
        NotificationEnvelope e;
        e.id = "preview";
        e.subscriptionName = r.name;
        e.configuration = r.configuration;
        e.stage = r.stages.isEmpty() ? QString() : r.stages.first().id;
        for (const auto& s : notifications->channels()) {
          auto match = r;
          match.scope = r.scope;
          if (notifications->policy.matchCount(match, {s}))
            e.alarms << NotificationAlarm{"preview", s};
          if (e.alarms.size() == 100)
            break;
        }
        auto d = new QDialog(dialog);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle("Message preview (sample scope; no delivery)");
        auto l = new QVBoxLayout(d);
        auto text = new QPlainTextEdit(notificationText(e));
        text->setReadOnly(true);
        l->addWidget(text);
        d->resize(720, 500);
        d->show();
      }
  });
  auto destRow = new QHBoxLayout;
  dl->addLayout(destRow);
  auto addDest = new QPushButton("Add destination");
  addDest->setObjectName("addNotificationDestination");
  auto editDest = new QPushButton("Edit");
  auto removeDest = new QPushButton("Remove");
  for (auto b : {addDest, editDest, removeDest, test})
    destRow->addWidget(b);
  auto editDestination = [=](bool add) {
    int i = destinations->currentRow();
    if (!add && i < 0)
      return;
    NotificationDestination d = add ? NotificationDestination{} : draft->destinations[i];
    if (add)
      d.id = notificationId();
    if (destinationEditor(dialog, d)) {
      if (add)
        draft->destinations << d;
      else
        draft->destinations[i] = d;
      *dirty = true;
      refresh();
    }
  };
  connect(addDest, &QPushButton::clicked, dialog, [=] { editDestination(true); });
  connect(editDest, &QPushButton::clicked, dialog, [=] { editDestination(false); });
  connect(removeDest, &QPushButton::clicked, dialog, [=] {
    int i = destinations->currentRow();
    if (i < 0)
      return;
    auto id = draft->destinations[i].id;
    for (const auto& r : draft->subscriptions)
      for (const auto& s : r.stages)
        if (s.destinations.contains(id)) {
          QMessageBox::warning(
              dialog, "Destination",
              "Remove this destination from subscription stages before deleting it.");
          return;
        }
    draft->destinations.removeAt(i);
    *dirty = true;
    refresh();
  });
  connect(test, &QPushButton::clicked, dialog, [=] {
    if (destinations->currentItem())
      notifications->sendTest(destinations->currentItem()->data(Qt::UserRole).toString());
  });
  auto bottom = new QHBoxLayout;
  layout->addLayout(bottom);
  auto reload = new QPushButton("Reload saved settings");
  reload->setObjectName("reloadNotifications");
  auto close = new QPushButton("Close");
  for (auto b : {save, reload, close})
    bottom->addWidget(b);
  connect(save, &QPushButton::clicked, dialog, [=] {
    try {
      notifications->store.save(*draft);
      notifications->apply(*draft);
      *dirty = false;
      refresh();
    } catch (const std::exception& e) {
      QMessageBox::warning(dialog, "Notification settings", e.what());
    }
  });
  connect(reload, &QPushButton::clicked, dialog, [=] {
    try {
      notifications->reload();
      *draft = notifications->policy.settings();
      *dirty = false;
      refresh();
    } catch (const std::exception& e) {
      QMessageBox::warning(dialog, "Notification settings", e.what());
    }
  });
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  connect(enabled, &QCheckBox::toggled, dialog, [this](bool on) { notifications->enable(on); });
  auto timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, [=] {
    status->setText(notifications->status() + (notifications->settingsError.isEmpty()
                                                   ? QString()
                                                   : "; " + notifications->settingsError));
    auto text = notifications->activity.join('\n');
    if (activity->toPlainText() != text) {
      activity->setPlainText(text);
      activity->verticalScrollBar()->setValue(activity->verticalScrollBar()->maximum());
    }
  });
  timer->start(500);
  dialog->resize(850, 560);
  dialog->show();
}
} // namespace alh
