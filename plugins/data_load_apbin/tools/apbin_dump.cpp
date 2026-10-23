// apbin_dump: load an ArduPilot .bin log through the APBin data source plugin, headlessly,
// and print a canonical summary of everything the plugin wrote. Used to check the PJ4 port
// against PJ3 (same summary format as the PJ3 harness) and to time loads.
//
//   apbin_dump <apbin_source_plugin.dll> <log.bin>
//
// The plugin is driven through the same C ABI the app uses (PJ_get_data_source_vtable), with a
// minimal in-memory host that records topics, fields and rows.
//
// Output, one line per series, sorted by name:
//   SERIES <topic>/<field> count=<n> t0=<ns> t1=<ns> hash=<fnv1a of (ns, value bits)>
// Points are stable-sorted by time before hashing (PJ3's PlotData keeps series time-sorted).

#include <pj_base/data_source_protocol.h>
#include <pj_base/plugin_data_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

std::string_view sv(PJ_string_view_t s) {
  return {s.data, static_cast<size_t>(s.size)};
}

void setError(PJ_error_t* err, const std::string& message) {
  if (err != nullptr) {
    std::snprintf(err->message, sizeof(err->message), "%s", message.c_str());
  }
}

struct Field {
  std::string name;
  PJ_primitive_type_t type;
  std::vector<std::pair<int64_t, double>> points;
  std::vector<std::pair<int64_t, std::string>> strings;
};

struct Topic {
  std::string name;
  std::vector<Field> fields;
  std::map<std::string, uint32_t, std::less<>> field_ids;
};

double toDouble(const PJ_scalar_value_t& v) {
  switch (v.type) {
    case PJ_PRIMITIVE_TYPE_FLOAT32: return v.data.as_float32;
    case PJ_PRIMITIVE_TYPE_FLOAT64: return v.data.as_float64;
    case PJ_PRIMITIVE_TYPE_INT8: return v.data.as_int8;
    case PJ_PRIMITIVE_TYPE_INT16: return v.data.as_int16;
    case PJ_PRIMITIVE_TYPE_INT32: return v.data.as_int32;
    case PJ_PRIMITIVE_TYPE_INT64: return static_cast<double>(v.data.as_int64);
    case PJ_PRIMITIVE_TYPE_UINT8: return v.data.as_uint8;
    case PJ_PRIMITIVE_TYPE_UINT16: return v.data.as_uint16;
    case PJ_PRIMITIVE_TYPE_UINT32: return v.data.as_uint32;
    case PJ_PRIMITIVE_TYPE_UINT64: return static_cast<double>(v.data.as_uint64);
    case PJ_PRIMITIVE_TYPE_BOOL: return v.data.as_bool;
    default: return 0.0;
  }
}

class FakeHost {
 public:
  FakeHost() {
    write_vt_.abi_version = PJ_ABI_VERSION;
    write_vt_.struct_size = sizeof(write_vt_);
    write_vt_.ensure_topic = &FakeHost::ensureTopic;
    write_vt_.ensure_field = &FakeHost::ensureField;
    write_vt_.append_record = &FakeHost::appendRecord;
    write_vt_.append_bound_record = &FakeHost::appendBoundRecord;

    runtime_vt_.protocol_version = 1;
    runtime_vt_.struct_size = sizeof(runtime_vt_);
    runtime_vt_.report_message = &FakeHost::reportMessage;
    runtime_vt_.progress_start = &FakeHost::progressStart;
    runtime_vt_.progress_update = &FakeHost::progressUpdate;
    runtime_vt_.progress_finish = &FakeHost::progressFinish;
    runtime_vt_.is_stop_requested = &FakeHost::isStopRequested;
    runtime_vt_.notify_state = &FakeHost::notifyState;
    runtime_vt_.request_stop = &FakeHost::requestStop;
    runtime_vt_.set_dataset_metadata = &FakeHost::setDatasetMetadata;

    registry_vt_.protocol_version = 1;
    registry_vt_.struct_size = sizeof(registry_vt_);
    registry_vt_.get_service = &FakeHost::getService;
  }

  PJ_service_registry_t registry() {
    return {this, &registry_vt_};
  }

  std::vector<Topic> topics;
  std::map<std::string, uint32_t, std::less<>> topic_ids;
  std::vector<std::string> messages;
  std::string metadata;

