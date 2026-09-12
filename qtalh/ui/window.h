#pragma once
#include "core/engine.h"
#include "appearance.h"
#include "services/channel_access.h"
#include "services/logging.h"
#include "services/options.h"
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

private:
  Document* doc;
  Engine* engine;
  bool tree;
  Node* group = nullptr;
  int filter = 0;
  mutable QHash<Node*, QVector<Node*>> childCache;
  QVector<Node*> children(Node*) const;
  bool visible(Node*) const;
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

protected:
  void closeEvent(QCloseEvent*) override;
  bool eventFilter(QObject*, QEvent*) override;

private:
  Document doc;
  Options options;
  bool connectPv, modified = false, quitting = false, dirty = true;
  std::unique_ptr<ChannelAccess> ca;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<Logging> logging;
  std::unique_ptr<NotificationService> notifications;
  std::unique_ptr<AnalyticsService> analytics;
  AlarmModel *treeModel = nullptr, *groupModel = nullptr;
  QTreeView *treeView = nullptr, *groupView = nullptr;
  QLabel *execution = nullptr, *filename = nullptr, *messageArea = nullptr, *beepLabel = nullptr,
         *silenceForeverLabel = nullptr, *disabledForceLabel = nullptr, *shelvedLabel = nullptr,
         *notificationsEnabledLabel = nullptr;
  QWidget* runtime = nullptr;
  QPushButton* runtimeButton = nullptr;
  QCheckBox *silenceBox = nullptr, *currentBox = nullptr;
  QTimer refreshTimer, beepTimer, heartbeatTimer;
  int silenceMinutes = 30;
  QMediaPlayer* sound = nullptr;
  Node *selection = nullptr, *selectedGroup = nullptr;
  QStringList undo, redo;
  std::shared_ptr<Node> clipboard;
  QAction *undoAction = nullptr, *redoAction = nullptr;
  QPointer<QDialog> historyDialog, shelfListDialog, notificationDialog, analyticsDialog;
  QPointer<QMessageBox> errorPopup, exitPopup;
  QPointer<QPlainTextEdit> historyText;
  struct SelectionDialog {
    QPointer<QDialog> window;
    Node* node = nullptr;
    QString state;
    std::function<void()> build;
  };
  QMap<QString, SelectionDialog> selectionDialogs;
  QString rebuildingDialog;
  bool dialogSyncPending = false;
  QDialog* beginSelectionDialog(const QString&, const QString&, std::function<void()>);
  void finishSelectionDialog(QDialog*);
  void clearDialogContent(QDialog*);
  void rebuildSelectionDialog(const QString&);
  void scheduleDialogSync();
  QString selectionState() const;
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
  void click(const QModelIndex&, AlarmModel*);
  void select(Node*);
  void error(const QString&);
  void open(bool insert = false);
  void save(bool as = false);
  void replace(Document, bool snapshot = true);
  void properties();
  void forceDialog();
  void guidance(Node* target = nullptr);
  void related(Node* target = nullptr);
  void masks(bool forced = false);
  void beepSeverity(bool global);
  void showText(const QString&, const QString&, bool fromFile = false);
  void showHistory();
  void broadcast(int mode);
  void cut(bool remove);
  void paste();
  void clearNode();
  void exitApplication();
  QModelIndex treeIndex(Node*) const;
  void expandOneLevel(const QModelIndex&);
  void expandBranch(const QModelIndex&);
};
} // namespace alh
