#pragma once
#include "core/engine.h"
#include "appearance.h"
#include "services/pv_router.h"
#include "services/logging.h"
#include "services/options.h"
#include "services/process_limits.h"
#include "services/notifications.h"
#include "services/analytics.h"
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeView>
namespace alh {
class AlarmModel : public QAbstractItemModel {
public:
  AlarmModel(Document*, Engine*, bool tree, QObject* parent = nullptr);
  QModelIndex index(int, int, const QModelIndex& = {}) const override;
  QModelIndex parent(const QModelIndex&) const override;
  int rowCount(const QModelIndex& = {}) const override;
  int columnCount(const QModelIndex& = {}) const override {
    return 9;
  }
  QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex&) const override;
  QStringList mimeTypes() const override;
  QMimeData* mimeData(const QModelIndexList&) const override;
  Node* node(const QModelIndex& i) const {
    return i.isValid() ? static_cast<Node*>(i.internalPointer()) : nullptr;
  }
  void reset(Document*, Engine*);
  void setGroup(Node*);
  void setFilter(int);
  void refresh();
  bool coloredMasks = false;
  // How a node differs from its configuration in this runtime (non-zero marks
  // its name), and the tooltip lines describing it. Unset in the editor.
  std::function<int(Node*)> modifications;
  std::function<QString(Node*)> modificationText;

private:
  Document* doc;
  Engine* engine;
  bool tree;
  Node* group = nullptr;
  int filter = 0;
  mutable QHash<Node*, QVector<Node*>> childCache;
  QVector<Node*> children(Node*) const;
  bool visible(Node*) const;
  // Every input of the displayed text and colors. refresh() reports only rows
  // whose signature changed, so views repaint those rows instead of everything.
  struct RowSignature {
    std::array<int, 5> counts{};
    int severity = 0, unack = 0, shelved = 0, status = 0, beep = 0, flags = 0, modified = 0;
    QString label;
    bool operator==(const RowSignature& o) const {
      return counts == o.counts && severity == o.severity && unack == o.unack &&
             shelved == o.shelved && status == o.status && beep == o.beep && flags == o.flags &&
             modified == o.modified && label == o.label;
    }
    // The first displayed column whose content differs. Cells are laid out
    // consecutively, so everything from that column to the row's end repaints.
    int firstDifference(const RowSignature& o) const;
  };
  QHash<Node*, RowSignature> signatures;
  RowSignature signature(Node*) const;
  // Configuration facts shown on each row. Nodes change only through a
  // document replacement, which resets the model and this cache.
  struct NodeInfo {
    QString label;
    bool childGroups = false, guidance = false, command = false;
  };
  mutable QHash<Node*, NodeInfo> nodeInfo;
  const NodeInfo& info(Node*) const;
  // Fonts are requested for every painted and measured cell.
  QFont font(int column, bool group) const;
  mutable QFont legacyText, legacyName, legacyIndicator, styledBase;
  mutable std::array<QFont, 2> styledName;
  mutable bool legacyFonts = false, styledFonts = false;
};
class Window : public QMainWindow {
public:
  Window(Document, Options, bool connectPv = true);
  ~Window() override;
  Document& document() {
    return doc;
  }
  Engine& alarmEngine() {
    return *engine;
  }
  AnalyticsService* alarmAnalytics() { return analytics.get(); }
  void addNode(bool group, const QString& name);
  void undoEdit();
  void redoEdit();
  void showInitial();
  void saveTo(const QString&);
  void saveAlarmSnapshot(const QString&) const;
  // SIGINT under -B: the normal exit path without the confirmation popup.
  void exitOnInterrupt();

protected:
  void closeEvent(QCloseEvent*) override;
  bool eventFilter(QObject*, QEvent*) override;
  virtual void systemBeep();

private:
  Document doc;
  Options options;
  bool connectPv, modified = false, quitting = false, dirty = true;
  // INCLUDE file texts as last read or saved, by path: the editor rewrites
  // only those that change.
  QHash<QString, QString> savedIncludes;
  void rememberIncludes();
  std::unique_ptr<PvRouter> ca;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<Logging> logging;
  std::unique_ptr<NotificationService> notifications;
  std::unique_ptr<AnalyticsService> analytics;
  AlarmModel *treeModel = nullptr, *groupModel = nullptr;
  QTreeView *treeView = nullptr, *groupView = nullptr;
  QLabel *execution = nullptr, *filename = nullptr, *messageArea = nullptr, *beepLabel = nullptr,
         *silenceForeverLabel = nullptr, *disabledForceLabel = nullptr, *shelvedLabel = nullptr,
         *notificationsEnabledLabel = nullptr, *displayFilterLabel = nullptr,
         *runtimeNotificationsEnabledLabel = nullptr;
  QWidget *runtime = nullptr, *messageBanner = nullptr;
  QPushButton* runtimeButton = nullptr;
  QString runtimeSizedText;
  QFont runtimeSizedFont;
  QCheckBox *silenceBox = nullptr, *currentBox = nullptr;
  // The status footer and the classic pane-width scale, which ALH turns blue
  // during a silence interval (as are the panes, unless -maskcolor).
  QWidget *footer = nullptr, *paneWidth = nullptr;
  bool silenceColored = false;
  QTimer refreshTimer, statusTimer, beepTimer, heartbeatTimer;
  int statusTicks = 0;
  OpenFileMonitor openFileMonitor;
  int silenceMinutes = 30;
  QMediaPlayer* sound = nullptr;
  Node *selection = nullptr, *selectedGroup = nullptr;
  QStringList undo, redo;
  void rememberEdit(QStringList&, QString);
  // Dummy root holds notification destinations required by its copied child.
  Document clipboard;
  QAction *undoAction = nullptr, *redoAction = nullptr;
  QPointer<QDialog> historyDialog, shelfListDialog, notificationDialog, analyticsDialog;
  QPointer<QMessageBox> errorPopup, exitPopup;
  QVector<QPointer<QAction>> selectionActions;
  QPointer<QTableWidget> historyTable;
  // The engine history last shown; unchanged history leaves the view untouched.
  QVector<QString> shownHistory;
  // The operator's Current Alarm History sort; newest first by default.
  int historySortColumn = 0;
  Qt::SortOrder historySortOrder = Qt::DescendingOrder;
  void refreshHistory();
  struct SelectionDialog {
    QPointer<QDialog> window;
    Node* node = nullptr;
    QString state;
    std::function<void()> build;
  };
  QMap<QString, SelectionDialog> selectionDialogs;
  QString rebuildingDialog;
  bool dialogSyncPending = false, dialogClosePending = false, auditingError = false;
  QDialog* beginSelectionDialog(const QString&, const QString&, std::function<void()>);
  void finishSelectionDialog(QDialog*);
  void clearDialogContent(QDialog*);
  void rebuildSelectionDialog(const QString&);
  void scheduleDialogSync();
  QString selectionState() const;
  // Operator tools (ui/tools.cc): active alarm list, search, alarm details,
  // runtime overrides, summary panel, and Phoebus XML import/export.
  QPointer<QDialog> activeAlarmsDialog, findDialog, overridesDialog, summaryDialog;
  // BEEPSEVR as loaded, when other than MINOR (the default for absent nodes);
  // Engine::setBeep rewrites the node's option at runtime.
  QHash<Node*, int> configuredBeep;
  int configuredBeepSeverity = 1;
  // Force PV settings as loaded, and nodes whose settings the Force PV dialog
  // changed since. Both are rebuilt with the engine for each document.
  QHash<const Node*, QVector<Directive>> configuredForce;
  QSet<const Node*> forceEdited;
  int modifications(Node*) const;
  QString modificationText(Node*) const;
  void showActiveAlarms();
  void findAlarm();
  void alarmDetails();
  void showOverrides();
  void showSummary();
  void importPhoebus();
  void exportPhoebus();
  void showConversionNotes(const QString& title, const QStringList& notes, QWidget* parent);
  // Shows the main window with this group or channel selected.
  void reveal(Node*);
  // Updates open tool windows; cheap when none are open.
  void refreshTools();
  void activateRuntime();
  void showLogBrowser(bool alarm);
  void shelveDialog(Node*, bool change = false);
  void showShelvedAlarms();
  void showNotifications();
  void showAnalytics();
  void refreshShelfList();
  void setupEngine();
  void scheduleHeartbeat();
  void buildUi();
  void menus();
  void refresh();
  void refreshStatus();
  void updateSilenceColors();
  void restoreVisibleSelection();
  void click(const QModelIndex&, AlarmModel*);
  void select(Node*);
  void error(const QString&);
  void showMessage(const QString&);
  void showLoadWarnings();
  void open(bool insert = false);
  void save(bool as = false);
  void replace(Document, bool snapshot = true);
  void properties();
  void forceDialog();
  void guidance(Node* target = nullptr);
  void related(Node* target = nullptr);
  void masks(bool forced = false);
  void beepSeverity(bool global);
  void noAckDialog();
  QDialog* showText(const QString&, const QString&, bool monospace = false, bool searchable = true);
  void showLiveLog(bool alarm);
  void showHistory();
  void broadcast(int mode);
  void cut(bool remove);
  void paste();
  void clearNode();
  Document withoutSelection() const;
  void exitApplication();
  QModelIndex treeIndex(Node*) const;
  // Select a node as replace() and addNode() leave it: highlighted in the tree
  // or in its group's contents.
  void showSelection(Node*);
  void expandOneLevel(const QModelIndex&);
  void expandBranch(const QModelIndex&);
};
// ALH -B: route SIGINT to window->exitOnInterrupt() (Linux/macOS). Without
// -B, and for other signals, the default action applies, as in ALH.
void handleInterrupt(Window* window);
} // namespace alh
