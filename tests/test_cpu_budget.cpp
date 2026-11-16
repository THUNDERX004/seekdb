// Standalone C++20 checks; no database, Docker, host quotas or affinity changes.
#include "../src/cpu_budget.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace budget = mvd::cpu_budget;
using budget::File;
using budget::Limit;
using budget::ReadStatus;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void parsers() {
  require(budget::parse_v2_quota("max 100000\n") == Limit{budget::unlimited},
          "v2 unlimited quota");
  require(budget::parse_v2_quota(" \t200000 100000\r\n") == Limit{2},
          "v2 two CPUs and whitespace");
  require(budget::parse_v2_quota("199999 100000") == Limit{1},
          "fraction just below two must round down");
  require(budget::parse_v2_quota("299999 100000") == Limit{2},
          "fraction above two must round down");
  require(budget::parse_v2_quota("50000 100000") == Limit{0}, "sub-CPU quota");
  require(budget::parse_v2_quota("18446744073709551614 9223372036854775808") == Limit{1},
          "large ratios must avoid multiplication and floating-point rounding");
  require(budget::parse_v2_quota("18446744073709551615 1") == Limit{budget::unlimited},
          "large whole-CPU count must saturate");
  for (const char *text : {"", "max", "max 0", "MAX 100000", "-1 100000", "0 100000",
                           "200000 0", "2.0 1", "+2 1", "200000 100000 extra",
                           "18446744073709551616 1", "2 18446744073709551616"}) {
    require(!budget::parse_v2_quota(text), "malformed v2 quota accepted");
  }
  require(budget::parse_v1_quota("-1\n", "100000\n") == Limit{budget::unlimited},
          "v1 unlimited quota");
  require(budget::parse_v1_quota(" 200000\t", "100000\n") == Limit{2},
          "v1 two CPUs");
  require(budget::parse_v1_quota("199999", "100000") == Limit{1}, "v1 fractional quota");
  require(budget::parse_v1_quota("1", "100000") == Limit{0}, "v1 sub-CPU quota");
  for (const char *text : {"", "max", "-2", "0", "+2", "1.5", "2 trailing",
                           "18446744073709551616"}) {
    require(!budget::parse_v1_quota(text, "100000"), "malformed v1 quota accepted");
  }
  for (const char *text : {"", "0", "-1", "100000 trailing", "18446744073709551616"}) {
    require(!budget::parse_v1_quota("-1", text), "invalid unlimited v1 period accepted");
  }
}

void affinity_and_calculation() {
  std::array<unsigned char, 256> mask{};
  require(budget::count_affinity(mask.data(), mask.size()) == 0, "empty affinity");
  mask[0] = 0x02;
  require(budget::count_affinity(mask.data(), mask.size()) == 1, "single allowed CPU");
  mask.back() = 0x80;
  require(budget::count_affinity(mask.data(), mask.size()) == 2,
          "sparse affinity must count bits, not highest CPU ID");
  mask[12] = 0x55;
  require(budget::count_affinity(mask.data(), mask.size()) == 6, "affinity population");
  require(budget::count_affinity(nullptr, mask.size()) == 0, "unknown affinity");
  require(budget::count_affinity(mask.data(), 0) == 0, "zero-byte affinity");
#if defined(__linux__) && defined(CPU_SETSIZE)
  cpu_set_t native{};
  CPU_ZERO(&native);
  CPU_SET(0, &native);
  CPU_SET(CPU_SETSIZE - 1, &native);
  require(budget::count_affinity(reinterpret_cast<const unsigned char *>(&native),
                                 sizeof(native)) == 2, "native Linux affinity layout");
#endif
  for (unsigned affinity : {0U, 1U, 2U, 8U}) {
    require(budget::calculate(affinity, std::nullopt) == 1, "unknown quota is serial");
    require(budget::calculate(affinity, Limit{0}) == 1, "sub-CPU budget is serial");
    require(budget::calculate(affinity, Limit{1}) == 1, "one-CPU budget is serial");
  }
  require(budget::calculate(0, Limit{budget::unlimited}) == 1, "unknown affinity is serial");
  require(budget::calculate(1, Limit{8}) == 1, "affinity overrides quota");
  require(budget::calculate(8, Limit{2}) == 2, "quota overrides affinity");
  require(budget::calculate(2, Limit{8}) == 2, "affinity limits quota");
  require(budget::calculate(8, Limit{budget::unlimited}) == 8, "explicit unlimited quota");
  static_assert(noexcept(budget::cached_cpu_count()));
  static_assert(noexcept(budget::has_two_cpus()));
}

