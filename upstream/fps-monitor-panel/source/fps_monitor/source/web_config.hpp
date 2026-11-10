/* Settings web app: pure helpers (standard library only, host-tested by
 * tests/host/web_config_test.cpp). Allowed values match config.ini.example.
 */
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fpsmon::web {

struct KeySpec {
  std::string_view name;
  std::array<std::string_view, 11> values; // unused slots stay empty
  bool quoted;                            // JSON string (else bool/number)
};

/* The 24 [overlay] keys the web app edits, in JSON order. */
inline constexpr std::array<std::string_view, 9> kSystemMetricNames = {
    "cpu", "gpu", "memory", "fan", "ip", "cpu_clock", "gpu_clock",
    "ssd_temp", "soc_power"};

inline bool valid_system_order(std::string_view value) {
  bool seen[kSystemMetricNames.size()] = {};
  std::size_t count = 0;
  while (true) {
    const std::size_t comma = value.find(',');
    const std::string_view token = value.substr(0, comma);
    std::size_t metric = 0;
    while (metric < kSystemMetricNames.size() &&
           kSystemMetricNames[metric] != token)
      ++metric;
    if (metric == kSystemMetricNames.size() || seen[metric])
      return false;
    seen[metric] = true;
    ++count;
    if (comma == std::string_view::npos)
      return count == kSystemMetricNames.size();
    value.remove_prefix(comma + 1);
  }
}

inline constexpr std::array<KeySpec, 24> kKeys = {{
    {"enabled", {"true", "false"}, false},
    {"edge", {"top", "bottom"}, true},
    {"align", {"left", "center", "right"}, true},
    {"font_size", {"small", "medium", "large"}, true},
    {"detail", {"minimal", "compact", "panel", "full"}, true},
    {"opacity", {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"}, false},
    {"theme", {"classic", "mono", "aurora", "vice"}, true},
    {"brand", {"true", "false"}, false},
    {"fps_pill", {"true", "false"}, false},
    {"system", {"true", "false"}, false},
    {"frame_graph", {"true", "false"}, false},
    {"frame_graph_opacity", {"0", "40", "65", "85"}, false},
    {"margin", {"0", "5", "10", "20"}, false},
    {"show_fps", {"true", "false"}, false},
    {"show_cpu", {"true", "false"}, false},
    {"show_gpu", {"true", "false"}, false},
    {"show_memory", {"true", "false"}, false},
    {"show_ip_address", {"true", "false"}, false},
    {"show_fan_duty", {"true", "false"}, false},
    {"show_cpu_clock", {"true", "false"}, false},
    {"show_gpu_clock", {"true", "false"}, false},
    {"show_ssd_temp", {"true", "false"}, false},
    {"show_soc_power", {"true", "false"}, false},
    {"metrics_order", {}, true},
}};

inline const KeySpec *find_key(std::string_view name) {
  for (const KeySpec &k : kKeys)
    if (k.name == name)
      return &k;
  return nullptr;
}

inline bool valid_value(const KeySpec &k, std::string_view value) {
  if (value.empty())
    return false;
  if (k.name == "metrics_order")
    return valid_system_order(value);
  for (std::string_view v : k.values)
    if (v == value)
      return true;
  return false;
}

/* One x-www-form-urlencoded component: '+' is a space, %XX a byte. */
inline bool form_decode(std::string_view in, std::string &out) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  out.clear();
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%') {
      if (i + 2 >= in.size())
        return false;
      const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += in[i];
    }
  }
  return true;
}

using Updates = std::vector<std::pair<std::string, std::string>>;

inline std::string json_string(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      out += "\\u00";
      out += kHex[u >> 4];
      out += kHex[u & 15];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

inline std::string json_error(std::string_view msg) {
  return "{\"error\":" + json_string(msg) + "}";
}

/* Parse a POST body into validated updates. Returns "" on success, else the
 * error message; `out` is only meaningful on success. */
inline std::string parse_updates(std::string_view body, Updates &out) {
  out.clear();
  if (body.empty())
    return "empty body";
  while (true) {
    const std::size_t amp = body.find('&');
    const std::string_view pair = body.substr(0, amp);
    const std::size_t eq = pair.find('=');
    std::string key, value;
    if (eq == std::string_view::npos || !form_decode(pair.substr(0, eq), key) ||
        !form_decode(pair.substr(eq + 1), value))
      return "malformed body";
    const KeySpec *spec = find_key(key);
    if (!spec)
      return "unknown key " + key;
    bool dup = false;
    for (const auto &u : out)
      dup = dup || u.first == key;
    if (dup || !valid_value(*spec, value))
      return "invalid value for " + key;
    out.emplace_back(std::move(key), std::move(value));
    if (amp == std::string_view::npos)
      return "";
    body.remove_prefix(amp + 1);
  }
}

constexpr std::string_view ini_trim(std::string_view s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n\f\v");
  if (b == std::string_view::npos)
    return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n\f\v") - b + 1);
}

constexpr bool iequals(std::string_view a, std::string_view lower) {
  if (a.size() != lower.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char c = a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i];
    if (c != lower[i])
      return false;
  }
  return true;
}

