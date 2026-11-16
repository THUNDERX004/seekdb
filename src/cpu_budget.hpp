#ifndef MVD_CPU_BUDGET_HPP
#define MVD_CPU_BUDGET_HPP

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <sched.h>
#endif

namespace mvd::cpu_budget {

// A missing Limit is unknown; unlimited is an explicit absence of a quota.
// Finite quotas count whole CPUs (a sub-CPU quota therefore yields zero).
using Limit = std::optional<unsigned>;
inline constexpr unsigned unlimited = std::numeric_limits<unsigned>::max();

namespace detail {
inline bool space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline std::string_view word(std::string_view &text) noexcept {
  while (!text.empty() && space(text.front())) text.remove_prefix(1);
  std::size_t end = 0;
  while (end < text.size() && !space(text[end])) ++end;
  const auto result = text.substr(0, end);
  text.remove_prefix(end);
  return result;
}

inline std::string_view line(std::string_view &text) noexcept {
  const auto end = text.find('\n');
  auto result = text.substr(0, end);
  text.remove_prefix(end == text.npos ? text.size() : end + 1);
  if (!result.empty() && result.back() == '\r') result.remove_suffix(1);
  return result;
}

inline std::optional<std::uint64_t> number(std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const unsigned digit = static_cast<unsigned>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

inline unsigned whole_cpus(std::uint64_t quota, std::uint64_t period) noexcept {
  // Division avoids overflow and rounding up just below the two-CPU boundary.
  return static_cast<unsigned>(std::min<std::uint64_t>(quota / period, unlimited));
}
}  // namespace detail

inline Limit parse_v2_quota(std::string_view text) noexcept {
  const auto quota_text = detail::word(text);
  const auto period = detail::number(detail::word(text));
  if (!period || *period == 0 || !detail::word(text).empty()) return std::nullopt;
  if (quota_text == "max") return unlimited;
  const auto quota = detail::number(quota_text);
  if (!quota || *quota == 0) return std::nullopt;
  return detail::whole_cpus(*quota, *period);
}

inline Limit parse_v1_quota(std::string_view quota_text,
                           std::string_view period_text) noexcept {
  const auto quota_token = detail::word(quota_text);
  const auto period = detail::number(detail::word(period_text));
  if (!period || *period == 0 || !detail::word(quota_text).empty() ||
      !detail::word(period_text).empty()) return std::nullopt;
  if (quota_token == "-1") return unlimited;
  const auto quota = detail::number(quota_token);
  if (!quota || *quota == 0) return std::nullopt;
  return detail::whole_cpus(*quota, *period);
}

// An injected/native affinity bitmap need not be contiguous or byte-aligned.
inline unsigned count_affinity(const unsigned char *mask, std::size_t bytes) noexcept {
  if (mask == nullptr) return 0;
  unsigned count = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    const auto bits = static_cast<unsigned>(std::popcount(mask[i]));
    if (count > unlimited - bits) return unlimited;
    count += bits;
  }
  return count;
}

// Always retain a serial path, even for unknown or less-than-one-CPU budgets.
inline unsigned calculate(unsigned allowed_cpus, Limit quota) noexcept {
  return std::max(1U, std::min(allowed_cpus, quota.value_or(1U)));
}

enum class ReadStatus { ok, missing, error };
struct File {
  ReadStatus status = ReadStatus::error;
  std::string text;
};

namespace detail {
inline bool csv_has(std::string_view list, std::string_view item) noexcept {
  while (!list.empty()) {
    const auto comma = list.find(',');
    if (list.substr(0, comma) == item) return true;
    list.remove_prefix(comma == list.npos ? list.size() : comma + 1);
  }
  return false;
}

inline bool absolute_path(std::string_view path) noexcept {
  if (path.empty() || path.front() != '/' || path.find('\0') != path.npos) return false;
  if (path == "/") return true;
  path.remove_prefix(1);
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == path.npos) return true;
    path.remove_prefix(slash + 1);
  }
  return false;  // No trailing slash except for the root itself.
}

inline bool beneath(std::string_view path, std::string_view root) noexcept {
  return root == "/" || path == root ||
         (path.size() > root.size() && path.substr(0, root.size()) == root &&
          path[root.size()] == '/');
}