 private:
  static FakeHost* self(void* ctx) {
    return static_cast<FakeHost*>(ctx);
  }

  static bool getService(void* ctx, PJ_string_view_t name, uint32_t, PJ_service_t* out, PJ_error_t* err) noexcept {
    if (sv(name) == "pj.source_write.v1") {
      *out = {ctx, &self(ctx)->write_vt_};
      return true;
    }
    if (sv(name) == "pj.runtime.v1") {
      *out = {ctx, &self(ctx)->runtime_vt_};
      return true;
    }
    setError(err, "service not provided by apbin_dump: " + std::string(sv(name)));
    return false;
  }

  static bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    auto* h = self(ctx);
    auto it = h->topic_ids.find(sv(name));
    if (it == h->topic_ids.end()) {
      it = h->topic_ids.emplace(std::string(sv(name)), static_cast<uint32_t>(h->topics.size())).first;
      h->topics.push_back({std::string(sv(name)), {}, {}});
    }
    out->id = it->second;
    return true;
  }

  static uint32_t fieldId(Topic& topic, std::string_view name, PJ_primitive_type_t type) {
    auto it = topic.field_ids.find(name);
    if (it == topic.field_ids.end()) {
      it = topic.field_ids.emplace(std::string(name), static_cast<uint32_t>(topic.fields.size())).first;
      topic.fields.push_back({std::string(name), type, {}, {}});
    }
    return it->second;
  }

  static void store(Field& field, int64_t ts, bool is_null, const PJ_scalar_value_t& value) {
    if (is_null) {
      return;
    }
    if (value.type == PJ_PRIMITIVE_TYPE_STRING) {
      field.strings.emplace_back(ts, std::string(sv(value.data.as_string)));
    } else {
      field.points.emplace_back(ts, toDouble(value));
    }
  }

  static bool ensureField(
      void* ctx, PJ_topic_handle_t topic, PJ_string_view_t name, PJ_primitive_type_t type, PJ_field_handle_t* out,
      PJ_error_t* err) noexcept {
    auto* h = self(ctx);
    if (topic.id >= h->topics.size()) {
      setError(err, "bad topic handle");
      return false;
    }
    out->topic = topic;
    out->id = fieldId(h->topics[topic.id], sv(name), type);
    return true;
  }

  static bool appendRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t ts, const PJ_named_field_value_t* fields, uint64_t count,
      PJ_error_t* err) noexcept {
    auto* h = self(ctx);
    if (topic.id >= h->topics.size()) {
      setError(err, "bad topic handle");
      return false;
    }
    Topic& t = h->topics[topic.id];
    for (uint64_t i = 0; i < count; ++i) {
      const uint32_t id = fieldId(t, sv(fields[i].name), fields[i].value.type);
      store(t.fields[id], ts, fields[i].is_null, fields[i].value);
    }
    return true;
  }

  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t ts, const PJ_bound_field_value_t* fields, uint64_t count,
      PJ_error_t* err) noexcept {
    auto* h = self(ctx);
    if (topic.id >= h->topics.size()) {
      setError(err, "bad topic handle");
      return false;
    }
    Topic& t = h->topics[topic.id];
    for (uint64_t i = 0; i < count; ++i) {
      if (fields[i].field.id >= t.fields.size()) {
        setError(err, "bad field handle");
        return false;
      }
      store(t.fields[fields[i].field.id], ts, fields[i].is_null, fields[i].value);
    }
    return true;
  }

  static void reportMessage(void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
    self(ctx)->messages.push_back("[" + std::to_string(static_cast<int>(level)) + "] " + std::string(sv(message)));
  }
  static bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept {
    return true;
  }
  static bool progressUpdate(void*, uint64_t) noexcept {
    return true;
  }
  static void progressFinish(void*) noexcept {}
  static bool isStopRequested(void*) noexcept {
    return false;
  }
  static void notifyState(void*, PJ_data_source_state_t) noexcept {}
  static void requestStop(void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {}
  static bool setDatasetMetadata(void* ctx, PJ_string_view_t json, PJ_error_t*) noexcept {
    self(ctx)->metadata = std::string(sv(json));
    return true;
  }

  PJ_source_write_host_vtable_t write_vt_{};
  PJ_data_source_runtime_host_vtable_t runtime_vt_{};
  PJ_service_registry_vtable_t registry_vt_{};
};

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Non-finite values are counted but left out of count/t0/t1/hash: PJ3's PlotData silently drops
// them on insert, while PJ4's datastore keeps them.
void printSeries(const std::string& name, std::vector<std::pair<int64_t, double>> points) {
  const auto non_finite = std::erase_if(points, [](const auto& p) { return !std::isfinite(p.second); });
  if (non_finite > 0) {
    std::printf("NONFINITE %s %zu\n", name.c_str(), static_cast<size_t>(non_finite));
  }
  std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  uint64_t hash = 14695981039346656037ULL;
  for (const auto& [ts, value] : points) {
    hash = fnv1a(hash, &ts, sizeof(ts));
    hash = fnv1a(hash, &value, sizeof(value));
  }
  const long long t0 = points.empty() ? 0 : points.front().first;
  const long long t1 = points.empty() ? 0 : points.back().first;
  std::printf(
      "SERIES %s count=%zu t0=%lld t1=%lld hash=%016llx\n", name.c_str(), points.size(), t0, t1,
      static_cast<unsigned long long>(hash));
}

