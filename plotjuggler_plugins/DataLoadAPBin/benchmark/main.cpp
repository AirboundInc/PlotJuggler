/**
 * APBin loading benchmark.
 *
 * Usage:
 *   apbin_benchmark <plugin.dll> <logfile.BIN> [iterations=3]
 *
 * Loads the given DataLoadAPBin DLL via QPluginLoader, runs readDataFromFile
 * the requested number of times, and prints per-run and summary timings.
 * Run once with the original DLL and once with the parallel DLL to compare.
 *
 * Example:
 *   apbin_benchmark.exe original\DataLoadAPBin.dll flight.BIN 3
 *   apbin_benchmark.exe parallel\DataLoadAPBin.dll flight.BIN 3
 */

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPluginLoader>

#include <PlotJuggler/dataloader_base.h>
#include <PlotJuggler/plotdata.h>

#include <algorithm>
#include <numeric>

int main(int argc, char* argv[])
{
  QApplication app(argc, argv);

  if (argc < 3)
  {
    qInfo() << "Usage: apbin_benchmark <plugin.dll> <logfile.BIN> [iterations=3]";
    return 1;
  }

  const QString dll_path = argv[1];
  const QString bin_path = argv[2];
  const int iterations = (argc >= 4) ? QString(argv[3]).toInt() : 3;

  if (!QFileInfo::exists(dll_path))
  {
    qCritical() << "Plugin DLL not found:" << dll_path;
    return 1;
  }
  if (!QFileInfo::exists(bin_path))
  {
    qCritical() << "BIN file not found:" << bin_path;
    return 1;
  }

  QPluginLoader loader(dll_path);
  QObject* plugin_obj = loader.instance();
  if (!plugin_obj)
  {
    qCritical() << "Failed to load plugin:" << loader.errorString();
    return 1;
  }

  auto* data_loader = qobject_cast<PJ::DataLoader*>(plugin_obj);
  if (!data_loader)
  {
    qCritical() << "Object does not implement the DataLoader interface.";
    loader.unload();
    return 1;
  }

  const qint64 file_bytes = QFileInfo(bin_path).size();
  qInfo() << "Plugin :" << data_loader->name();
  qInfo() << "DLL    :" << QFileInfo(dll_path).absoluteFilePath();
  qInfo() << "File   :" << bin_path;
  qInfo() << QString("Size   : %1 MB").arg(file_bytes / 1024.0 / 1024.0, 0, 'f', 2);
  qInfo() << "Runs   :" << iterations;
  qInfo() << "---------------------------------------";

  QVector<qint64> times;
  times.reserve(iterations);

  for (int i = 0; i < iterations; i++)
  {
    PJ::PlotDataMapRef plot_data;
    PJ::FileLoadInfo info;
    info.filename = bin_path;

    QElapsedTimer timer;
    timer.start();

    const bool ok = data_loader->readDataFromFile(&info, plot_data);

    const qint64 elapsed_ms = timer.elapsed();
    times.push_back(elapsed_ms);

    // Count total series loaded as a sanity check.
    const size_t total_series = plot_data.numeric.size();

    qInfo() << QString("  Run %1: %2 ms  |  %3 series  |  %4")
                   .arg(i + 1)
                   .arg(elapsed_ms, 6)
                   .arg(total_series, 5)
                   .arg(ok ? "OK" : "FAILED");

    // Let Qt process any pending events between runs.
    QApplication::processEvents();
  }

  if (iterations > 1)
  {
    const qint64 sum = std::accumulate(times.begin(), times.end(), 0LL);
    const qint64 min_t = *std::min_element(times.begin(), times.end());
    const qint64 max_t = *std::max_element(times.begin(), times.end());
    const double avg = static_cast<double>(sum) / iterations;
    const double mb_per_s = (file_bytes / 1024.0 / 1024.0) / (avg / 1000.0);

    qInfo() << "---------------------------------------";
    qInfo() << QString("  Average : %1 ms").arg(avg, 0, 'f', 1);
    qInfo() << QString("  Min     : %1 ms").arg(min_t);
    qInfo() << QString("  Max     : %1 ms").arg(max_t);
    qInfo() << QString("  Throughput: %1 MB/s").arg(mb_per_s, 0, 'f', 1);
  }

  loader.unload();
  return 0;
}