struct Fixture {
  std::map<std::string, File> files;
  std::vector<std::string> reads;

  void set(const std::string &path, std::string text) {
    files[path] = {ReadStatus::ok, std::move(text)};
  }
  File read(const std::string &path) {
    reads.push_back(path);
    const auto it = files.find(path);
    return it == files.end() ? File{ReadStatus::missing, {}} : it->second;
  }
  unsigned detect(unsigned affinity = 8) {
    return budget::detect_linux(affinity, [&](const std::string &path) { return read(path); });
  }
  bool read_path(const std::string &path) const {
    return std::find(reads.begin(), reads.end(), path) != reads.end();
  }
};

Fixture v2(std::string path = "/jobs/scan") {
  Fixture fixture;
  fixture.set("/proc/thread-self/cgroup", "0::" + path + "\n");
  fixture.set("/proc/self/mountinfo",
              "31 22 0:28 / /sys/fs/cgroup rw,nosuid shared:7 - cgroup2 cgroup rw\n");
  fixture.set("/sys/fs/cgroup/jobs/scan/cpu.max", "max 100000\n");
  fixture.set("/sys/fs/cgroup/jobs/cpu.max", "max 100000\n");
  // Root cpu.max is intentionally absent, as on a true hierarchy root.
  return fixture;
}

void v2_quotas_and_ancestors() {
  auto fixture = v2();
  require(fixture.detect() == 8, "unlimited visible v2 hierarchy");
  require(fixture.read_path("/sys/fs/cgroup/cpu.max"), "walk must include mounted root");
  fixture.set("/sys/fs/cgroup/jobs/scan/cpu.max", "200000 100000");
  require(fixture.detect() == 2, "v2 leaf quota");
  require(fixture.detect(1) == 1, "v2 leaf quota cannot bypass affinity");
  fixture.set("/sys/fs/cgroup/jobs/scan/cpu.max", "199999 100000");
  require(fixture.detect() == 1, "fractional v2 leaf quota must remain serial");
  fixture.set("/sys/fs/cgroup/jobs/scan/cpu.max", "200000 100000");
  fixture.set("/sys/fs/cgroup/jobs/cpu.max", "199999 100000");
  require(fixture.detect() == 1, "v2 fractional parent quota overrides leaf");
  fixture.set("/sys/fs/cgroup/jobs/cpu.max", "50000 100000");
  require(fixture.detect() == 1, "v2 sub-CPU parent quota");
  fixture.set("/sys/fs/cgroup/jobs/cpu.max", "400000 200000");
  require(fixture.detect() == 2, "each ancestor must use its own period");
  fixture.set("/sys/fs/cgroup/cpu.max", "100000 100000");
  require(fixture.detect() == 1, "namespace-visible root quota must be read");
  auto root = v2("/");
  require(root.detect() == 8, "membership at hierarchy root");
  root.set("/sys/fs/cgroup/cpu.max", "200000 100000");
  require(root.detect() == 2, "two-CPU container retains its visible parallel budget");
  root.set("/sys/fs/cgroup/cpu.max", "199999 100000");
  require(root.detect() == 1, "fractional container quota stays serial");
  root.set("/sys/fs/cgroup/cpu.max", "100000 100000");
  require(root.detect() == 1, "one-CPU container stays serial");
  root.set("/sys/fs/cgroup/cpu.max", "max 100000");
  require(root.detect(2) == 2, "unlimited visible quota still respects affinity");
  root.files.erase("/sys/fs/cgroup/cpu.max");
  root.set("/sys/fs/cgroup/cgroup.type", "domain");
  require(root.detect() == 1, "namespace root remains identifiable without CPU controller");
  root.files.erase("/sys/fs/cgroup/cgroup.type");
  require(root.detect() == 8, "true root has neither non-root interface");
  root.files["/sys/fs/cgroup/cpu.max"] = {ReadStatus::error, {}};
  require(root.detect() == 1, "unreadable root must not become unlimited");
}