struct Membership {
  bool v2;
  std::string_view path;
};

inline std::optional<Membership> membership(std::string_view text) noexcept {
  std::optional<Membership> v1, v2;
  while (!text.empty()) {
    const auto entry = line(text);
    if (entry.empty()) continue;
    const auto first = entry.find(':');
    if (first == entry.npos) return std::nullopt;
    const auto second = entry.find(':', first + 1);
    if (second == entry.npos) return std::nullopt;
    const auto id = number(entry.substr(0, first));
    if (!id) return std::nullopt;
    const auto controllers = entry.substr(first + 1, second - first - 1);
    const auto path = entry.substr(second + 1);
    const bool unified = *id == 0 && controllers.empty();
    const bool cpu = *id != 0 && csv_has(controllers, "cpu");
    if (!unified && !cpu) continue;
    // Reject traversal and ambiguous escaped proc paths instead of guessing.
    if (!absolute_path(path) || path.find('\\') != path.npos) return std::nullopt;
    auto &selected = unified ? v2 : v1;
    if (selected) return std::nullopt;
    selected = Membership{unified, path};
  }
  // In a hybrid hierarchy the CPU controller can belong to v1, not both.
  return v1 ? v1 : v2;
}

inline std::optional<std::string> mount_path(std::string_view encoded) {
  std::string decoded;
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] != '\\') {
      decoded += encoded[i];
      continue;
    }
    if (encoded.size() - i < 4) return std::nullopt;
    const auto escape = encoded.substr(i, 4);
    if (escape == "\\040") decoded += ' ';
    else if (escape == "\\011") decoded += '\t';
    else if (escape == "\\012") decoded += '\n';
    else if (escape == "\\134") decoded += '\\';
    else return std::nullopt;
    i += 3;
  }
  if (!absolute_path(decoded)) return std::nullopt;
  return decoded;
}

struct Mount {
  std::string root;
  std::string point;
};

inline std::optional<Mount> find_mount(std::string_view text, Membership member) {
  std::optional<Mount> best;
  while (!text.empty()) {
    const auto entry = line(text);
    const auto separator = entry.find(" - ");
    if (separator == entry.npos) continue;
    auto tail = entry.substr(separator + 3);
    const auto type = word(tail);
    const auto source = word(tail);
    const auto options = word(tail);
    // Controller names belong to superblock options, not the source field.
    if (member.v2 ? type != "cgroup2" :
                    (type != "cgroup" || !csv_has(options, "cpu"))) continue;
    if (source.empty() || options.empty() || !word(tail).empty()) return std::nullopt;
    auto front = entry.substr(0, separator);
    for (int i = 0; i < 3; ++i) {  // ID, parent ID, major:minor.
      if (word(front).empty()) return std::nullopt;
    }
    auto root = mount_path(word(front));
    auto point = mount_path(word(front));
    if (!root || !point || word(front).empty()) return std::nullopt;
    if (!beneath(member.path, *root)) continue;
    // Prefer the mount exposing the most ancestors, rather than a bind subtree.
    if (!best || root->size() < best->root.size())
      best = Mount{std::move(*root), std::move(*point)};
  }
  return best;
}

inline std::string child(const std::string &directory, std::string_view name) {
  return directory + (directory == "/" ? "" : "/") + std::string(name);
}

