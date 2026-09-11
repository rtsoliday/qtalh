// Sustained, isolated engine/UI CPU benchmark. No network PVs or external commands.
#include "ui/window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <ctime>
using namespace alh;

static double cpuSeconds() {
  timespec value{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
  return value.tv_sec + value.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  initializeAppearance(qEnvironmentVariable("QTALH_TEST_STYLE"));
  const auto args = app.arguments();
  const int count = args.value(1, "10000").toInt();
  const QString mode = args.value(2, "idle");
  const int seconds = args.value(3, "10").toInt();
  if (count < 1 || seconds < 1 ||
      !QStringList{"idle", "updates", "filtered", "runtime"}.contains(mode))
    return 2;
  QString config = "GROUP NULL benchmark\n";
  for (int i = 0; i < count; ++i) {
    if (i % 100 == 0)
      config += QString("GROUP benchmark group%1\n").arg(i / 100);
    config += QString("CHANNEL group%1 bench:pv%2\n").arg(i / 100).arg(i);
  }
  Options options;
  options.noLog = options.noErrorPopup = options.silent = true;
  options.mainWindow = mode != "runtime";
  options.filter = mode == "filtered" ? 1 : 0;
  Window window(parseConfig(config), options, false);
  auto& engine = window.alarmEngine();
  const auto nodes = window.document().channels();
  for (auto node : nodes)
    engine.event(node, {0, 0, 0, 1, "0"});
  window.showInitial();
  QEventLoop warmup;
  QTimer::singleShot(1200, &warmup, &QEventLoop::quit);
  warmup.exec();
  quint64 events = 0;
  QVector<int> expected(count, 0);
  QTimer updates;
  updates.setTimerType(Qt::PreciseTimer);
  updates.setInterval(50);
  QObject::connect(&updates, &QTimer::timeout, [&] {
    for (int i = 0; i < 100; ++i) {
      const int index = events % count;
      const int severity = expected[index] ? 0 : 2;
      expected[index] = severity;
      engine.event(nodes[index], {severity ? 3 : 0, severity, severity, 1,
                                 QString::number(events)});
      ++events;
    }
  });
  if (mode == "updates" || mode == "filtered")
    updates.start();
  QEventLoop measurement;
  QTimer finish;
  finish.setSingleShot(true);
  finish.setTimerType(Qt::PreciseTimer);
  QObject::connect(&finish, &QTimer::timeout, &measurement, &QEventLoop::quit);
  QElapsedTimer wall;
  wall.start();
  const double startCpu = cpuSeconds();
  finish.start(seconds * 1000);
  measurement.exec();
  const double cpu = cpuSeconds() - startCpu;
  const double elapsed = wall.nsecsElapsed() / 1e9;
  updates.stop();
  std::array<int, 5> counts{};
  for (int i = 0; i < count; ++i) {
    if (engine.state(nodes[i]).severity != expected[i])
      return 3;
    ++counts[expected[i]];
  }
  if (engine.state(window.document().root.get()).counts != counts)
    return 4;
  std::printf("mode,channels,wall_seconds,cpu_seconds,cpu_percent,events\n"
              "%s,%d,%.6f,%.6f,%.4f,%llu\n", qPrintable(mode), count, elapsed, cpu,
              100 * cpu / elapsed, static_cast<unsigned long long>(events));
}