void mount_paths() {
  auto fixture = v2();
  fixture.set("/proc/self/mountinfo",
              "30 22 0:28 /jobs /narrow rw - cgroup2 cgroup rw\n"
              "31 22 0:28 / /cgroup\\040mount\\134name rw shared:7 - cgroup2 cgroup rw\n");
  fixture.set("/cgroup mount\\name/jobs/scan/cpu.max", "300000 100000");
  fixture.set("/cgroup mount\\name/jobs/cpu.max", "200000 100000");
  require(fixture.detect() == 2, "escaped mountpoint and preference for full hierarchy");
  require(fixture.read_path("/cgroup mount\\name/jobs/cpu.max"), "decoded ancestor path");
  require(!fixture.read_path("/narrow/scan/cpu.max"), "subtree mount must not hide ancestors");

  fixture = v2();
  fixture.set("/proc/self/mountinfo", "30 22 0:28 /jobs /narrow rw - cgroup2 cgroup rw\n");
  fixture.set("/narrow/scan/cpu.max", "200000 100000");
  fixture.set("/narrow/cpu.max", "max 100000");
  require(fixture.detect() == 1, "unaccounted-for ancestors above bind mount are unknown");
  require(fixture.read_path("/narrow/scan/cpu.max") && fixture.read_path("/narrow/cpu.max"),
          "membership must be relative to mount root");
  require(!fixture.read_path("/cpu.max"), "ancestor walk must stop at mountpoint");

  fixture = v2("/jobs2/scan");
  fixture.set("/proc/self/mountinfo", "30 22 0:28 /jobs /narrow rw - cgroup2 cgroup rw\n");
  require(fixture.detect() == 1, "mount roots must match whole path components");
  require(!fixture.read_path("/narrow/2/scan/cpu.max"), "no prefix-only root mapping");

  fixture = v2("/scan");
  fixture.set("/proc/self/mountinfo", "31 22 0:28 / / rw - cgroup2 cgroup rw\n");
  fixture.set("/scan/cpu.max", "200000 100000");
  require(fixture.detect() == 2, "cgroup mount at filesystem root");
  require(fixture.read_path("/cpu.max") && !fixture.read_path("//cpu.max"),
          "root path joining must not double slashes");
}

void v1_and_hybrid() {
  Fixture fixture;
  fixture.set("/proc/thread-self/cgroup",
              "7:cpuacct:/accounting\n6:cpu,cpuacct:/pool/scan\n0::/unified\n");
  fixture.set("/proc/self/mountinfo",
              "30 22 0:27 / /accounting rw - cgroup cgroup rw,cpuacct\n"
              "31 22 0:28 / /cpu rw - cgroup cgroup rw,cpuacct,cpu\n"
              "32 22 0:29 / /unified rw - cgroup2 cgroup rw\n");
  fixture.set("/cpu/pool/scan/cpu.cfs_quota_us", "-1\n");
  fixture.set("/cpu/pool/scan/cpu.cfs_period_us", "100000\n");
  fixture.set("/cpu/pool/cpu.cfs_quota_us", "500000\n");
  fixture.set("/cpu/pool/cpu.cfs_period_us", "200000\n");
  require(fixture.detect() == 2, "v1 ancestor quota on combined CPU controller mount");
  require(!fixture.read_path("/unified/unified/cpu.max"), "hybrid must use CPU controller");
  require(!fixture.read_path("/accounting/pool/scan/cpu.cfs_quota_us"),
          "cpuacct must not match cpu");
  fixture.set("/cpu/pool/cpu.cfs_quota_us", "399999");
  require(fixture.detect() == 1, "fractional v1 ancestor limit");
  fixture.set("/cpu/pool/cpu.cfs_quota_us", "-1");
  fixture.set("/cpu/cpu.cfs_quota_us", "200000");
  fixture.set("/cpu/cpu.cfs_period_us", "100000");
  require(fixture.detect() == 2, "v1 root limit");
  fixture.files.erase("/cpu/cpu.cfs_period_us");
  require(fixture.detect() == 1, "missing half of v1 quota pair");
  fixture.files.erase("/cpu/cpu.cfs_quota_us");
  fixture.files.erase("/cpu/pool/cpu.cfs_period_us");
  require(fixture.detect() == 1, "missing v1 non-root period");
}