template <class ReadFile>
Limit hierarchy_limit(Membership member, const Mount &mount, ReadFile &read_file) {
  auto suffix = member.path.substr(mount.root == "/" ? 1 : mount.root.size());
  std::string directory = mount.point;
  if (!suffix.empty()) {
    if (suffix.front() == '/') suffix.remove_prefix(1);
    directory = child(directory, suffix);
  }
  unsigned result = unlimited;
  for (;;) {
    const bool at_mount = directory == mount.point;
    const bool at_root = at_mount && mount.root == "/";
    Limit quota;
    if (member.v2) {
      const auto file = read_file(child(directory, "cpu.max"));
      if (file.status == ReadStatus::ok) quota = parse_v2_quota(file.text);
      // A true v2 hierarchy root has neither interface. A namespaced root
      // exposes its local quota; retain it instead of disabling all container
      // parallelism. Ancestors outside the namespace cannot be inspected.
      else if (at_root && file.status == ReadStatus::missing) {
        const auto type = read_file(child(directory, "cgroup.type"));
        if (type.status == ReadStatus::missing) quota = unlimited;
      }
    } else {
      const auto quota_file = read_file(child(directory, "cpu.cfs_quota_us"));
      const auto period_file = read_file(child(directory, "cpu.cfs_period_us"));
      if (at_root && quota_file.status == ReadStatus::missing &&
          period_file.status == ReadStatus::missing) quota = unlimited;
      else if (quota_file.status == ReadStatus::ok && period_file.status == ReadStatus::ok)
        quota = parse_v1_quota(quota_file.text, period_file.text);
    }
    if (!quota) return std::nullopt;
    result = std::min(result, *quota);
    if (at_mount) break;
    const auto slash = directory.rfind('/');
    directory.resize(slash == 0 ? 1 : slash);
  }
  // An explicit subtree mount leaves ancestors unaccounted for. Cgroup
  // namespaces can hide ancestors too; only limits visible in this namespace
  // can be discovered without host access.
  return mount.root == "/" ? Limit{result} : std::nullopt;
}

inline File read_file(const std::string &path) {
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"),
                                                        &std::fclose);
  if (!file) return {errno == ENOENT ? ReadStatus::missing : ReadStatus::error, {}};
  File result{ReadStatus::ok, {}};
  char buffer[4096];
  for (;;) {
    const auto bytes = std::fread(buffer, 1, sizeof(buffer), file.get());
    // Bound one-time allocations, including unusually large mount tables.
    if (result.text.size() + bytes > 1024 * 1024) return {};
    result.text.append(buffer, bytes);
    if (bytes < sizeof(buffer)) {
      if (std::ferror(file.get())) return {};
      return result;
    }
  }
}
}  // namespace detail

// Reader: File(const std::string &absolute_path). All reads are injectable;
// malformed/inaccessible inputs, oversized affinity masks and exceptions are
// conservative serial fallbacks. No Docker or process-affinity changes needed.
template <class ReadFile>
unsigned detect_linux(unsigned allowed_cpus, ReadFile &&read_file) noexcept {
  if (allowed_cpus < 2) return 1;
  try {
    // Match sched_getaffinity(0): threaded cgroups may differ from the leader.
    const auto cgroup = read_file(std::string("/proc/thread-self/cgroup"));
    if (cgroup.status != ReadStatus::ok) return 1;
    const auto member = detail::membership(cgroup.text);
    if (!member) return 1;
    const auto mounts = read_file(std::string("/proc/self/mountinfo"));
    if (mounts.status != ReadStatus::ok) return 1;
    const auto mount = detail::find_mount(mounts.text, *member);
    if (!mount) return 1;
    return calculate(allowed_cpus, detail::hierarchy_limit(*member, *mount, read_file));
  } catch (...) {
    return 1;
  }
}

// Initialize on the scan-submitting thread before starting its worker. This is
// a process-wide snapshot: affinity/cgroup membership/quotas must remain fixed
// after first use. Warm calls perform no syscalls, file reads or allocations.
inline unsigned cached_cpu_count() noexcept {
  static const unsigned count = []() noexcept {
#if defined(__linux__)
#if defined(CPU_SETSIZE)
    cpu_set_t mask{};
    // A fixed mask keeps probing small. EINVAL on a larger kernel mask is an
    // unknown affinity, never a reason to use host hardware_concurrency().
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) return 1U;
    return detect_linux(count_affinity(reinterpret_cast<const unsigned char *>(&mask),
                                       sizeof(mask)), detail::read_file);
#else
    return 1U;
#endif
#else
    // Portable fallback, including Windows; zero means unknown.
    return std::max(1U, std::thread::hardware_concurrency());
#endif
  }();
  return count;
}

inline bool has_two_cpus() noexcept { return cached_cpu_count() >= 2; }

}  // namespace mvd::cpu_budget

#endif
