#include "core/diagnostics.h"
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
// Qt Widgets port of ALH awAlh/awAct/axRunW/axSubW and dialog workflows.
// Author credits are recorded in ../../AUTHORS.md; see ../../LICENSE for the license.
#include "alarm_view.h"
#include "dialogs.h"
#include "window.h"
#include <QDesktopServices>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QRegularExpression>
#include <QTextDocument>
#include <QtWidgets>
namespace alh {
namespace {
QColor severityColor(int s) {
  static const QColor c[] = {QColor("#b0c3ca"), QColor("yellow"), QColor("red"), QColor("white"),
                             QColor("white")};
  return c[qBound(0, s, 4)];
}
QString code(int s) {
  return QString(" YRVE").mid(qBound(0, s, 4), 1);
}
} // namespace
AlarmModel::AlarmModel(Document* d, Engine* e, bool t, QObject* p)
    : QAbstractItemModel(p), doc(d), engine(e), tree(t), group(d->root.get()) {}
bool AlarmModel::visible(Node* n) const {
  if (!filter)
    return true;
  auto& s = engine->state(n);
  return filter == 1 ? s.severity > 0 || s.unack > 0 : s.unack > 0;
}
QVector<Node*> AlarmModel::children(Node* n) const {
  auto cached = childCache.constFind(n);
  if (cached != childCache.cend())
    return cached.value();
  auto key = n;
  QVector<Node*> result;
  if (!n) {
    if (tree && doc->root && visible(doc->root.get()))
      result.push_back(doc->root.get());
    else if (!tree)
      n = group;
  }
  if (n) {
    for (auto& c : n->children)
      if (c->group && visible(c.get()))
        result.push_back(c.get());
    if (!tree)
      for (auto& c : n->children)
        if (!c->group && visible(c.get()))
          result.push_back(c.get());
  }
  childCache.insert(key, result);
  return result;
}
QModelIndex AlarmModel::index(int row, int col, const QModelIndex& p) const {
  if (row < 0 || col < 0 || col >= 9 || p.column() > 0)
    return {};
  if (!tree && p.isValid())
    return {};
  auto list = children(node(p));
  if (row >= list.size())
    return {};
  return createIndex(row, col, list[row]);
}
QModelIndex AlarmModel::parent(const QModelIndex& i) const {
  if (!tree)
    return {};
  auto n = node(i);
  if (!n || !n->parent)
    return {};
  auto p = n->parent;
  auto siblings = children(p->parent);
  int row = siblings.indexOf(p);
  return row < 0 ? QModelIndex() : createIndex(row, 0, p);
}
int AlarmModel::rowCount(const QModelIndex& p) const {
  if (p.column() > 0 || (!tree && p.isValid()))
    return 0;
  return children(node(p)).size();
}
QVariant AlarmModel::data(const QModelIndex& i, int role) const {
  auto n = node(i);
  if (!n)
    return {};
  const auto& s = engine->state(n);
  int severity = !n->group && (s.mask[Cancel] || s.mask[Disable]) ? 0 : s.severity;
  int unack = !n->group && (s.mask[Cancel] || s.mask[Disable] || s.mask[Ack]) ? 0 : s.unack;
  if (role == Qt::ToolTipRole)
    return n->name + "\n" + statusName(s.status) + " " + severityName(s.severity) + "\n" + s.value;
  if (role == Qt::BackgroundRole) {
    if (i.column() == 0)
      return severityColor(unack);
    if (i.column() == 1)
      return severityColor(severity);
    if (i.column() == 2)
      return n->group ? QColor("#b0c3ca") : QColor("lightblue");
    if (i.column() == 6 && coloredMasks) {
      // ALH colors <CDATL> when C, D, or A (including the timed H) is set.
      const bool silenced = n->group
                                ? s.maskCounts[Cancel] || s.maskCounts[Disable] || s.maskCounts[Ack]
                                : s.mask[Cancel] || s.mask[Disable] || s.mask[Ack];
      if (silenced || s.noAckUntil)
        return QColor("blue");
    }
  }
  if (role == Qt::FontRole) {
    QFont f("monospace", 10);
    f.setStyleHint(QFont::TypeWriter);
    if (i.column() == 2) {
      f = QFont("Helvetica", 10);
      f.setBold(true);
    }
    f.setPixelSize(12);
    f.setStretch(100);
    if (i.column() != 2)
      f.setStretch(85);
    return f;
  }
  if (role == Qt::TextAlignmentRole)
    return int(i.column() < 2 ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
  if (role != Qt::DisplayRole)
    return {};
  switch (i.column()) {
  case 0:
    return code(unack);
  case 1:
    return code(severity);
  case 2:
    return n->label();
  case 3:
    return n->group && std::any_of(n->children.begin(), n->children.end(),
                                   [](const auto& c) { return c->group; })
               ? QString::fromUtf8("▶")
               : QString();
  case 4:
    return !n->option("GUIDANCE").isEmpty() || !n->option("GUIDANCE_TEXT").isEmpty() ? "G" : "";
  case 5:
    return n->option("COMMAND").isEmpty() ? "" : "P";
  case 6: {
    QString m = s.mask.text();
    if (n->group) {
      m = "-----";
      for (int j = 0; j < 5; ++j)
        if (s.maskCounts[j])
          m[j] = QString("CDATL")[j];
    }
    if (s.noAckUntil)
      m[2] = 'H';
    return '<' + m + '>';
  }
  case 7:
    return s.beepThreshold > 1 ? code(s.beepThreshold) : QString();
  case 8:
    if (!severity && !unack)
      return QString();
    if (n->group)
      return QString("(%1,%2,%3,%4,%5)")
          .arg(s.counts[4])
          .arg(s.counts[3])
          .arg(s.counts[2])
          .arg(s.counts[1])
          .arg(s.counts[0]);
    return QString("<%1,%2>").arg(statusName(s.status), severityName(severity)) +
           (unack ? QString(",<%1>").arg(severityName(unack)) : QString());
  }
  return {};
}
Qt::ItemFlags AlarmModel::flags(const QModelIndex& i) const {
  return i.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled
                     : Qt::NoItemFlags;
}
QStringList AlarmModel::mimeTypes() const {
  return {"text/plain"};
}
QMimeData* AlarmModel::mimeData(const QModelIndexList& list) const {
  auto data = new QMimeData;
  QStringList names;
  for (auto i : list)
    if (node(i) && !names.contains(node(i)->name))
      names << node(i)->name;
  data->setText(names.join('\n'));
  return data;
}
void AlarmModel::reset(Document* d, Engine* e) {
  beginResetModel();
  childCache.clear();
  doc = d;
  engine = e;
  group = d->root.get();
  endResetModel();
}
void AlarmModel::setGroup(Node* n) {
  beginResetModel();
  childCache.clear();
  group = n;
  endResetModel();
}
void AlarmModel::setFilter(int f) {
  beginResetModel();
  childCache.clear();
  filter = f;
  endResetModel();
}
void AlarmModel::refresh() {
  // Filter membership can change as alarms arrive. Unfiltered views retain selection.
  if (filter) {
    beginResetModel();
    childCache.clear();
    endResetModel();
    return;
  }
  std::function<void(QModelIndex)> visit = [&](QModelIndex p) {
    int count = rowCount(p);
    if (count)
      emit dataChanged(index(0, 0, p), index(count - 1, 8, p));
    if (tree)
      for (int r = 0; r < count; ++r)
        visit(index(r, 0, p));
  };
  visit({});
}
Window::Window(Document d, Options o, bool connections)
    : doc(std::move(d)), options(o), connectPv(connections) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowIcon(QIcon(":/qtalh/alh.xbm"));
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { delete this; });
  QFont mainFont("monospace");
  mainFont.setPixelSize(12);
  mainFont.setStretch(85);
  setFont(mainFont);
  setupEngine();
  buildUi();
  menus();
  setWindowTitle((options.editor ? "Alarm Configuration Tool: " : "Alarm Handler: ") +
                 doc.root->name);
  resize(1000, 600);
  if (!options.geometry.isEmpty()) {
    auto m = QRegularExpression("^(\\d+)x(\\d+)([+-]\\d+)?([+-]\\d+)?$").match(options.geometry);
    if (m.hasMatch()) {
      resize(m.captured(1).toInt(), m.captured(2).toInt());
      if (!m.captured(3).isEmpty())
        move(m.captured(3).toInt(), m.captured(4).toInt());
    }
  }
  heartbeatTimer.setParent(this);
  heartbeatTimer.setObjectName("heartbeatTimer");
  heartbeatTimer.setSingleShot(true);
  heartbeatTimer.setTimerType(Qt::PreciseTimer);
  connect(&heartbeatTimer, &QTimer::timeout, this, [this] {
    engine->tickHeartbeat();
    scheduleHeartbeat();
  });
  refreshTimer.setInterval(200);
  connect(&refreshTimer, &QTimer::timeout, this, [this] {
    engine->tick();
    if (dirty) {
      refresh();
      dirty = false;
    }
  });
  refreshTimer.start();
  beepTimer.setInterval(1000);
  connect(&beepTimer, &QTimer::timeout, this, [this] {
    if (engine->audible()) {
      if (options.sound.isEmpty())
        QApplication::beep();
      else if (sound && sound->error() == QMediaPlayer::NoError) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const bool playing = sound->playbackState() == QMediaPlayer::PlayingState;
#else
        const bool playing = sound->state() == QMediaPlayer::PlayingState;
#endif
        if (!playing) {
          sound->setPosition(0);
          sound->play();
        }
      }
    }
    if (dirty)
      refresh();
    else
      refreshStatus();
  });
  beepTimer.start();
  if (!options.sound.isEmpty()) {
    sound = new QMediaPlayer(this);
    sound->setObjectName("alarmSound");
    auto reportSoundError = [this] {
      error(QString("Cannot play alarm sound %1: %2").arg(options.sound, sound->errorString()));
    };
    const auto source = QUrl::fromLocalFile(QFileInfo(options.sound).absoluteFilePath());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    sound->setAudioOutput(new QAudioOutput(sound));
    connect(sound, &QMediaPlayer::errorOccurred, this, reportSoundError);
    sound->setSource(source);
#else
    connect(sound, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            reportSoundError);
    sound->setMedia(source);
#endif
  }
  if (!options.editor)
    engine->start();
  scheduleHeartbeat();
  refresh();
}
void Window::scheduleHeartbeat() {
  heartbeatTimer.stop();
  int delay = engine->heartbeatDelay();
  if (delay >= 0)
    heartbeatTimer.start(delay);
}
Window::~Window() {
  debugLog(options.debug, "window", "close " + doc.filename);
  heartbeatTimer.stop();
  refreshTimer.stop();
  beepTimer.stop();
  if (sound)
    sound->stop();
  engine.reset();
  ca.reset();
  delete runtime;
}
void Window::setupEngine() {
  options.engine.debug = options.debug;
  debugLog(options.debug, "config", QString("load %1: %2 nodes, %3 channels")
      .arg(doc.filename).arg(doc.nodes().size()).arg(doc.channels().size()));
  heartbeatTimer.stop();
  engine.reset();
  ca.reset();

  if (connectPv && !options.editor) {
    ca = std::make_unique<ChannelAccess>(options.engine);
    ca->error = [this](const QString& s) { error(s); };
    for (auto n : doc.channels())
      ca->setInitialAckT(n->name, !n->mask[AckT]);
  }
  engine = std::make_unique<Engine>(doc, options.engine, ca.get());
  engine->silenceForever = options.silent;
  engine->error = [this](const QString& s) { error(s); };
  engine->changed = [this] { dirty = true; };
  if (!options.editor && !logging) {
    logging = std::make_unique<Logging>(options, doc.root->name);
    logging->error = [this](const QString& s) { error(s); };
    logging->message = [this](const QString& s) { showText("Broadcast Message", s); };
    logging->reload = [this] {
      QTimer::singleShot(0, this, [this] {
        try {
          replace(loadConfig(doc.filename, options.configDir), false);
        } catch (const std::exception& e) {
          error(e.what());
        }
      });
    };
    engine->alarmLog = [this](Node* n, const State& s, qint64 t) { logging->alarm(n, s, t); };
    engine->operation = [this](Node* n, const QString& s) { logging->operation(n, s); };
  }
  if (logging) {
    engine->acknowledgement = [this](Node* n) { logging->acknowledgement(n); };
    engine->alarmLog = [this](Node* n, const State& s, qint64 t) { logging->alarm(n, s, t); };
    engine->operation = [this](Node* n, const QString& s) { logging->operation(n, s); };
  }
  engine->command = [this](QString command) {
    debugLog(options.debug, "command", "requested " + command);
    if (logging && !logging->commandsAllowed())
      return;
    if (command.startsWith("MASTER_ONLY")) {
      command = command.mid(11).trimmed();
      if (logging && !logging->isMaster())
        return;
    }
    // Prefer the Qt display manager even for legacy configuration commands.
    // Replace only the executable; preserve arguments, quoting, and redirects.
    static const QRegularExpression executable(R"(^\s*("[^"]+"|'[^']+'|[^\s;&|<>]+))");
    const auto match = executable.match(command);
    QString program = match.captured(1);
    if (program.startsWith('"') || program.startsWith('\''))
      program = program.mid(1, program.size() - 2);
    program.replace('\\', '/');
    const auto basename = program.section('/', -1);
    if (basename == "medm" || basename == "medm.exe")
      command.replace(match.capturedStart(1), match.capturedLength(1), "qtedm");
#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(qEnvironmentVariable("COMSPEC", "cmd.exe"));
    process.setNativeArguments("/d /s /c \"" + command + "\"");
    if (!process.startDetached())
#else
    if (!QProcess::startDetached("/bin/sh", {"-c", command}))
#endif
      error("Cannot start command: " + command);
  };
  selection = selectedGroup = doc.root.get();
  dirty = true;
}
void Window::buildUi() {
  auto central = new QWidget;
  auto layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto splitter = new QSplitter;
  splitter->setObjectName("alarmPanes");
  splitter->setHandleWidth(2);
  splitter->setStyleSheet("QSplitter::handle {background:black;}");
  treeView = new AlarmView(true);
  groupView = new AlarmView(false);
  treeView->setObjectName("alarmTree");
  groupView->setObjectName("groupContents");
  treeModel = new AlarmModel(&doc, engine.get(), true, this);
  groupModel = new AlarmModel(&doc, engine.get(), false, this);
  treeModel->coloredMasks = groupModel->coloredMasks = options.maskColor;
  treeView->setModel(treeModel);
  groupView->setModel(groupModel);
  for (auto view : {treeView, groupView})
    splitter->addWidget(view);
  splitter->setSizes({500, 500});
  layout->addWidget(splitter, 1);
  auto divider = new QSlider(Qt::Horizontal);
  divider->setObjectName("paneWidth");
  divider->setRange(5, 95);
  divider->setValue(50);
  divider->setFixedHeight(17);
  divider->setToolTip("Tree/group window width");
  divider->setStyleSheet(
      "QSlider {background:#b0c3ca; border:1px solid; border-top-color:#dde6e9; "
      "border-left-color:#dde6e9; border-bottom-color:#5f696d; border-right-color:#5f696d;}"
      "QSlider::groove:horizontal {height:11px; margin:1px; background:#b0c3ca; "
      "border:1px solid; border-top-color:#5f696d; border-left-color:#5f696d; "
      "border-bottom-color:#dde6e9; border-right-color:#dde6e9;}"
      "QSlider::handle:horizontal {width:28px; margin:1px 0; background:#b0c3ca; "
      "border:1px solid; border-top-color:#dde6e9; border-left-color:#dde6e9; "
      "border-bottom-color:#5f696d; border-right-color:#5f696d;}");
  connect(divider, &QSlider::valueChanged, splitter, [splitter](int value) {
    int width = splitter->width() - splitter->handleWidth();
    splitter->setSizes({width * value / 100, width * (100 - value) / 100});
  });
  connect(splitter, &QSplitter::splitterMoved, divider, [splitter, divider](int, int) {
    auto sizes = splitter->sizes();
    QSignalBlocker block(divider);
    if (sizes[0] + sizes[1])
      divider->setValue(100 * sizes[0] / (sizes[0] + sizes[1]));
  });
  layout->addWidget(divider);
  connect(treeView, &QTreeView::clicked, this, [this](QModelIndex i) { click(i, treeModel); });
  connect(groupView, &QTreeView::clicked, this, [this](QModelIndex i) { click(i, groupModel); });
  connect(groupView, &QTreeView::doubleClicked, this, [this](QModelIndex i) {
    auto n = groupModel->node(i);
    if (n && n->group)
      select(n);
    else
      properties();
  });
  connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            auto n = treeModel->node(i);
            if (n && (i.column() == 0 || i.column() == 2))
              select(n);
          });
  connect(groupView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            auto n = groupModel->node(i);
            if (n)
              selection = n;
            scheduleDialogSync();
          });
  auto footer = new QWidget;
  footer->setFixedHeight(114);
  auto bottom = new QHBoxLayout(footer);
  bottom->setContentsMargins(10, 4, 4, 17);
  auto info = new QVBoxLayout;
  info->setSpacing(2);
  execution = new QLabel;
  auto statusRow = new QHBoxLayout;
  statusRow->addWidget(execution);
  statusRow->addStretch();
  disabledForceLabel = new QLabel;
  disabledForceLabel->setObjectName("disabledForcePvCount");
  statusRow->addWidget(disabledForceLabel);
  info->addLayout(statusRow);
  info->addWidget(
      new QLabel("Mask <CDATL>:  <Cancel,Disable,noAck,noackT,noLog>     H=noAck 1hr timer"));
  info->addWidget(new QLabel("Group Alarm Counts:  (ERROR,INVALID,MAJOR,MINOR,NOALARM)"));
  info->addWidget(new QLabel("Channel Alarm Data: <Status,Severity> (<Unack Severity>)"));
  filename = new QLabel;
  filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  info->addWidget(filename);
  bottom->addLayout(info, 1);
  auto silence = new QVBoxLayout;
  silence->setSpacing(4);
  silence->setAlignment(Qt::AlignRight);
  QFont toggleFont("monospace");
  toggleFont.setPixelSize(13);
  toggleFont.setStretch(100);
  silenceBox = new MotifCheckBox("Silence 30 minutes");
  currentBox = new MotifCheckBox("Silence current");
  silenceBox->setFont(toggleFont);
  currentBox->setFont(toggleFont);
  silenceBox->setObjectName("silenceInterval");
  currentBox->setObjectName("silenceCurrent");
  silenceForeverLabel = new QLabel;
  silenceForeverLabel->setObjectName("silenceForever");
  beepLabel = new QLabel;
  beepLabel->setObjectName("beepSeverity");
  silenceForeverLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  beepLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  // Each line occupies the same height, including the two status labels.
  for (QWidget* row :
       {static_cast<QWidget*>(silenceBox), static_cast<QWidget*>(currentBox),
        static_cast<QWidget*>(silenceForeverLabel), static_cast<QWidget*>(beepLabel)}) {
    row->setFixedHeight(20);
    silence->addWidget(row, 0, Qt::AlignRight);
  }
  bottom->addLayout(silence);
  layout->addWidget(footer);
  connect(silenceBox, &QCheckBox::toggled, this, [this](bool yes) {
    engine->silenceUntil = yes ? engine->now() + silenceMinutes * 60000 : 0;
  });
  connect(currentBox, &QCheckBox::toggled, this,
          [this](bool yes) { engine->silenceCurrent = yes; });
  messageArea = new QLabel;
  messageArea->setWordWrap(true);
  messageArea->hide();
  layout->addWidget(messageArea);
  setCentralWidget(central);
  treeModel->setFilter(options.filter);
  groupModel->setFilter(options.filter);
  treeView->expand(treeModel->index(0, 0));
  treeView->setCurrentIndex(treeModel->index(0, 2));
  runtime = new QWidget(nullptr, Qt::Window);
  runtime->setWindowTitle("Alarm Handler");
  runtime->setObjectName("runtimeWindow");
  runtime->setWindowIcon(windowIcon());
  runtime->setFont(font());
  runtime->setStyleSheet("#runtimeWindow {border:2px outset #b0c3ca;}");
  auto rlayout = new QHBoxLayout(runtime);
  rlayout->setContentsMargins(6, 5, 6, 5);
  runtimeButton = new MotifButton(doc.root->label());
  runtimeButton->setObjectName("runtimeAlarm");
  runtimeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  rlayout->addWidget(runtimeButton);
  runtime->resize(220, 35);
  connect(runtimeButton, &QPushButton::clicked, this, [this] {
    show();
    raise();
    activateWindow();
  });
  runtime->installEventFilter(this);
  if (!options.font.isEmpty()) {
    QFont f;
    auto parts = options.font.split('-');
    if (options.font.startsWith('-') && parts.size() > 7) {
      f = QFont(parts.value(2), 10);
      bool ok = false;
      int px = parts.value(7).toInt(&ok);
      if (ok && px > 0)
        f.setPixelSize(px);
      f.setBold(parts.value(3) == "bold");
    } else if (!f.fromString(options.font))
      f = QFont(options.font);
    runtimeButton->setFont(f);
  }
}
void Window::showInitial() {
  if (options.editor || options.mainWindow)
    show();
  if (!options.editor)
    runtime->show();
}
void Window::error(const QString& s) {
  qWarning("QtALH: %s", qPrintable(s));
  if (messageArea) {
    messageArea->setText(s);
    messageArea->show();
  }
  if (!options.noErrorPopup && (isVisible() || (runtime && runtime->isVisible()))) {
    if (!errorPopup) {
      auto popup = new QMessageBox(QMessageBox::Warning, "ALH Message", s, QMessageBox::Ok, this);
      popup->setAttribute(Qt::WA_DeleteOnClose);
      errorPopup = popup;
      popup->show();
    } else
      errorPopup->setDetailedText((errorPopup->detailedText() + "\n" + s).right(16000));
  }
}
void Window::refresh() {
  dirty = false;
  // A filtered reset invalidates QModelIndex values. Keep node identity and
  // expansion independent of that reset so new alarms do not move the operator.
  QSet<Node*> expanded;
  Node* currentTree = treeModel->node(treeView->currentIndex());
  Node* currentGroup = groupModel->node(groupView->currentIndex());
  std::function<void(QModelIndex, bool)> visit = [&](QModelIndex parent, bool restore) {
    for (int row = 0; row < treeModel->rowCount(parent); ++row) {
      auto index = treeModel->index(row, 0, parent);
      auto node = treeModel->node(index);
      if (restore) {
        treeView->setExpanded(index, expanded.contains(node));
        if (node == currentTree)
          treeView->setCurrentIndex(index);
      } else if (treeView->isExpanded(index))
        expanded.insert(node);
      visit(index, restore);
    }
  };
  if (options.filter)
    visit({}, false);
  QSignalBlocker treeSignals(treeView->selectionModel());
  QSignalBlocker groupSignals(groupView->selectionModel());
  treeModel->refresh();
  groupModel->refresh();
  if (options.filter) {
    visit({}, true);
    for (int row = 0; row < groupModel->rowCount(); ++row) {
      auto index = groupModel->index(row, 0);
      if (groupModel->node(index) == currentGroup)
        groupView->setCurrentIndex(index);
    }
  }
  int disabledForces = 0;
  for (auto node : doc.nodes())
    if (engine->state(node).forceDisabled) ++disabledForces;
  disabledForceLabel->setText(disabledForces ? QString("Disabled forcePVs: %1").arg(disabledForces)
                                            : QString());
  refreshStatus();
  if (historyText) {
    QStringList rows;
    const QRegularExpression fields("^(\\S+ \\S+) (\\S+) (\\S+) (\\S+) (.*)$");
    for (const auto& entry : engine->history) {
      const auto match = fields.match(entry);
      rows << (match.hasMatch()
                   ? QString("%1 %2 %3 %4 %5")
                         .arg(match.captured(1), -21).arg(match.captured(2), -32)
                         .arg(match.captured(3), -10).arg(match.captured(4), -10)
                         .arg(match.captured(5))
                   : entry);
    }
    const auto text = rows.join('\n');
    if (historyText->toPlainText() != text)
      historyText->setPlainText(text);
  }
  scheduleDialogSync();
  if (undoAction)
    undoAction->setEnabled(!undo.isEmpty());
  if (redoAction)
    redoAction->setEnabled(!redo.isEmpty());
}
// Time-dependent indicators and logging ownership still update while alarms are idle.
void Window::refreshStatus() {
  execution->setText(
      QString("Execution Status: %1 %2%3")
          .arg(options.engine.global ? "Global" : "Local",
               options.engine.passive ? "Passive" : "Active",
               logging && options.lock ? (logging->isMaster() ? " — Master" : " — Slave") : ""));
  filename->setText("Filename:  " + doc.filename + (modified ? " *" : ""));
  filename->setToolTip(doc.filename);
  silenceForeverLabel->setText(
      QString("Silence Forever: %1").arg(engine->silenceForever ? "On" : "Off"));
  beepLabel->setText(QString("ALH Beep Severity: %1").arg(severityName(doc.beepSeverity)));
  {
    QSignalBlocker b(silenceBox);
    silenceBox->setChecked(engine->silenceUntil > engine->now());
  }
  {
    QSignalBlocker b(currentBox);
    currentBox->setChecked(engine->silenceCurrent);
  }
  auto& s = engine->state(doc.root.get());
  int color = s.unack ? ((engine->now() / 1000) % 2 ? s.unack : 0) : s.severity;
  auto button = static_cast<MotifButton*>(runtimeButton);
  const auto background = severityColor(color);
  if (button->background != background) {
    button->background = background;
    button->update();
  }
  QString mask = "-----";
  for (int bit = 0; bit < 5; ++bit)
    if (s.maskCounts[bit])
      mask[bit] = QString("CDATL")[bit];
  runtimeButton->setText(doc.root->label() + (mask == "-----" ? QString() : "  <" + mask + ">"));
  scheduleDialogSync();
}
void Window::select(Node* n) {
  selection = n;
  scheduleDialogSync();
  if (n->group) {
    selectedGroup = n;
    groupModel->setGroup(n);
  }
}
void Window::click(const QModelIndex& i, AlarmModel* m) {
  auto n = m->node(i);
  if (!n)
    return;
  selection = n;
  scheduleDialogSync();
  switch (i.column()) {
  case 0:
    engine->acknowledge(n);
    break;
  case 2:
    if (n->group && m == treeModel)
      select(n);
    break;
  case 3:
    if (n->group) {
      std::function<QModelIndex(Node*)> locate = [&](Node* node) -> QModelIndex {
        auto parent = node->parent ? locate(node->parent) : QModelIndex();
        for (int row = 0; row < treeModel->rowCount(parent); ++row) {
          auto index = treeModel->index(row, 0, parent);
          if (treeModel->node(index) == node)
            return index;
        }
        return {};
      };
      auto index = locate(n);
      if (index.isValid()) {
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
          treeView->expand(parent);
        treeView->setExpanded(index, !treeView->isExpanded(index));
      }
    }
    break;
  case 4:
    guidance();
    break;
  case 5:
    related();
    break;
  case 6:
    masks();
    break;
  case 7:
    beepSeverity(false);
    break;
  }
  refresh();
}
void Window::expandBranch(const QModelIndex& i) {
  treeView->expand(i);
  for (int r = 0; r < treeModel->rowCount(i); ++r)
    expandBranch(treeModel->index(r, 0, i));
}
void Window::menus() {
  auto action = [this](QMenu* m, QString name, std::function<void()> fn,
                       QKeySequence key = {}) -> QAction* {
    auto a = m->addAction(name);
    a->setObjectName(name.remove('&'));
    if (!key.isEmpty())
      a->setShortcut(key);
    connect(a, &QAction::triggered, this, std::move(fn));
    return a;
  };
  auto file = menuBar()->addMenu("&File");
  if (options.editor)
    action(
        file, "&New",
        [this] {
          auto o = options;
          o.config.clear();
          auto d = parseConfig("GROUP NULL NewGroup\n");
          auto w = new Window(std::move(d), o, connectPv);
          w->showInitial();
        },
        QKeySequence("Ctrl+N"));
  action(file, "&Open...", [this] { open(); }, QKeySequence("Ctrl+O"));
  if (options.editor)
    action(file, "&Save", [this] { save(); }, QKeySequence("Ctrl+S"));
  action(file, "Save &As...", [this] { save(true); });
  if (options.editor) {
    action(file, "Activate ALH...", [this] { activateRuntime(); });
    action(file, "Report...", [this] {
      auto p = chooseFile(this, "Report File", {}, "Reports (*.alhReport)", true);
      if (!p.isEmpty()) {
        QFile f(p);
        if (f.open(QIODevice::WriteOnly))
          f.write(writeReport(doc).toLocal8Bit());
        else
          error(f.errorString());
      }
    });
  }
  action(file, "Print...", [this] {
    QPrinter printer;
    QPrintDialog dialog(&printer, this);
    if (dialog.exec() == QDialog::Accepted) {
      QTextDocument report;
      report.setPlainText(writeReport(doc));
      report.print(&printer);
    }
  });
  action(file, "&Close", [this] {
    if (!options.editor) {
      exitApplication();
      return;
    }
    quitting = true;
    close();
  });
  if (options.editor)
    action(file, "Close All", [] { QApplication::closeAllWindows(); });
  file->addSeparator();
  action(file, "E&xit", [this] { exitApplication(); }, QKeySequence("Ctrl+X"));
  if (options.editor) {
    auto edit = menuBar()->addMenu("&Edit");
    undoAction = action(edit, "&Undo", [this] { undoEdit(); }, QKeySequence("Alt+Backspace"));
    redoAction = action(edit, "&Redo", [this] { redoEdit(); }, QKeySequence("Ctrl+Y"));
    edit->addSeparator();
    action(edit, "Cu&t", [this] { cut(true); }, QKeySequence("Shift+Delete"));
    action(edit, "&Copy", [this] { cut(false); }, QKeySequence("Ctrl+Insert"));
    action(edit, "&Paste", [this] { paste(); }, QKeySequence("Shift+Insert"));
    action(edit, "C&lear", [this] { clearNode(); });
    auto insert = menuBar()->addMenu("&Insert");
    action(insert, "Group...", [this] {
      bool ok;
      auto name =
          QInputDialog::getText(this, "Insert Group", "Group name:", QLineEdit::Normal, {}, &ok);
      if (ok)
        try {
          addNode(true, name);
        } catch (const std::exception& e) {
          error(e.what());
        }
    });
    action(insert, "Channel...", [this] {
      bool ok;
      auto name =
          QInputDialog::getText(this, "Insert Channel", "PV name:", QLineEdit::Normal, {}, &ok);
      if (ok)
        try {
          addNode(false, name);
        } catch (const std::exception& e) {
          error(e.what());
        }
    });
    action(insert, "Configuration File...", [this] { open(true); });
  } else {
    auto a = menuBar()->addMenu("&Action");
    action(a, "Acknowledge Alarm", [this] {
      engine->acknowledge(selection);
      refresh();
    });
    action(a, "Display Guidance", [this] { guidance(); });
    action(a, "Start Related Process", [this] { related(); });
    action(a, "Force Process Variable...", [this] { forceDialog(); });
    action(a, "Force Mask...", [this] { masks(true); });
    action(a, "Modify Mask Settings...", [this] { masks(); });
    action(a, "Beep Severity...", [this] { beepSeverity(false); });
    action(a, "NoAck for One Hour", [this] {
      engine->noAck(selection, !engine->state(selection).noAckUntil);
      refresh();
    });
    if (options.broadcast) {
      a->addSeparator();
      action(a, "Send Message...", [this] { broadcast(0); });
      action(a, "Stop Alarm Logging...", [this] { broadcast(1); });
      action(a, "Reload Facility...", [this] { broadcast(2); });
    }
  }
  auto view = menuBar()->addMenu("&View");
  action(
      view, "Expand One Level", [this] { treeView->expand(treeView->currentIndex()); },
      QKeySequence("+"));
  action(
      view, "Expand Branch", [this] { expandBranch(treeView->currentIndex()); }, QKeySequence("*"));
  action(view, "Expand All", [this] { treeView->expandAll(); }, QKeySequence("Ctrl+*"));
  action(
      view, "Collapse Branch", [this] { treeView->collapse(treeView->currentIndex()); },
      QKeySequence("-"));
  if (!options.editor) {
    action(view, "Current Alarm History", [this] { showHistory(); });
    action(view, "Configuration File",
           [this] { showText("Configuration File", writeConfig(doc)); });
    action(view, "Alarm Log File", [this] { showText("Alarm Log", logging->alarmPath(), true); });
    action(view, "Browser for Alarm Log",
           [this] { showLogBrowser(true); });
    action(view, "Operation Log File",
           [this] { showText("Operation Log", logging->opmodPath(), true); });
    action(view, "Browser for Operation Log",
           [this] { showLogBrowser(false); });
  }
  action(view, "Properties Window", [this] { properties(); });
  if (!options.editor) {
    auto setup = menuBar()->addMenu("&Setup");
    auto filterMenu = setup->addMenu("Display Filter...");
    auto filterGroup = new QActionGroup(filterMenu);
    const QStringList filters = {"No filter", "Active Alarms Only", "Unacknowledged Alarms Only"};
    for (int i = 0; i < filters.size(); ++i) {
      auto item = filterMenu->addAction(filters[i]);
      item->setCheckable(true); item->setChecked(options.filter == i);
      filterGroup->addAction(item);
      connect(item, &QAction::triggered, this, [this, i] {
        options.filter = i; treeModel->setFilter(i); groupModel->setFilter(i);
        treeView->expandAll();
      });
    }
    auto beepMenu = setup->addMenu("ALH Beep Severity...");
    auto beepGroup = new QActionGroup(beepMenu);
    for (int i = 1; i < 5; ++i) {
      auto item = beepMenu->addAction(severityName(i));
      item->setCheckable(true); item->setChecked(doc.beepSeverity == i);
      beepGroup->addAction(item);
      connect(item, &QAction::triggered, this, [this, i] { doc.beepSeverity = i; refresh(); });
    }
    connect(beepMenu, &QMenu::aboutToShow, this, [this, beepGroup] {
      for (auto action : beepGroup->actions()) action->setChecked(action->text() == severityName(doc.beepSeverity));
    });
    auto intervalMenu = setup->addMenu("Silence Time Interval");
    auto intervalActions = new QActionGroup(intervalMenu);
    for (int minutes : {5, 10, 15, 30, 60}) {
      auto item = intervalMenu->addAction(QString::number(minutes) + " minutes");
      item->setCheckable(true);
      item->setChecked(minutes == silenceMinutes);
      intervalActions->addAction(item);
      connect(item, &QAction::triggered, this, [this, minutes] {
        silenceMinutes = minutes;
        silenceBox->setText(QString("Silence %1 minutes").arg(minutes));
        if (engine->silenceUntil > engine->now())
          engine->silenceUntil = engine->now() + minutes * 60000;
        refresh();
      });
    }
    auto silenceForeverAction = setup->addAction("Silence Forever");
    silenceForeverAction->setCheckable(true);
    silenceForeverAction->setChecked(engine->silenceForever);
    connect(silenceForeverAction, &QAction::toggled, this, [this](bool yes) {
      options.silent = yes;
      engine->silenceForever = yes;
      refresh();
    });
    action(setup, "New Alarm Log File...", [this] {
      auto p = chooseFile(this, "Alarm Log File", logging->alarmPath(), "All files (*)", true);
      if (!p.isEmpty())
        logging->setAlarmFile(p);
    });
    action(setup, "New Operation Modification Log File...", [this] {
      auto p = chooseFile(this, "Operation Log File", logging->opmodPath(), "All files (*)", true);
      if (!p.isEmpty())
        logging->setOpmodFile(p);
    });
  }
  if (!options.editor) {
    const QHash<QString, QString> shortcuts = {
        {"Acknowledge Alarm", "Ctrl+A"},     {"Display Guidance", "Ctrl+G"},
        {"Start Related Process", "Ctrl+P"}, {"Force Process Variable...", "Ctrl+V"},
        {"Force Mask...", "Ctrl+M"},         {"Modify Mask Settings...", "Ctrl+S"},
        {"Beep Severity...", "Ctrl+B"},      {"NoAck for One Hour", "Ctrl+N"}};
    for (auto a : findChildren<QAction*>())
      if (shortcuts.contains(a->text()))
        a->setShortcut(QKeySequence(shortcuts[a->text()]));
  }
  QFont menuFont("monospace");
  menuFont.setPixelSize(13);
  menuFont.setStretch(100);
  menuBar()->setFont(menuFont);
  menuBar()->setFixedHeight(30);
  menuBar()->setStyleSheet(
      "QMenuBar {border:1px solid; border-top-color:#dde6e9; border-left-color:#dde6e9; "
      "border-bottom-color:#5f696d; border-right-color:#5f696d;} "
      "QMenuBar::item {padding:2px 7px; "
      "background:transparent;} QMenuBar::item:selected {border:1px inset #b0c3ca;}");
  for (auto menu : findChildren<QMenu*>())
    menu->setFont(menuFont);
  menuBar()->addSeparator();
  auto help = new QMenu("Help", this);
  auto helpButton = new QToolButton(menuBar());
  helpButton->setText("Help");
  helpButton->setFont(menuFont);
  helpButton->setStyleSheet(
      "QToolButton {border:0; padding:2px 10px;} QToolButton::menu-indicator {image:none;}");
  helpButton->setMenu(help);
  helpButton->setPopupMode(QToolButton::InstantPopup);
  menuBar()->setCornerWidget(helpButton, Qt::TopRightCorner);
  action(help, "Help Topics", [] {
    QDesktopServices::openUrl(QUrl("https://ops.aps.anl.gov/manuals/QtALH/"));
  });
  action(help, "About QtALH", [this] {
    QMessageBox::about(this, "About QtALH",
                       "QtALH — Qt port of ALH 1.2.35\nEPICS Alarm Handler and Alarm Configuration "
                       "Tool\nDeveloped at Argonne National Laboratory\n\n"
                       "Qt port development and maintenance:\nRobert Soliday\n\n"
                       "Original ALH authors:\nBen-Chin Cha, Janet Anderson, Mark Anderson,\n"
                       "Marty Kraimer, and Albert Kagarmanov\n\n"
                       "Additional ALH contributors:\nJohn Sinclair and Kay Kasemir (SNS)\n"
                       "Andreas Luedeke (PSI)\n\nQt " +
                           QString(qVersion()));
  });
}
void Window::replace(Document d, bool snapshot) {
  debugLog(options.debug, "config", snapshot ? "apply editor change" : "reload configuration");
  // Serialization emits channels before groups. Track the ordinal within each
  // kind so that a mixed child list cannot redirect an open Properties dialog.
  QVector<QPair<bool, int>> selectedPath;
  for (auto n = selection; n && n->parent; n = n->parent) {
    int ordinal = 0;
    for (const auto& child : n->parent->children) {
      if (child.get() == n) break;
      if (child->group == n->group) ++ordinal;
    }
    selectedPath.prepend(qMakePair(n->group, ordinal));
  }
  if (snapshot) {
    undo.push_back(writeConfig(doc));
    redo.clear();
    modified = true;
  }
  // Detach views before old model objects are released.
  for (auto dialog : findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly)) {
    if (selectionDialogs.contains(dialog->objectName()) &&
        selectionDialogs[dialog->objectName()].window == dialog) {
      clearDialogContent(dialog);
      selectionDialogs[dialog->objectName()].node = nullptr;
    } else {
      dialog->setEnabled(false);
      dialog->close();
    }
  }
  treeView->setModel(nullptr);
  groupView->setModel(nullptr);
  engine.reset();
  ca.reset();
  doc = std::move(d);
  setupEngine();
  treeModel->reset(&doc, engine.get());
  groupModel->reset(&doc, engine.get());
  treeView->setModel(treeModel);
  groupView->setModel(groupModel);
  connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            if (auto n = treeModel->node(i))
              select(n);
          });
  connect(groupView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            if (auto n = groupModel->node(i))
              selection = n;
            scheduleDialogSync();
          });
  auto restored = doc.root.get();
  for (const auto& step : selectedPath) {
    Node* next = nullptr;
    int ordinal = 0;
    for (const auto& child : restored->children)
      if (child->group == step.first && ordinal++ == step.second) {
        next = child.get();
        break;
      }
    if (!next) break;
    restored = next;
  }
  select(restored->group ? restored : restored->parent);
  selection = restored;
  treeView->expandAll();
  if (!options.editor)
    engine->start();
  scheduleHeartbeat();
  refresh();
}
void Window::open(bool insert) {
  auto path = chooseFile(this, "Alarm Configuration File", options.configDir,
                         "Alarm configurations (*.alhConfig);;All files (*)");
  if (path.isEmpty())
    return;
  try {
    auto d = loadConfig(path, options.configDir);
    if (insert) {
      auto parent = selection->group ? selection : selection->parent;
      auto previous = writeConfig(doc);
      auto c = cloneNode(*d.root, parent);
      parent->children.push_back(c);
      auto next = writeConfig(doc);
      parent->children.pop_back();
      auto result = parseConfig(next);
      result.filename = doc.filename;
      replace(std::move(result));
    } else {
      auto o = options;
      o.config = path;
      auto w = new Window(std::move(d), o, connectPv);
      w->showInitial();
    }
  } catch (const std::exception& e) {
    error(e.what());
  }
}
void Window::save(bool as) {
  auto path = doc.filename;
  if (as || path.isEmpty())
    path = chooseFile(this, "Save Configuration", path, "Alarm configurations (*.alhConfig)", true);
  if (!path.isEmpty())
    try {
      saveTo(path);
    } catch (const std::exception& e) {
      error(e.what());
    }
}
void Window::saveTo(const QString& p) {
  Document saved = doc;
  if (!options.editor) {
    saved.root = cloneNode(*doc.root);
    auto originals = doc.nodes(), copies = saved.nodes();
    for (int i = 0; i < originals.size(); ++i)
      if (!originals[i]->group)
        copies[i]->mask = engine->state(originals[i]).mask;
  }
  saveConfig(saved, p);
  // Runtime Save As exports the current masks, as legacy ALH does. Keep the
  // monitored facility bound to its original reload, broadcast and lock files.
  if (options.editor)
    doc.filename = p;
  modified = false;
  refresh();
}
void Window::addNode(bool group, const QString& name) {
  if (!options.editor)
    throw ParseError("Configuration edits require -c mode");
  if (name.isEmpty() || name.contains(QRegularExpression("\\s")))
    throw ParseError("Name must be a nonempty token");
  Node* parent = selection && selection->group ? selection
                 : selection                   ? selection->parent
                                               : doc.root.get();
  if (!parent)
    throw ParseError("Select a parent group");
  auto n = std::make_shared<Node>();
  n->group = group;
  n->name = name;
  n->parent = parent;
  parent->children.push_back(n);
  auto text = writeConfig(doc);
  parent->children.pop_back();
  auto d = parseConfig(text);
  d.filename = doc.filename;
  replace(std::move(d));
}
void Window::undoEdit() {
  if (undo.isEmpty())
    return;
  redo.push_back(writeConfig(doc));
  auto d = parseConfig(undo.takeLast());
  d.filename = doc.filename;
  replace(std::move(d), false);
  modified = true;
  refresh();
}
void Window::redoEdit() {
  if (redo.isEmpty())
    return;
  undo.push_back(writeConfig(doc));
  auto d = parseConfig(redo.takeLast());
  d.filename = doc.filename;
  replace(std::move(d), false);
  modified = true;
  refresh();
}
void Window::cut(bool remove) {
  if (!selection)
    return;
  clipboard = cloneNode(*selection);
  Document copied;
  copied.root = std::make_shared<Node>();
  copied.root->group = true;
  copied.root->name = "__QTALH_CLIPBOARD__";
  copied.root->children.push_back(cloneNode(*selection, copied.root.get()));
  auto mime = new QMimeData;
  mime->setText(selection->name);
  mime->setData("application/x-qtalh-config", writeConfig(copied).toUtf8());
  QApplication::clipboard()->setMimeData(mime);
  if (remove)
    clearNode();
}
void Window::clearNode() {
  if (!selection || !selection->parent) {
    error("The root group cannot be removed.");
    return;
  }
  auto parent = selection->parent;
  auto& c = parent->children;
  auto i = std::find_if(c.begin(), c.end(), [this](auto& p) { return p.get() == selection; });
  auto backup = *i;
  auto offset = i - c.begin();
  c.erase(i);
  auto text = writeConfig(doc);
  c.insert(c.begin() + offset, backup);
  auto d = parseConfig(text);
  d.filename = doc.filename;
  replace(std::move(d));
}
void Window::paste() {
  auto mime = QApplication::clipboard()->mimeData();
  if (mime && mime->hasFormat("application/x-qtalh-config")) {
    try {
      auto copied = parseConfig(QString::fromUtf8(mime->data("application/x-qtalh-config")));
      if (!copied.root->children.empty())
        clipboard = cloneNode(*copied.root->children.front());
    } catch (const std::exception& e) {
      error(e.what());
      return;
    }
  }
  if (!clipboard)
    return;
  auto p = selection->group ? selection : selection->parent;
  if (!p)
    return;
  auto c = cloneNode(*clipboard, p);
  p->children.push_back(c);
  auto text = writeConfig(doc);
  p->children.pop_back();
  try {
    auto d = parseConfig(text);
    d.filename = doc.filename;
    replace(std::move(d));
  } catch (const std::exception& e) {
    error(e.what());
  }
}
QString Window::selectionState() const {
  if (!selection) return {};
  const auto& state = engine->state(selection);
  QString result = selection->name + '\n' + selection->mask.text() + '\n' + state.mask.text();
  for (int count : state.maskCounts) result += ':' + QString::number(count);
  result += QString("/%1/%2/%3/%4")
                .arg(state.beepThreshold).arg(state.forceDisabled).arg(state.noAckUntil)
                .arg(doc.beepSeverity);
  for (const auto& directive : selection->directives)
    result += '\n' + directive.key + ' ' + directive.value;
  return result;
}
QDialog* Window::beginSelectionDialog(const QString& key, const QString& title,
                                     std::function<void()> build) {
  auto& entry = selectionDialogs[key];
  if (entry.window && rebuildingDialog != key) {
    entry.window->show();
    entry.window->raise();
    entry.window->activateWindow();
    return nullptr;
  }
  if (!entry.window) {
    entry.window = new QDialog(this);
    entry.window->setAttribute(Qt::WA_DeleteOnClose);
    entry.window->setObjectName(key);
    auto dialog = entry.window.data();
    connect(dialog, &QDialog::finished, this, [this, key, dialog] {
      if (selectionDialogs[key].window == dialog) selectionDialogs.remove(key);
    });
  }
  entry.build = std::move(build);
  entry.node = selection;
  entry.state = selectionState();
  entry.window->setProperty("pendingEdits", false);
  entry.window->setWindowTitle(title);
  return entry.window;
}
void Window::finishSelectionDialog(QDialog* dialog) {
  auto edited = [dialog] { dialog->setProperty("pendingEdits", true); };
  for (auto field : dialog->findChildren<QLineEdit*>())
    connect(field, &QLineEdit::textEdited, dialog, edited);
  for (auto field : dialog->findChildren<QPlainTextEdit*>())
    if (!field->isReadOnly()) connect(field, &QPlainTextEdit::textChanged, dialog, edited);
  for (auto field : dialog->findChildren<QCheckBox*>())
    connect(field, &QCheckBox::clicked, dialog, edited);
  dialog->show();
}
void Window::clearDialogContent(QDialog* dialog) {
  delete dialog->layout();
  qDeleteAll(dialog->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
  qDeleteAll(dialog->findChildren<QButtonGroup*>(QString(), Qt::FindDirectChildrenOnly));
}
void Window::rebuildSelectionDialog(const QString& key) {
  auto entry = selectionDialogs.value(key);
  if (!entry.window || !selection) return;
  auto geometry = entry.window->geometry();
  clearDialogContent(entry.window);
  rebuildingDialog = key;
  entry.build();
  rebuildingDialog.clear();
  entry.window->setGeometry(geometry);
}
void Window::scheduleDialogSync() {
  if (dialogSyncPending || selectionDialogs.isEmpty()) return;
  dialogSyncPending = true;
  // Rebuild after the current input callback returns; its controls may be replaced.
  QTimer::singleShot(0, this, [this] {
    dialogSyncPending = false;
    const auto state = selectionState();
    for (const auto& key : selectionDialogs.keys()) {
      const auto entry = selectionDialogs.value(key);
      if (!entry.window) continue;
      auto modal = QApplication::activeModalWidget();
      if (modal && (modal == entry.window || entry.window->isAncestorOf(modal))) continue;
      if (entry.node == selection) {
        if (auto summary = entry.window->findChild<QLabel*>("currentMaskSummary")) {
          const auto& current = engine->state(selection);
          QString mask = current.mask.text();
          if (selection->group)
            for (int i = 0; i < 5; ++i) mask[i] = current.maskCounts[i] ? QString("CDATL")[i] : QChar('-');
          summary->setText("Current Mask Summary: " + mask);
        }
      }
      if (entry.node != selection ||
          (entry.state != state && !entry.window->property("pendingEdits").toBool()))
        rebuildSelectionDialog(key);
    }
  });
}
void Window::properties() {
  if (!selection) return;
  auto original = selection;
  auto dialog = beginSelectionDialog("propertiesDialog", "Alarm Handler Properties",
                                     [this] { properties(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  auto scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto body = new QWidget;
  auto form = dialogColumn(body);
  scroll->setWidget(body); layout->addWidget(scroll);
  QHash<QString, QLineEdit*> edits;
  auto field = [&](const QString& key, const QString& value) {
    auto edit = new QLineEdit(value, body);
    edit->setObjectName("property" + key);
    edit->setReadOnly(!options.editor);
    edits[key] = edit;
    return edit;
  };
  auto line = [&](QVBoxLayout* target, const QString& label, const QString& key) {
    auto row = dialogRow(target);
    row->addWidget(new QLabel(label));
    auto edit = field(key, original->option(key));
    row->addWidget(edit, 1);
    return edit;
  };
  auto top = dialogRow(form);
  top->addWidget(new QLabel(original->group ? "Group" : "Channel"));
  auto name = field("NAME", original->name); top->addWidget(name, 1);
  auto masks = dialogRow(form);
  auto maskColumn = new QVBoxLayout;
  auto mask = field("MASK", original->mask.text());
  mask->setMaximumWidth(65);
  if (options.editor && !original->group) {
    auto row = new QHBoxLayout;
    row->addWidget(new QLabel("Alarm Mask")); row->addWidget(mask); row->addStretch();
    maskColumn->addLayout(row);
  } else {
    mask->hide();
    QString summary = engine->state(original).mask.text();
    if (original->group)
      for (int i = 0; i < 5; ++i)
        summary[i] = engine->state(original).maskCounts[i] ? QString("CDATL")[i] : QChar('-');
    maskColumn->addWidget(new QLabel("Current Mask " + summary));
    maskColumn->addWidget(new QLabel("Reset Mask   " + (original->group ? QString() : original->mask.text())));
  }
  masks->addLayout(maskColumn, 1);
  auto filterWidget = new QWidget;
  auto filterLayout = dialogColumn(filterWidget);
  auto filterFrame = dialogFrame(filterLayout, "Alarm Count Filter");
  auto filterRow = dialogRow(filterFrame);
  auto countParts = original->option("ALARMCOUNTFILTER").split(' ', Qt::SkipEmptyParts);
  auto count = field("COUNT", countParts.value(0)), seconds = field("SECONDS", countParts.value(1));
  count->setMaximumWidth(42); seconds->setMaximumWidth(42);
  count->setReadOnly(!options.editor || original->group);
  seconds->setReadOnly(!options.editor || original->group);
  filterRow->addWidget(new QLabel("Count")); filterRow->addWidget(count);
  filterRow->addWidget(new QLabel("Seconds")); filterRow->addWidget(seconds);
  masks->addWidget(filterWidget, 1);
  auto forceFrame = dialogFrame(form, "Force Process Variable");
  auto forceParts = original->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
  auto forceRow = dialogRow(forceFrame);
  forceRow->addWidget(new QLabel("PV Name"));
  auto forceName = field("FORCE_NAME", forceParts.value(0)); forceRow->addWidget(forceName, 1);
  auto forceValues = dialogRow(forceFrame);
  for (int i = 1; i <= 3; ++i) {
    forceValues->addWidget(new QLabel(QStringList{"Force Mask", "Force Value", "Reset Value"}[i - 1]));
    auto edit = field("FORCE_" + QString::number(i), forceParts.value(i));
    edit->setMaximumWidth(65); forceValues->addWidget(edit);
  }
  auto calc = dialogFrame(form);
  auto expressionRow = dialogRow(calc);
  expressionRow->addWidget(new QLabel("Force CALC     Expression"));
  expressionRow->addWidget(field("FORCEPV_CALC", original->option("FORCEPV_CALC")), 1);
  auto grid = new QGridLayout;
  for (int i = 0; i < 6; ++i) {
    QString letter(QChar('A' + i)), key = "FORCEPV_CALC_" + letter;
    grid->addWidget(new QLabel(letter), i % 3, (i / 3) * 2);
    grid->addWidget(field(key, original->option(key)), i % 3, (i / 3) * 2 + 1);
  }
  calc->addLayout(grid);
  auto beep = line(form, "Beep Severity", "BEEPSEVR");
  if (!options.editor && beep->text().isEmpty())
    beep->setText(severityName(engine->state(original).beepThreshold));
  beep->setMaximumWidth(80);
  // Keep the short severity field next to its label, as in the Motif form.
  auto beepRow = qobject_cast<QHBoxLayout*>(form->itemAt(form->count() - 1)->layout());
  beepRow->setStretch(1, 0);
  beepRow->addStretch();
  line(form, "Severity PV Name", "SEVRPV");
  if (!original->group) line(form, "Acknowledgement PV / Value", "ACKPV");
  if (!original->parent) line(form, "Heartbeat PV / Interval / Value", "HEARTBEATPV");
  line(form, "Alias", "ALIAS");
  form->addWidget(new QLabel("Related Process Command"));
  form->addWidget(field("COMMAND", original->option("COMMAND")));
  auto area = [&](const QString& label, const QString& key, int height) {
    form->addWidget(new QLabel(label));
    QStringList lines;
    for (const auto& directive : original->directives)
      if (directive.key == key) lines << directive.value;
    auto edit = new QPlainTextEdit(lines.join('\n'));
    edit->setObjectName("property" + key);
    edit->setReadOnly(!options.editor);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setFixedHeight(height);
    form->addWidget(edit);
    return edit;
  };
  auto severity = area("Alarm Severity Commands", "SEVRCOMMAND", 52);
  auto status = area("Alarm Status Commands", "STATCOMMAND", 52);
  auto url = area("Guidance URL", "GUIDANCE", 30);
  auto guidance = area("Guidance Text", "GUIDANCE_TEXT", 100);
  form->addStretch();
  auto buttons = dialogActions(layout, dialog,
      options.editor ? QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::Close
                     : QDialogButtonBox::Close,
      "Properties show the selected group or channel configuration. In configuration mode, "
      "edit fields and use Apply to apply them. Cancel restores the current configuration; Dismiss closes the window. Invalid configurations leave the original unchanged.");
  if (options.editor) {
    disconnect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked, dialog, [this] {
      QTimer::singleShot(0, this, [this] { rebuildSelectionDialog("propertiesDialog"); });
    });
  }
  if (options.editor) connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
    try {
      Document candidate;
      candidate.root = cloneNode(*doc.root);
      candidate.filename = doc.filename; candidate.beepSeverity = doc.beepSeverity;
      auto n = candidate.nodes().at(doc.nodes().indexOf(original));
      n->name = name->text().trimmed(); n->directives.clear();
      if (!n->group) n->mask = Mask::parse(mask->text());
      for (const auto& key : {"ALIAS", "COMMAND", "SEVRPV", "BEEPSEVR", "FORCEPV_CALC",
                              "FORCEPV_CALC_A", "FORCEPV_CALC_B", "FORCEPV_CALC_C",
                              "FORCEPV_CALC_D", "FORCEPV_CALC_E", "FORCEPV_CALC_F"})
        n->setOption(key, edits[key]->text());
      if (!forceName->text().trimmed().isEmpty()) {
        QString force = forceName->text().trimmed();
        for (int i = 1; i <= 3; ++i) force += ' ' + edits["FORCE_" + QString::number(i)]->text();
        n->setOption("FORCEPV", force);
      }
      if (!n->group) {
        n->setOption("ACKPV", edits["ACKPV"]->text());
        if (!count->text().isEmpty() || !seconds->text().isEmpty())
          n->setOption("ALARMCOUNTFILTER", count->text() + ' ' + seconds->text());
      }
      if (!n->parent) n->setOption("HEARTBEATPV", edits["HEARTBEATPV"]->text());
      for (auto item : {qMakePair(severity, QString("SEVRCOMMAND")),
                        qMakePair(status, QString("STATCOMMAND")), qMakePair(url, QString("GUIDANCE"))})
        for (auto value : item.first->toPlainText().split('\n'))
          if (!value.trimmed().isEmpty()) n->directives.push_back({item.second, value.trimmed()});
      n->setOption("GUIDANCE_TEXT", guidance->toPlainText());
      auto checked = parseConfig(writeConfig(candidate)); checked.filename = doc.filename;
      replace(std::move(checked));
    } catch (const std::exception& e) {
      QMessageBox::warning(dialog, "Invalid properties", e.what());
    }
  });
  dialog->resize(500, 730);
  finishSelectionDialog(dialog);
}
void Window::masks(bool forced) {
  if (!selection) return;
  if (options.editor) { properties(); return; }
  auto n = selection;
  auto dialog = beginSelectionDialog(forced ? "forceMaskDialog" : "modifyMaskDialog",
                                     forced ? "Force Mask" : "Modify Mask Settings",
                                     [this, forced] { masks(forced); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group Name:" : "Channel Name:", n->label());
  auto summary = [this, n] {
    if (!n->group) return engine->state(n).mask.text();
    QString value = "-----";
    for (int i = 0; i < 5; ++i)
      if (engine->state(n).maskCounts[i]) value[i] = QString("CDATL")[i];
    return value;
  };
  if (forced) {
    auto current = new QLabel("Current Mask Summary: " + summary());
    current->setObjectName("currentMaskSummary");
    current->setAlignment(Qt::AlignCenter);
    layout->addWidget(current);
    auto reset = new QLabel("Reset Mask: " + (n->group ? QString() : n->mask.text()));
    reset->setAlignment(Qt::AlignCenter);
    layout->addWidget(reset);
    auto value = new QLabel("Mask: " + summary());
    value->setAlignment(Qt::AlignCenter);
    layout->addWidget(value);
    auto choicesColumn = dialogRow(layout);
    choicesColumn->addStretch(1);
    auto choicesPanel = new QVBoxLayout;
    choicesColumn->addLayout(choicesPanel, 3);
    auto choices = maskChoices(choicesPanel, summary(), options.engine.passive);
    for (auto choice : choices)
      connect(choice, &QCheckBox::toggled, dialog,
              [choices, value] { value->setText("Mask: " + selectedMask(choices)); });
    auto buttons = dialogActions(layout, dialog,
        QDialogButtonBox::Apply | QDialogButtonBox::Reset | QDialogButtonBox::Close,
        "Apply sets the chosen mask on the selected channel or every channel in the group. "
        "Reset restores each channel's configured mask.");
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
      engine->cancelNoAckTimer(n);
      engine->setMask(n, Mask::parse(selectedMask(choices)));
      current->setText("Current Mask Summary: " + summary());
      dialog->setProperty("pendingEdits", false);
      refresh();
    });
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, dialog, [=] {
      engine->cancelNoAckTimer(n);
      engine->resetMask(n);
      auto mask = summary();
      for (int i = 0; i < choices.size(); ++i) choices[i]->setChecked(mask[i] != '-');
      current->setText("Current Mask Summary: " + mask);
      dialog->setProperty("pendingEdits", false);
      refresh();
    });
    dialog->resize(277, 244);
  } else {
    auto grid = new QGridLayout;
    grid->setHorizontalSpacing(6); grid->setVerticalSpacing(6);
    const QStringList labels = {"Add/Cancel Alarms", "Enable/Disable Alarms", "Ack/NoAck Alarms",
                                 "Ack/NoAck Transient Alarms", "Log/NoLog Alarms"};
    const QStringList off = {"Add", "Enable", "Ack", "AckT", "Log"};
    const QStringList on = {"Cancel", "Disable", "NoAck", "NoAckT", "NoLog"};
    for (int bit = 0; bit < 5; ++bit) {
      grid->addWidget(new QLabel(labels[bit]), bit, 0);
      for (int choice = 0; choice < 3; ++choice) {
        auto button = new QPushButton(choice == 0 ? off[bit] : choice == 1 ? on[bit] : "Reset");
        button->setObjectName(QString("maskAction%1_%2").arg(bit).arg(choice));
        button->setEnabled(!options.engine.passive || bit != AckT);
        grid->addWidget(button, bit, choice + 1);
        connect(button, &QPushButton::clicked, dialog, [this, n, bit, choice] {
          if (bit == Ack) engine->cancelNoAckTimer(n);
          std::function<void(Node*)> apply = [&](Node* c) {
            if (c->group) {
              for (auto& child : c->children) apply(child.get());
            } else {
              auto mask = engine->state(c).mask;
              mask[bit] = choice == 2 ? c->mask[bit] : choice == 1;
              engine->setMask(c, mask);
            }
          };
          apply(n); refresh();
        });
      }
    }
    layout->addLayout(grid);
    dialogActions(layout, dialog, QDialogButtonBox::Close,
                  "Change one mask field for the selected channel or group. Reset uses each "
                  "channel's configured value. Other mask fields are preserved.");
    dialog->resize(336, 202);
  }
  finishSelectionDialog(dialog);
}
void Window::forceDialog() {
  auto n = selection;
  if (!n) return;
  auto dialog = beginSelectionDialog("forcePvDialog", "Force Process Variable",
                                     [this] { forceDialog(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group Name:" : "Channel Name:", n->label());
  auto settings = dialogFrame(layout);
  auto disabled = new QCheckBox("ForcePV Disabled");
  disabled->setChecked(engine->state(n).forceDisabled);
  settings->addWidget(disabled);
  settings->addSpacing(8);
  settings->addWidget(new QLabel("Force Process Variable Name (or CALC):"));
  auto words = n->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
  auto pvName = new QLineEdit(words.value(0));
  pvName->setObjectName("forcePvName");
  settings->addWidget(pvName);
  auto value = new QLabel("Force Mask " + words.value(1, "-----"));
  settings->addWidget(value);
  auto choices = maskChoices(settings, words.value(1, "-----"));
  for (auto box : choices)
    connect(box, &QCheckBox::toggled, dialog,
            [value, choices] { value->setText("Force Mask " + selectedMask(choices)); });
  auto force = new QLineEdit(words.value(2, "1")), reset = new QLineEdit(words.value(3, "0"));
  force->setObjectName("forceValue"); reset->setObjectName("forceReset");
  auto values = new QFormLayout;
  values->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
  force->setMaximumWidth(75); reset->setMaximumWidth(75);
  values->addRow("Force Value", force); values->addRow("Reset Value", reset);
  settings->addLayout(values);
  auto calc = dialogFrame(settings, "Force CALC");
  calc->addWidget(new QLabel("Expression:"));
  auto expression = new QLineEdit(n->option("FORCEPV_CALC"));
  expression->setObjectName("forceExpression");
  calc->addWidget(expression);
  QVector<QLineEdit*> inputs;
  for (int i = 0; i < 6; ++i) {
    auto row = dialogRow(calc);
    row->addWidget(new QLabel(QString(QChar('A' + i))));
    auto field = new QLineEdit(n->option("FORCEPV_CALC_" + QString(QChar('A' + i))));
    field->setObjectName("forceInput" + QString(QChar('A' + i)));
    inputs << field; row->addWidget(field);
  }
  auto buttons = dialogActions(layout, dialog,
      QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::Close,
      "A Force PV applies the selected mask at the force value and restores configured masks "
      "at the reset value. CALC accepts an expression with A through F PVs or constants.");
  disconnect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, dialog, &QDialog::close);
  connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked, dialog, [=] {
    const auto saved = n->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    pvName->setText(saved.value(0));
    const auto mask = saved.value(1, "-----");
    for (int i = 0; i < choices.size(); ++i) choices[i]->setChecked(mask[i] != '-');
    force->setText(saved.value(2, "1")); reset->setText(saved.value(3, "0"));
    disabled->setChecked(engine->state(n).forceDisabled);
    expression->setText(n->option("FORCEPV_CALC"));
    for (int i = 0; i < inputs.size(); ++i)
      inputs[i]->setText(n->option("FORCEPV_CALC_" + QString(QChar('A' + i))));
    dialog->setProperty("pendingEdits", false);
  });
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
    try {
      QString text = "GROUP NULL check\n";
      if (!pvName->text().trimmed().isEmpty())
        text += "$FORCEPV " + pvName->text().trimmed() + " " + selectedMask(choices) + " " +
                force->text() + " " + reset->text() + "\n";
      if (pvName->text().trimmed() == "CALC") {
        text += "$FORCEPV_CALC " + expression->text() + "\n";
        for (int i = 0; i < 6; ++i)
          if (!inputs[i]->text().isEmpty())
            text += "$FORCEPV_CALC_" + QString(QChar('A' + i)) + " " + inputs[i]->text() + "\n";
      }
      auto checked = parseConfig(text);
      engine->configureForce(n, checked.root->directives, disabled->isChecked());
      dialog->setProperty("pendingEdits", false);
      dirty = true;
    } catch (const std::exception& e) {
      QMessageBox::warning(dialog, "Invalid Force PV", e.what());
    }
  });
  dialog->resize(276, 494);
  finishSelectionDialog(dialog);
}
void Window::beepSeverity(bool global) {
  auto n = selection;
  if (!global && !n) return;
  auto dialog = beginSelectionDialog("beepSeverityDialog", "Set Beep Severity",
                                     [this, global] { beepSeverity(global); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, global ? "Facility:" : n->group ? "Group Name:" : "Channel Name:",
                global ? doc.root->label() : n->label());
  auto severityRow = dialogRow(layout);
  severityRow->addStretch();
  auto severityColumn = new QVBoxLayout;
  severityRow->addLayout(severityColumn);
  severityRow->addStretch();
  auto frame = dialogFrame(severityColumn);
  auto buttons = new QButtonGroup(dialog);
  for (int severity = 1; severity < 5; ++severity) {
    auto button = new QRadioButton(severityName(severity));
    button->setObjectName("beepSeverity" + QString::number(severity));
    button->setChecked((global ? doc.beepSeverity : engine->state(n).beepThreshold) == severity);
    frame->addWidget(button); buttons->addButton(button, severity);
    connect(button, &QRadioButton::clicked, dialog, [this, global, n, severity] {
      int previous = global ? doc.beepSeverity : engine->state(n).beepThreshold;
      if (severity == previous) return;
      if (options.editor) {
        undo.push_back(writeConfig(doc)); redo.clear(); modified = true;
      }
      if (global) doc.beepSeverity = severity;
      else engine->setBeep(n, severity);
      refresh();
    });
  }
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "Select the lowest unacknowledged severity that should sound an alarm.");
  dialog->resize(170, 178);
  finishSelectionDialog(dialog);
}
void Window::guidance() {
  auto url = selection->option("GUIDANCE");
  if (!url.isEmpty()) {
    QUrl u =
        QUrl::fromUserInput(url, QFileInfo(doc.filename).absolutePath(), QUrl::AssumeLocalFile);
    if (!QDesktopServices::openUrl(u))
      error("Cannot open guidance: " + url);
  } else
    showText("Guidance: " + selection->name,
             selection->option("GUIDANCE_TEXT", "No guidance available."));
}
void Window::related() {
  auto text = selection->option("COMMAND");
  if (text.isEmpty())
    return;
  auto entries = text.split(QRegularExpression("[!\\n]"), Qt::SkipEmptyParts);
  if (entries.isEmpty())
    return;
  if (entries.size() > 1 && entries.size() % 2) {
    error("Related process command requires label/command pairs.");
    return;
  }
  if (entries.size() > 1) {
    auto menu = new QMenu(this);
    for (int i = 0; i + 1 < entries.size(); i += 2) {
      auto a = menu->addAction(entries[i].trimmed());
      QString command = entries[i + 1].trimmed();
      connect(a, &QAction::triggered, this, [this, command] { engine->command(command); });
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QCursor::pos());
  } else
    engine->command(entries.front().trimmed());
}
void Window::showText(const QString& title, const QString& content, bool fromFile) {
  auto dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(title);
  dialog->resize(fromFile ? 800 : 600, fromFile ? 400 : 300);
  auto l = dialogColumn(dialog);
  if (fromFile) l->addWidget(new QLabel("File: " + content));
  auto text = new QPlainTextEdit;
  text->setReadOnly(true);
  text->setFont(font());
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  l->addWidget(text);
  auto update = [text, content, fromFile] {
    QString body = content;
    if (fromFile) {
      QFile f(content);
      body = f.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(f.readAll()) : f.errorString();
    }
    if (body != text->toPlainText()) {
      auto scroll = text->verticalScrollBar();
      int value = scroll->value();
      bool bottom = value == scroll->maximum();
      text->setPlainText(body);
      scroll->setValue(bottom ? scroll->maximum() : value);
    }
  };
  update();
  if (fromFile) {
    auto t = new QTimer(dialog);
    t->setInterval(1000);
    connect(t, &QTimer::timeout, dialog, update);
    t->start();
  }
  auto row = new QHBoxLayout;
  row->addWidget(new QLabel("Search:"));
  auto search = new QLineEdit;
  row->addWidget(search);
  for (bool reverse : {false, true}) {
    auto b = new QPushButton(reverse ? "Reverse" : "Forward");
    row->addWidget(b);
    connect(b, &QPushButton::clicked, dialog, [text, search, reverse] {
      if (!text->find(search->text(),
                      reverse ? QTextDocument::FindBackward : QTextDocument::FindFlags())) {
        text->moveCursor(reverse ? QTextCursor::End : QTextCursor::Start);
        text->find(search->text(),
                   reverse ? QTextDocument::FindBackward : QTextDocument::FindFlags());
      }
    });
  }
  l->addLayout(row);
  dialogActions(l, dialog, QDialogButtonBox::Close);
  dialog->show();
}
void Window::showHistory() {
  if (historyDialog) { historyDialog->show(); historyDialog->raise(); return; }
  auto d = new QDialog(this);
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setObjectName("historyDialog");
  d->setWindowTitle("Alarm Handler: Current Alarm History");
  auto l = dialogColumn(d);
  auto heading = dialogRow(l);
  auto close = new QPushButton("Close");
  heading->addWidget(close);
  heading->addWidget(new QLabel("TIME_STAMP       PROCESS_VARIABLE_NAME          STATUS     SEVERITY   VALUE"), 1);
  connect(close, &QPushButton::clicked, d, &QDialog::close);
  auto text = new QPlainTextEdit;
  auto historyFont = font();
  historyFont.setStretch(100);
  text->setFont(historyFont);
  text->setReadOnly(true);
  text->setFrameShape(QFrame::NoFrame);
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  l->addWidget(text);
  historyDialog = d; historyText = text;
  refresh();
  d->resize(720, 225); d->show();
}
void Window::broadcast(int mode) {
  if (!logging) return;
  auto dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("broadcastDialog");
  dialog->setWindowTitle("ALH MessageEntryDialog");
  auto layout = dialogColumn(dialog);
  layout->addWidget(new QLabel("Type message (See help for detail):"));
  auto text = new QLineEdit(mode == 2 ? "Reload config. Reason: " : "");
  text->setObjectName("broadcastMessage");
  layout->addWidget(text);
  auto minutes = new QSpinBox;
  minutes->setRange(1, 10); minutes->setValue(1);
  if (mode == 1) {
    auto row = dialogRow(layout);
    row->addWidget(new QLabel("Minutes without alarm logging:")); row->addWidget(minutes);
  } else {
    minutes->setParent(dialog);
    minutes->hide();
  }
  auto buttons = dialogActions(layout, dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
      "Messages are delivered to other ALH instances using the same facility configuration. "
      "Stop Logging suspends alarm logging for the chosen duration. Reload rereads the facility configuration.");
  connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, text, minutes, mode] {
    if (logging->sendBroadcast(text->text(), mode == 1 ? minutes->value() : 0, mode == 2)) dialog->close();
    else error("Could not broadcast message");
  });
  dialog->resize(390, mode == 1 ? 135 : 105); dialog->show();
}
void Window::exitApplication() {
  if (exitPopup) {
    exitPopup->raise();
    exitPopup->activateWindow();
    return;
  }
  auto parent = isVisible() ? static_cast<QWidget*>(this) : runtime;
  exitPopup = new QMessageBox(QMessageBox::Warning, "Alarm Handler", "Exit Alarm Handler?",
                              QMessageBox::Ok | QMessageBox::Cancel, parent);
  exitPopup->setObjectName("exitConfirmation");
  exitPopup->setDefaultButton(QMessageBox::Cancel);
  exitPopup->setAttribute(Qt::WA_DeleteOnClose);
  exitPopup->setWindowModality(Qt::ApplicationModal);
  connect(exitPopup, &QDialog::finished, this, [this](int result) {
    exitPopup = nullptr;
    if (result == QMessageBox::Ok) {
      quitting = true;
      close();
    }
  });
  exitPopup->show();
}
bool Window::eventFilter(QObject* object, QEvent* event) {
  if (object == runtime && event->type() == QEvent::Close) {
    exitApplication();
    event->ignore();
    return true;
  }
  return QMainWindow::eventFilter(object, event);
}
void Window::closeEvent(QCloseEvent* event) {
  if (!options.editor && !quitting) {
    hide();
    event->ignore();
    return;
  }
  if (modified) {
    auto answer =
        QMessageBox::question(this, "Exit ALH", "Save configuration changes?",
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) {
      event->ignore();
      quitting = false;
      return;
    }
    if (answer == QMessageBox::Save) {
      save();
      if (modified) {
        event->ignore();
        quitting = false;
        return;
      }
    }
  }
  if (runtime)
    runtime->hide();
  event->accept();
}
} // namespace alh