void unknown_and_malformed_inputs() {
  Fixture empty;
  require(empty.detect() == 1, "missing proc metadata");
  auto fixture = v2();
  fixture.files.erase("/sys/fs/cgroup/jobs/cpu.max");
  require(fixture.detect() == 1, "missing non-root quota");
  fixture = v2();
  fixture.set("/sys/fs/cgroup/jobs/cpu.max", "garbage");
  require(fixture.detect() == 1, "malformed ancestor quota");
  fixture.files["/sys/fs/cgroup/jobs/cpu.max"] = {ReadStatus::error, {}};
  require(fixture.detect() == 1, "unreadable ancestor quota");
  fixture.files["/proc/self/mountinfo"] = {ReadStatus::error, {}};
  require(fixture.detect() == 1, "unreadable mount metadata");
  for (const char *membership : {"", "bad", "x::/jobs/scan\n", "0::relative\n",
                                 "0::/jobs/../scan\n", "0::/jobs//scan\n",
                                 "0::/jobs/scan/\n", "0::/escaped\\012name\n",
                                 "0::/jobs/scan\n0::/other\n", "7:cpuacct:/jobs\n"}) {
    fixture = v2();
    fixture.set("/proc/thread-self/cgroup", membership);
    require(fixture.detect() == 1, "invalid/unknown CPU membership must be serial");
  }
  for (const char *mount : {"", "malformed\n",
                            "31 22 0:28 / /sys/fs/cgroup rw - cgroup2\n",
                            "31 22 0:28 / /sys/fs/cgroup rw - cgroup2 cgroup\n",
                            "31 22 0:28 /.. /sys/fs/cgroup rw - cgroup2 cgroup rw\n",
                            "31 22 0:28 / /bad\\999path rw - cgroup2 cgroup rw\n",
                            "31 22 0:28 / relative rw - cgroup2 cgroup rw\n"}) {
    fixture = v2();
    fixture.set("/proc/self/mountinfo", mount);
    require(fixture.detect() == 1, "unusable mount metadata must be serial");
  }
  unsigned reads = 0;
  const auto throwing_reader = [&](const std::string &) -> File {
    ++reads;
    throw std::runtime_error("injected read/allocation failure");
  };
  require(budget::detect_linux(1, throwing_reader) == 1 && reads == 0,
          "one allowed CPU must bypass file reads");
  require(budget::detect_linux(0, throwing_reader) == 1 && reads == 0,
          "unknown affinity must bypass file reads");
  require(budget::detect_linux(8, throwing_reader) == 1 && reads == 1,
          "read exceptions must fall back to serial");
}
}  // namespace

int main() {
  try {
    parsers();
    affinity_and_calculation();
    v2_quotas_and_ancestors();
    mount_paths();
    v1_and_hybrid();
    unknown_and_malformed_inputs();
    std::cout << "cpu budget tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cpu budget test failed: " << error.what() << '\n';
    return 1;
  }
}
