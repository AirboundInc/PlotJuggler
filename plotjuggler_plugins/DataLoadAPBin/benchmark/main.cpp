/**
 * APBin loading benchmark.
 *
 * Usage:
 *   apbin_benchmark <plugin.dll> <logfile.BIN> [iterations=3]
 *
 * Results are written to apbin_bench_results.txt in the current directory.
 */

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPluginLoader>

#include <PlotJuggler/dataloader_base.h>
#include <PlotJuggler/plotdata.h>

#include <algorithm>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>

// Write to both the log file and stdout.  Using std::ofstream bypasses any
// CRT/console attachment issues introduced by QApplication on Windows.
static std::ofstream g_log;
static void LOG(const std::string& s)
{
  if (g_log.is_open())
  {
    g_log << s;
    g_log.flush();
  }
  // Also try stdout in case it works
  fputs(s.c_str(), stdout);
  fflush(stdout);
}

int main(int argc, char* argv[])
{
  // Open log file immediately — before QApplication — so diagnostics are
  // captured even if Qt crashes during initialisation.
  g_log.open("apbin_bench_results.txt", std::ios::out | std::ios::trunc);

  LOG("apbin_benchmark started\n");

  // Use the offscreen platform to avoid loading qwindows.dll, which can
  // trigger a CFG / security-check failure when launched non-interactively.
  qputenv("QT_QPA_PLATFORM", "offscreen");

  QApplication app(argc, argv);

  LOG("QApplication ready\n");

  if (argc < 3)
  {
    LOG("Usage: apbin_benchmark <plugin.dll> <logfile.BIN> [iterations=3]\n");
    return 1;
  }

  const QString dll_path = argv[1];
  const QString bin_path = argv[2];
  const int iterations = (argc >= 4) ? QString(argv[3]).toInt() : 3;

  if (!QFileInfo::exists(dll_path))
  {
    LOG(std::string("Plugin DLL not found: ") + argv[1] + "\n");
    return 1;
  }
  if (!QFileInfo::exists(bin_path))
  {
    LOG(std::string("BIN file not found: ") + argv[2] + "\n");
    return 1;
  }

  LOG("Loading plugin...\n");

  QPluginLoader loader(dll_path);
  QObject* plugin_obj = loader.instance();
  if (!plugin_obj)
  {
    LOG(std::string("Failed to load plugin: ") +
        loader.errorString().toLocal8Bit().constData() + "\n");
    return 1;
  }

  auto* data_loader = qobject_cast<PJ::DataLoader*>(plugin_obj);
  if (!data_loader)
  {
    LOG("Object does not implement DataLoader.\n");
    loader.unload();
    return 1;
  }

  LOG("Plugin loaded OK\n");

  const qint64 file_bytes = QFileInfo(bin_path).size();

  auto fmt = [](const std::string& label, const std::string& val) {
    return label + " : " + val + "\n";
  };
  LOG(fmt("Plugin", std::string(data_loader->name())));
  LOG(fmt("DLL",
          QFileInfo(dll_path).absoluteFilePath().toLocal8Bit().constData()));
  LOG(fmt("File", argv[2]));
  {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed << (file_bytes / 1024.0 / 1024.0) << " MB";
    LOG(fmt("Size", ss.str()));
  }
  {
    std::ostringstream ss;
    ss << iterations;
    LOG(fmt("Runs", ss.str()));
  }
  LOG("---------------------------------------\n");

  std::vector<long long> times;
  times.reserve(iterations);

  for (int i = 0; i < iterations; i++)
  {
    PJ::PlotDataMapRef plot_data;
    PJ::FileLoadInfo info;
    info.filename = bin_path;

    QElapsedTimer timer;
    timer.start();

    const bool ok = data_loader->readDataFromFile(&info, plot_data);

    const long long elapsed_ms = timer.elapsed();
    times.push_back(elapsed_ms);

    const size_t total_series = plot_data.numeric.size();

    std::ostringstream ss;
    ss << "  Run " << (i + 1) << ": " << elapsed_ms << " ms"
       << "  |  " << total_series << " series"
       << "  |  " << (ok ? "OK" : "FAILED") << "\n";
    LOG(ss.str());

    QApplication::processEvents();
  }

  if (iterations > 1)
  {
    const long long sum =
        std::accumulate(times.begin(), times.end(), 0LL);
    const long long min_t = *std::min_element(times.begin(), times.end());
    const long long max_t = *std::max_element(times.begin(), times.end());
    const double avg = static_cast<double>(sum) / iterations;
    const double mb_per_s = (file_bytes / 1024.0 / 1024.0) / (avg / 1000.0);

    LOG("---------------------------------------\n");
    {
      std::ostringstream ss;
      ss << std::fixed;
      ss.precision(1);
      ss << "  Average   : " << avg << " ms\n";
      ss << "  Min       : " << min_t << " ms\n";
      ss << "  Max       : " << max_t << " ms\n";
      ss.precision(1);
      ss << "  Throughput: " << mb_per_s << " MB/s\n";
      LOG(ss.str());
    }
  }

  LOG("Done.\n");
  g_log.close();
  loader.unload();
  return 0;
}