using GetVtableFn = const PJ_data_source_vtable_t* (*)();

GetVtableFn loadPlugin(const char* path) {
#ifdef _WIN32
  HMODULE lib = LoadLibraryA(path);
  if (lib == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<GetVtableFn>(GetProcAddress(lib, "PJ_get_data_source_vtable"));
#else
  void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<GetVtableFn>(dlsym(lib, "PJ_get_data_source_vtable"));
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: apbin_dump <apbin_source_plugin.dll> <log.bin>\n");
    return 2;
  }
  GetVtableFn get_vtable = loadPlugin(argv[1]);
  if (get_vtable == nullptr) {
    std::fprintf(stderr, "cannot load plugin: %s\n", argv[1]);
    return 2;
  }
  const PJ_data_source_vtable_t* vt = get_vtable();

  FakeHost host;
  PJ_error_t err{};
  void* ctx = vt->create();
  if (!vt->bind(ctx, host.registry(), &err)) {
    std::fprintf(stderr, "bind failed: %s\n", err.message);
    return 1;
  }
  const std::string config = nlohmann::json{{"filepath", argv[2]}}.dump();
  if (!vt->load_config(ctx, {config.data(), config.size()}, &err)) {
    std::fprintf(stderr, "load_config failed: %s\n", err.message);
    return 1;
  }

  const auto start = std::chrono::steady_clock::now();
  const bool ok = vt->start(ctx, &err);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  vt->destroy(ctx);
  std::fprintf(stderr, "load %s in %lld ms\n", ok ? "succeeded" : "FAILED", static_cast<long long>(elapsed));
  if (!ok) {
    std::fprintf(stderr, "start failed: %s\n", err.message);
    return 1;
  }

  std::map<std::string, const Field*> numeric;
  for (const auto& topic : host.topics) {
    for (const auto& field : topic.fields) {
      if (field.type == PJ_PRIMITIVE_TYPE_STRING) {
        for (const auto& [ts, text] : field.strings) {
          std::printf("STRING %s/%s %lld %s\n", topic.name.c_str(), field.name.c_str(), static_cast<long long>(ts),
                      text.c_str());
        }
      } else {
        numeric[topic.name + "/" + field.name] = &field;
      }
    }
  }
  for (const auto& [name, field] : numeric) {
    printSeries(name, field->points);
  }
  for (const auto& message : host.messages) {
    std::printf("REPORT %s\n", message.c_str());
  }
  std::fflush(stdout);
  if (!host.metadata.empty()) {
    std::printf("METADATA %s\n", host.metadata.c_str());
    std::fflush(stdout);
    try {
      const auto doc = nlohmann::json::parse(host.metadata);
      // Named copies: iterating items() of the temporary that value() returns would dangle.
      const auto parameters = doc.value("Parameters", nlohmann::json::object());
      const auto message_logs = doc.value("Message Logs", nlohmann::json::array());
      for (const auto& [name, value] : parameters.items()) {
        std::printf("PARAM %s=%s\n", name.c_str(), value.get<std::string>().c_str());
      }
      for (const auto& line : message_logs) {
        std::printf("MSGLOG %s\n", line.get<std::string>().c_str());
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "metadata is not the expected JSON: %s\n", e.what());
      return 1;
    }
  }
  return 0;
}
