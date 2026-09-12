#include "dialogs.h"
#include "window.h"
#include <QtWidgets>
#include <algorithm>
namespace alh {
namespace {
QString duration(qint64 ms) {
  return ms < 0 ? QString("—") : QString::number(ms / 1000.0, 'f', 1) + " s";
}
QString utc(qint64 time) {
  return time ? QDateTime::fromMSecsSinceEpoch(time, Qt::UTC).toString(Qt::ISODate) : QString("—");
}
QString quality(const AnalyticsChannel& c) {
  return c.ambiguous      ? "Ambiguous identity"
         : c.removed      ? "Removed"
         : !c.initialized ? "Awaiting observation"
         : !c.available   ? "Coverage gap"
                          : "Observed";
}
class AnalyticsModel : public QAbstractTableModel {
public:
  AnalyticsReport report;
  QVector<int> rows;
  int tab;
  QStringList headers;
  AnalyticsModel(int t, QStringList h, QObject* parent)
      : QAbstractTableModel(parent), tab(t), headers(h) {}
  int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : rows.size(); }
  int columnCount(const QModelIndex& p = {}) const override {
    return p.isValid() ? 0 : headers.size();
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
      return headers.value(section);
    return {};
  }
  void replace(const AnalyticsReport& next) {
    beginResetModel();
    report = next;
    rows.clear();
    for (int i = 0; i < report.rows.size(); ++i)
      if (tab != 2 || report.rows[i].standingMs >= 0)
        rows << i;
    endResetModel();
  }
  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid() || index.row() >= rows.size())
      return {};
    const auto& r = report.rows[rows[index.row()]];
    const auto& c = r.channel;
    const auto& s = r.stats;
    if (role == Qt::UserRole)
      return c.key;
    if (role == Qt::ToolTipRole)
      return c.path + "\n" + quality(c);
    if (role != Qt::DisplayRole && role != Qt::UserRole + 1)
      return {};
    int col = index.column();
    if (!col)
      return c.path;
    auto number = [&](QString text, double value) -> QVariant {
      return role == Qt::DisplayRole ? QVariant(text) : QVariant(value);
    };
    auto seconds = [&](qint64 value, bool lower = false) -> QVariant {
      return number((lower && value >= 0 ? "At least " : "") + duration(value), value);
    };
    if (tab == 0) {
      switch (col) {
      case 1:
        return number(QString::number(s.activations), s.activations);
      case 2: {
        double share =
            report.totalActivations ? 100.0 * s.activations / report.totalActivations : 0;
        return number(QString::number(share, 'f', 1), share);
      }
      case 3:
        return number(severityName(s.peak), s.peak);
      case 4:
        return utc(s.lastActivationUtc);
      case 5:
        return number(QString::number(s.gaps), s.gaps);
      default:
        return quality(c);
      }
    }
    if (tab == 1) {
      switch (col) {
      case 1:
        return number(r.chattering ? "Yes" : "No", r.chattering);
      case 2:
        return number(QString::number(r.currentActivations), r.currentActivations);
      case 3:
        return number(QString::number(r.chatterMaximum), r.chatterMaximum);
      case 4:
        return number(QString::number(s.activations), s.activations);
      case 5:
        return analyticsSuppression(c.flags);
      default:
        return quality(c);
      }
    }
    if (tab == 2) {
      switch (col) {
      case 1:
        return seconds(r.standingMs, c.standingUnknown);
      case 2:
        return seconds(s.activeMs);
      case 3:
        return seconds(s.observedMs);
      case 4:
        return number(severityName(c.severity), c.severity);
      case 5:
        return analyticsSuppression(c.flags);
      default:
        return utc(c.lastUtc);
      }
    }
    switch (col) {
    case 1:
      return number(QString::number(s.ackCount), s.ackCount);
    case 2:
      return seconds(s.ackCount ? s.ackSumMs / s.ackCount : -1);
    case 3:
      return seconds(s.ackCount ? s.ackMaxMs : -1);
    case 4:
      return seconds(r.outstandingMs, c.ackUnknown);
    case 5:
      return number(QString::number(s.incomplete), s.incomplete);
    case 6:
      return analyticsSuppression(c.flags);
    default:
      return quality(c);
    }
  }
};
class AnalyticsPlot : public QWidget {
public:
  AnalyticsReport report;
  int tab;
  QString selected;
  explicit AnalyticsPlot(int t, QWidget* parent = nullptr) : QWidget(parent), tab(t) {
    setMinimumHeight(200);
    setAccessibleName(t == 0   ? "Activation trend and top offenders"
                      : t == 1 ? "Selected channel activation timeline"
                      : t == 2 ? "Longest standing alarms"
                               : "Acknowledgement response distribution");
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    p.setPen(palette().text().color());
    if (report.rows.isEmpty()) {
      p.drawText(rect(), Qt::AlignCenter, "No observations in this scope");
      return;
    }
    auto sorted = report.rows;
    std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
      return tab == 2 ? a.standingMs > b.standingMs : a.stats.activations > b.stats.activations;
    });
    auto bars = [&](QRect area, const QVector<QPair<QString, double>>& values,
                    const QString& title) {
      p.setPen(palette().text().color());
      p.drawText(area.adjusted(4, 0, -4, 0), Qt::AlignTop | Qt::AlignLeft, title);
      area.adjust(4, 24, -8, -10);
      double max = 1;
      for (const auto& v : values)
        max = qMax(max, v.second);
      int step = qMax(12, area.height() / qMax(1, int(values.size())));
      for (int i = 0; i < values.size(); ++i) {
        auto v = values[i];
        int label = qMin(150, area.width() / 2);
        QRect row(area.left(), area.top() + i * step, area.width(), step);
        p.setPen(palette().text().color());
        p.drawText(row.adjusted(0, 0, -row.width() + label - 4, 0),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(v.first, Qt::ElideMiddle, label - 4));
        int width = int(qMax(0.0, v.second) / max * qMax(1, row.width() - label - 60));
        p.fillRect(QRect(row.left() + label, row.top() + 3, width, qMax(2, step - 6)),
                   palette().highlight());
        p.drawText(QRect(row.left() + label + width + 4, row.top(), 60, step), Qt::AlignVCenter,
                   QString::number(v.second, 'g', 4));
      }
    };
    if (tab == 3) {
      QVector<QPair<QString, double>> values;
      auto labels = analyticsHistogramLabels();
      for (int i = 0; i < labels.size(); ++i)
        values << qMakePair(labels[i], double(report.histogram[i]));
      bars(rect(), values, "Confirmed acknowledgement samples by response time");
      return;
    }
    if (tab == 2) {
      QVector<QPair<QString, double>> values;
      for (const auto& r : sorted) {
        if (r.standingMs >= 0)
          values << qMakePair(r.channel.path, r.standingMs / 1000.0);
        if (values.size() == 10)
          break;
      }
      bars(rect(), values, "Longest currently observed standing alarms (seconds)");
      return;
    }
    if (tab == 0) {
      QVector<QPair<QString, double>> values;
      for (const auto& r : sorted) {
        if (r.stats.activations)
          values << qMakePair(r.channel.path, double(r.stats.activations));
        if (values.size() == 10)
          break;
      }
      bars(QRect(0, 0, width() / 2, height()), values, "Top ten offenders (activations)");
    }
    QRect area = tab == 0 ? QRect(width() / 2 + 10, 26, width() / 2 - 30, height() - 55)
                          : rect().adjusted(20, 30, -20, -35);
    p.setPen(palette().text().color());
    p.drawText(area.left(), 18,
               tab == 0 ? "Activations over retained coverage"
                        : "Selected channel activations over retained coverage");
    p.drawRect(area);
    qint64 span = qMax<qint64>(1, report.to - report.coverage);
    if (tab == 0) {
      qint64 maximum = 1;
      for (auto n : report.trend)
        maximum = qMax(maximum, n);
      for (int i = 0; i < report.trend.size(); ++i) {
        int x = area.left() + i * area.width() / report.trend.size();
        int h = int(double(report.trend[i]) / maximum * area.height());
        p.fillRect(x, area.bottom() - h, qMax(1, area.width() / int(report.trend.size())), h,
                   palette().highlight());
      }
    } else {
      const AnalyticsRow* row = nullptr;
      for (const auto& r : report.rows)
        if (r.channel.key == selected)
          row = &r;
      if (!row && !sorted.isEmpty())
        row = &sorted.first();
      if (row) {
        p.setPen(palette().highlight().color());
        for (auto time : row->activationTimes) {
          int x = area.left() + int(double(time - report.coverage) / span * area.width());
          p.drawLine(x, area.top() + 12, x, area.bottom());
        }
      }
    }
    p.setPen(palette().text().color());
    p.drawText(area.left(), height() - 8, utc(report.coverageUtc));
    auto end = utc(report.toUtc);
    p.drawText(area.right() - p.fontMetrics().horizontalAdvance(end), height() - 8, end);
  }
};
bool sameQuery(const AnalyticsQuery& a, const AnalyticsQuery& b) {
  return a.rangeMs == b.rangeMs && a.scope == b.scope && a.search == b.search &&
         a.suppression == b.suppression && a.chatterCount == b.chatterCount &&
         a.chatterSeconds == b.chatterSeconds;
}
} // namespace
void Window::showAnalytics() {
  if (!analytics)
    return;
  if (analyticsDialog) {
    analyticsDialog->show();
    analyticsDialog->raise();
    return;
  }
  auto dialog = new QDialog(this);
  analyticsDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Alarm Analytics — current runtime session");
  dialog->setObjectName("analyticsDialog");
  auto layout = new QVBoxLayout(dialog);
  auto controls = new QGridLayout;
  layout->addLayout(controls);
  auto range = new QComboBox;
  range->setObjectName("analyticsRange");
  range->addItem("Last 15 minutes", qint64(900000));
  range->addItem("Last hour", qint64(3600000));
  range->addItem("Last 8 hours", qint64(28800000));
  range->addItem("Last 24 hours", qint64(86400000));
  range->addItem("Since Session Start", qint64(0));
  range->setCurrentIndex(1);
  auto scope = new QComboBox;
  scope->setObjectName("analyticsScope");
  scope->addItem("Whole configuration", QString());
  for (auto n : doc.nodes())
    scope->addItem(Engine::channelPath(n), Engine::nodeIdentity(n));
  auto search = new QLineEdit;
  search->setObjectName("analyticsSearch");
  search->setPlaceholderText("Find channel or path");
  auto suppression = new QComboBox;
  suppression->setObjectName("analyticsSuppression");
  suppression->addItems({"All observed activity", "Unsuppressed activity", "Suppressed activity"});
  controls->addWidget(new QLabel("Range"), 0, 0);
  controls->addWidget(range, 0, 1);
  controls->addWidget(new QLabel("Scope"), 0, 2);
  controls->addWidget(scope, 0, 3);
  controls->addWidget(search, 1, 0, 1, 2);
  controls->addWidget(suppression, 1, 2, 1, 2);
  auto chatterCount = new QSpinBox;
  chatterCount->setRange(2, 10000);
  chatterCount->setValue(5);
  chatterCount->setObjectName("analyticsChatterCount");
  auto chatterWindow = new QSpinBox;
  chatterWindow->setRange(1, 86400);
  chatterWindow->setValue(60);
  chatterWindow->setSuffix(" s");
  chatterWindow->setObjectName("analyticsChatterWindow");
  controls->addWidget(new QLabel("Chatter: activations"), 2, 0);
  controls->addWidget(chatterCount, 2, 1);
  controls->addWidget(new QLabel("within"), 2, 2);
  controls->addWidget(chatterWindow, 2, 3);
  auto coverage = new QLabel("Collecting processed alarms; waiting for report…");
  coverage->setObjectName("analyticsCoverage");
  coverage->setWordWrap(true);
  layout->addWidget(coverage);
  auto tabs = new QTabWidget;
  tabs->setObjectName("analyticsTabs");
  layout->addWidget(tabs, 1);
  QVector<QTableView*> tables;
  QVector<AnalyticsModel*> models;
  QVector<AnalyticsPlot*> plots;
  QStringList titles = {"Frequent offenders", "Chattering alarms", "Standing alarms",
                        "Acknowledgement times"};
  QVector<QStringList> headers = {{"Channel path", "Activations", "Share (%)", "Peak severity",
                                   "Last activation (UTC)", "Coverage gaps", "State"},
                                  {"Channel path", "Chattering now", "Activations in window",
                                   "Maximum in window", "Activations in range", "Suppression",
                                   "State"},
                                  {"Channel path", "Standing time", "Active time", "Observed time",
                                   "Severity", "Suppression", "Last observation (UTC)"},
                                  {"Channel path", "Samples", "Mean response", "Max response",
                                   "Outstanding age", "Incomplete", "Suppression", "State"}};
  for (int t = 0; t < 4; ++t) {
    auto page = new QWidget;
    auto l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    auto split = new QSplitter(Qt::Vertical);
    auto table = new QTableView;
    auto model = new AnalyticsModel(t, headers[t], table);
    auto proxy = new QSortFilterProxyModel(table);
    proxy->setSourceModel(model);
    proxy->setSortRole(Qt::UserRole + 1);
    table->setModel(proxy);
    models << model;
    table->setObjectName("analyticsTable" + QString::number(t));

    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSortingEnabled(true);
    table->sortByColumn(t == 1 ? 3 : 1, Qt::DescendingOrder);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setColumnWidth(0, 280);
    for (int col = 1; col < headers[t].size(); ++col) {
      int width = table->fontMetrics().horizontalAdvance(headers[t][col]) + 28;
      if (headers[t][col].contains("UTC"))
        width = qMax(width, table->fontMetrics().horizontalAdvance("2023-11-14T22:13:20Z") + 16);
      table->setColumnWidth(col, qMax(75, width));
    }
    split->addWidget(table);
    auto plot = new AnalyticsPlot(t);
    plot->setObjectName("analyticsPlot" + QString::number(t));
    split->addWidget(plot);
    split->setSizes({350, 240});
    l->addWidget(split);
    tabs->addTab(page, titles[t]);
    tables << table;
    plots << plot;
  }
  auto actions = new QHBoxLayout;
  layout->addLayout(actions);
  auto details = new QPushButton("Channel details");
  details->setObjectName("analyticsDetails");
  auto exportTable = new QPushButton("Export table CSV…");
  exportTable->setObjectName("analyticsExportTable");
  auto exportChart = new QPushButton("Export chart CSV…");
  exportChart->setObjectName("analyticsExportChart");
  auto reset = new QPushButton("Reset Analytics");
  reset->setObjectName("analyticsReset");
  auto close = new QPushButton("Close");
  for (auto b : {details, exportTable, exportChart, reset, close})
    actions->addWidget(b);
  auto report = std::make_shared<AnalyticsReport>();
  auto query = [=] {
    AnalyticsQuery q;
    q.rangeMs = range->currentData().toLongLong();
    q.scope = scope->currentData().toString();
    q.search = search->text();
    q.suppression = suppression->currentIndex();
    q.chatterCount = chatterCount->value();
    q.chatterSeconds = chatterWindow->value();
    return q;
  };
  QPointer<QDialog> guard = dialog;
  analytics->reportReady = [=](AnalyticsReport result) {
    if (!guard || !sameQuery(result.query, query()))
      return;
    *report = std::move(result);
    QString text = QString("Session %1 | UTC %2 to %3 | %4 activations | %5 gaps | %6 incomplete "
                           "acknowledgement samples")
                       .arg(report->session.left(8), utc(report->fromUtc), utc(report->toUtc))
                       .arg(report->totalActivations)
                       .arg(report->gaps)
                       .arg(report->incomplete);
    if (report->partial)
      text += "\nPARTIAL DETAIL: rolling history, timelines and chatter available from " +
              utc(report->coverageUtc) + ". Session totals are retained.";
    else
      text += "\nOnly observed intervals are measured; connection/access failures and Cancel "
              "create gaps.";
    coverage->setText(text);
    for (int t = 0; t < 4; ++t) {
      auto table = tables[t];
      auto selectedId = table->currentIndex().data(Qt::UserRole).toString();
      models[t]->replace(*report);
      for (int i = 0; i < table->model()->rowCount(); ++i)
        if (table->model()->index(i, 0).data(Qt::UserRole).toString() == selectedId) {
          table->setCurrentIndex(table->model()->index(i, 0));
          break;
        }
      if (!table->currentIndex().isValid() && table->model()->rowCount())
        table->setCurrentIndex(table->model()->index(0, 0));
      plots[t]->report = *report;
      plots[t]->update();
    }
  };
  auto showDetails = [=] {
    auto table = tables[tabs->currentIndex()];
    if (!table->currentIndex().isValid())
      return;
    auto id = table->currentIndex().data(Qt::UserRole).toString();
    for (const auto& r : report->rows)
      if (r.channel.key == id) {
        auto d = new QDialog(dialog);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle("Analytics channel details");
        d->setObjectName("analyticsChannelDetails");
        auto l = new QVBoxLayout(d);
        auto text = new QPlainTextEdit;
        text->setReadOnly(true);
        text->setPlainText(
            QString("%1\nPV: %2\nState: %3\nSuppression: %4\nActivations: %5\nActive time: "
                    "%6\nObserved coverage: %7\nStanding: %8%9\nAcknowledgements: %10 completed; "
                    "%11 incomplete\nMean response: %12\nMaximum response: %13\nOutstanding: "
                    "%14%15\nCoverage gaps: %16\nLast observation: %17\n\nAutomatic clears and "
                    "unknown/interrupted response times are excluded.\nCharts and chatter use "
                    "retained detail beginning %18.")
                .arg(r.channel.path, r.channel.pv, quality(r.channel),
                     analyticsSuppression(r.channel.flags))
                .arg(r.stats.activations)
                .arg(duration(r.stats.activeMs), duration(r.stats.observedMs),
                     r.channel.standingUnknown ? "at least " : "", duration(r.standingMs))
                .arg(r.stats.ackCount)
                .arg(r.stats.incomplete)
                .arg(r.stats.ackCount ? duration(r.stats.ackSumMs / r.stats.ackCount) : "—",
                     r.stats.ackCount ? duration(r.stats.ackMaxMs) : "—",
                     r.channel.ackUnknown ? "at least " : "", duration(r.outstandingMs))
                .arg(r.stats.gaps)
                .arg(utc(r.channel.lastUtc), utc(report->coverageUtc)));
        l->addWidget(text);
        d->resize(650, 450);
        d->show();
        break;
      }
  };
  connect(details, &QPushButton::clicked, dialog, showDetails);
  for (int t = 0; t < 4; ++t) {
    connect(tables[t], &QTableView::doubleClicked, dialog,
            [=](const QModelIndex&) { showDetails(); });
    connect(tables[t]->selectionModel(), &QItemSelectionModel::currentChanged, dialog,
            [=](const QModelIndex& index, const QModelIndex&) {
              plots[t]->selected = index.data(Qt::UserRole).toString();
              plots[t]->update();
            });
  }

  auto exportCsv = [=](bool chart) {
    if (report->session.isEmpty())
      return;
    auto path = chooseFile(dialog, "Export analytics CSV", {}, "CSV files (*.csv)", true);
    if (path.isEmpty())
      return;
    auto copy = *report;
    int tab = tabs->currentIndex();
    if (chart && tab == 1 && !plots[1]->selected.isEmpty()) {
      copy.rows.erase(
          std::remove_if(copy.rows.begin(), copy.rows.end(),
                         [&](const auto& r) { return r.channel.key != plots[1]->selected; }),
          copy.rows.end());
    } else if (!chart) {
      copy.rows.clear();
      auto table = tables[tab];
      QHash<QString, int> lookup;
      for (int i = 0; i < report->rows.size(); ++i)
        lookup[report->rows[i].channel.key] = i;
      for (int i = 0; i < table->model()->rowCount(); ++i) {
        auto id = table->model()->index(i, 0).data(Qt::UserRole).toString();
        if (lookup.contains(id))
          copy.rows << report->rows[lookup[id]];
      }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
      QMessageBox::warning(dialog, "Export analytics", "Cannot open CSV output.");
      return;
    }
    auto bytes = analyticsCsv(copy, tab, chart);
    if (file.write(bytes) != bytes.size() || !file.commit())
      QMessageBox::warning(dialog, "Export analytics", "Cannot save CSV output.");
  };
  connect(exportTable, &QPushButton::clicked, dialog, [=] { exportCsv(false); });
  connect(exportChart, &QPushButton::clicked, dialog, [=] { exportCsv(true); });
  connect(reset, &QPushButton::clicked, dialog, [=] {
    analytics->reset();
    coverage->setText("Analytics reset; awaiting updated report.");
    for (auto model : models)
      model->replace(AnalyticsReport{});
    for (auto plot : plots) {
      plot->report = {};
      plot->update();
    }
    *report = {};
  });
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  auto timer = new QTimer(dialog);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, dialog, [=] { analytics->request(query()); });
  timer->start();
  analytics->request(query());
  dialog->resize(1120, 800);
  dialog->show();
}
} // namespace alh