/* What the server needs from a request head (the bytes before "\r\n\r\n"). */
struct Head {
  std::string_view method, path; // path without ?query
  std::size_t content_length = 0;
  bool token = false; // "X-FPSMon: 1"
};

/* Returns 0, or the HTTP status to answer with and `err` set. */
constexpr int parse_head(std::string_view head, std::size_t max_body,
                         Head &out, const char *&err) {
  constexpr std::size_t npos = std::string_view::npos;
  out = Head{};
  const std::size_t line_end = head.find("\r\n");
  const std::string_view line = head.substr(0, line_end);
  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 = sp1 == npos ? npos : line.find(' ', sp1 + 1);
  if (sp2 == npos) {
    err = "malformed request";
    return 400;
  }
  out.method = line.substr(0, sp1);
  out.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  out.path = out.path.substr(0, out.path.find('?'));

  bool bad_length = false;
  std::string_view rest =
      line_end == npos ? std::string_view{} : head.substr(line_end + 2);
  while (!rest.empty()) {
    const std::size_t e = rest.find("\r\n");
    const std::string_view h = rest.substr(0, e);
    rest = e == npos ? std::string_view{} : rest.substr(e + 2);
    const std::size_t colon = h.find(':');
    if (colon == npos)
      continue;
    const std::string_view name = h.substr(0, colon);
    const std::string_view value = ini_trim(h.substr(colon + 1));
    if (iequals(name, "content-length")) {
      bad_length = value.empty();
      out.content_length = 0;
      for (char c : value) {
        if (c < '0' || c > '9') {
          bad_length = true;
          break;
        }
        out.content_length =
            out.content_length * 10 + static_cast<std::size_t>(c - '0');
        if (out.content_length > max_body)
          break; // too large either way; stop before it can overflow
      }
    } else if (iequals(name, "x-fpsmon")) {
      out.token = value == "1";
    }
  }
  if (bad_length) {
    err = "bad Content-Length";
    return 400;
  }
  if (out.content_length > max_body) {
    err = "request body too large";
    return 413;
  }
  return 0;
}

/* Set section.key=value the way include/ini.h reads it: replace every
 * occurrence, else insert after the section's last key, else create the
 * section (at the top when `at_top`, otherwise at the end). */
inline void ini_set(std::vector<std::string> &lines, std::string_view section,
                    std::string_view key, std::string_view value, bool at_top) {
  const std::string entry = std::string(key) + "=" + std::string(value);
  std::string_view cur;
  bool found = false, first_block = false, seen = false;
  std::size_t insert = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view t = ini_trim(lines[i]);
    if (t.empty() || t[0] == '#' || t[0] == ';')
      continue;
    if (t.front() == '[' && t.back() == ']') {
      cur = ini_trim(t.substr(1, t.size() - 2));
      first_block = cur == section && !seen;
      if (first_block) {
        seen = true;
        insert = i + 1;
      }
      continue;
    }
    const std::size_t eq = t.find('=');
    if (cur != section || eq == std::string_view::npos)
      continue;
    if (ini_trim(t.substr(0, eq)) == key) {
      lines[i] = entry;
      found = true;
    }
    if (first_block)
      insert = i + 1;
  }
  if (found)
    return;
  if (seen) {
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert), entry);
  } else if (at_top) {
    lines.insert(lines.begin(), {"[" + std::string(section) + "]", entry, ""});
  } else {
    if (!lines.empty() && !ini_trim(lines.back()).empty())
      lines.emplace_back();
    lines.push_back("[" + std::string(section) + "]");
    lines.push_back(entry);
  }
}

/* Apply validated updates to config.ini text ("" for a missing file),
 * keeping every other line, and ensure [meta] schema_version=1. */
inline std::string apply_updates(std::string_view ini, const Updates &updates) {
  std::vector<std::string> lines;
  while (!ini.empty()) {
    const std::size_t nl = ini.find('\n');
    lines.emplace_back(ini.substr(0, nl));
    if (nl == std::string_view::npos)
      break;
    ini.remove_prefix(nl + 1);
  }
  ini_set(lines, "meta", "schema_version", "1", true);
  for (const auto &u : updates)
    ini_set(lines, "overlay", u.first, u.second, false);
  std::string out;
  for (const std::string &l : lines)
    out += l + "\n";
  return out;
}

/* {"enabled":true,...} from the 24 values in kKeys order. */
inline std::string config_json(const std::array<std::string_view, 24> &values) {
  std::string out = "{";
  for (std::size_t i = 0; i < kKeys.size(); ++i) {
    out += (i ? ",\"" : "\"") + std::string(kKeys[i].name) + "\":";
    out += kKeys[i].quoted ? json_string(values[i]) : std::string(values[i]);
  }
  return out + "}";
}

} // namespace fpsmon::web
