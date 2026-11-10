/* Host tests for the settings web app helpers (web_config.hpp).
 * Built and run by scripts/test.sh. */
#undef NDEBUG
#include "web_config.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace fpsmon::web;

static std::string apply(const char *ini, const char *body) {
  Updates u;
  const std::string err = parse_updates(body, u);
  assert(err.empty());
  return apply_updates(ini, u);
}

int main() {
  /* validation */
  for (const KeySpec &k : kKeys)
    for (std::string_view v : k.values)
      assert(v.empty() || valid_value(k, v));
  assert(valid_value(*find_key("margin"), "20"));
  assert(!valid_value(*find_key("margin"), "15"));
  assert(valid_value(*find_key("opacity"), "0"));
  assert(valid_value(*find_key("opacity"), "10"));
  assert(valid_value(*find_key("opacity"), "100"));
  assert(!valid_value(*find_key("opacity"), "5"));
  assert(!valid_value(*find_key("opacity"), "40.0"));
  assert(!valid_value(*find_key("frame_graph_opacity"), "10"));
  assert(!valid_value(*find_key("enabled"), ""));
  assert(!valid_value(*find_key("theme"), "Vice"));
  assert(find_key("show_ip_address"));
  assert(valid_value(*find_key("show_fan_duty"), "true"));
  assert(!valid_value(*find_key("show_fan_duty"), "yes"));
  assert(valid_value(*find_key("show_cpu_clock"), "true"));
  assert(valid_value(*find_key("show_gpu_clock"), "false"));
  assert(valid_value(*find_key("show_ssd_temp"), "true"));
  assert(valid_value(*find_key("show_soc_power"), "true"));
  assert(valid_value(*find_key("metrics_order"),
                     "cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,soc_power"));
  assert(!valid_value(*find_key("metrics_order"), "cpu,gpu"));
  assert(!valid_value(*find_key("metrics_order"),
                      "cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,ssd_temp"));
  assert(!valid_value(*find_key("metrics_order"),
                      "cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,soc_power,"));

  /* form decode */
  std::string s;
  assert(form_decode("a+b%2Fc%2f", s) && s == "a b/c/");
  assert(!form_decode("%", s));
  assert(!form_decode("%4", s));
  assert(!form_decode("%zz", s));

  Updates u;
  assert(parse_updates("show_cpu=false&show_fan_duty=true", u).empty());
  assert(u.size() == 2 && u[0].first == "show_cpu" &&
         u[1].first == "show_fan_duty");
  assert(parse_updates("enabled=false&theme=vice", u).empty());
  assert(u.size() == 2 && u[0].first == "enabled" && u[1].second == "vice");
  assert(parse_updates("metrics_order=soc_power%2Cssd_temp%2Cgpu_clock%2Ccpu_clock%2Cip%2Cfan%2Cmemory%2Cgpu%2Ccpu", u).empty());
  assert(u.size() == 1 && u[0].first == "metrics_order");
  assert(parse_updates("metrics_order=cpu%2Cgpu", u) ==
         "invalid value for metrics_order");
  assert(parse_updates("th%65me=mono", u).empty() && u[0].first == "theme");
  assert(parse_updates("", u) == "empty body");
  assert(parse_updates("enabled", u) == "malformed body");
  assert(parse_updates("enabled=true&", u) == "malformed body");
  assert(parse_updates("theme=%G1", u) == "malformed body");
  assert(parse_updates("bogus=1", u) == "unknown key bogus");
  assert(parse_updates("theme=red", u) == "invalid value for theme");
  assert(parse_updates("theme=mono&theme=vice", u) ==
         "invalid value for theme");
  assert(json_error("unknown key a\"b") ==
         "{\"error\":\"unknown key a\\\"b\"}");

  /* request head */
  Head h;
  const char *err = nullptr;
  assert(parse_head("POST /api/config?x=1 HTTP/1.1\r\nHost: ps5\r\n"
                    "content-length: 12\r\nX-FPSMon: 1",
                    4096, h, err) == 0);
  assert(h.method == "POST" && h.path == "/api/config" &&
         h.content_length == 12 && h.token);
  assert(parse_head("GET / HTTP/1.1", 4096, h, err) == 0 && h.path == "/" &&
         h.content_length == 0 && !h.token);
  assert(parse_head("POST / HTTP/1.1\r\nX-FPSMon: 0", 4096, h, err) == 0 &&
         !h.token);
  assert(parse_head("GET /", 4096, h, err) == 400 &&
         std::string(err) == "malformed request");
  assert(parse_head("POST / HTTP/1.1\r\nContent-Length: 12abc", 4096, h,
                    err) == 400 &&
         std::string(err) == "bad Content-Length");
  assert(parse_head("POST / HTTP/1.1\r\nContent-Length:", 4096, h, err) ==
         400);
  assert(parse_head("POST / HTTP/1.1\r\nContent-Length: 4097", 4096, h,
                    err) == 413);
  assert(parse_head("POST / HTTP/1.1\r\nContent-Length: "
                    "99999999999999999999999999",
                    4096, h, err) == 413);

  /* missing file */
  assert(apply("", "enabled=false&margin=5") ==
         "[meta]\nschema_version=1\n\n[overlay]\nenabled=false\nmargin=5\n");

  /* existing key replaced, missing key appended to [overlay], other sections
   * and comments untouched */
  const char *ini = "# top comment\n"
                    "[meta]\n"
                    "schema_version=1\n"
                    "\n"
                    "[overlay]\n"
                    "# theme picks the colours\n"
                    " theme = classic\r\n"
                    "margin=20\n"
                    "\n"
                    "# logging comment\n"
                    "[logging]\n"
                    "theme=keep\n";
  assert(apply(ini, "theme=vice&enabled=false") ==
         "# top comment\n"
         "[meta]\n"
         "schema_version=1\n"
         "\n"
         "[overlay]\n"
         "# theme picks the colours\n"
         "theme=vice\n"
         "margin=20\n"
         "enabled=false\n"
         "\n"
         "# logging comment\n"
         "[logging]\n"
         "theme=keep\n");

  /* missing [overlay] created at the end; no trailing newline handled */
  assert(apply("[meta]\nschema_version=1", "edge=bottom") ==
         "[meta]\nschema_version=1\n\n[overlay]\nedge=bottom\n");

  /* schema_version ensured: added to [meta], fixed, or [meta] prepended */
  assert(apply("[meta]\n[overlay]\nedge=top\n", "edge=bottom") ==
         "[meta]\nschema_version=1\n[overlay]\nedge=bottom\n");
  assert(apply("[meta]\nschema_version=2\n", "brand=true") ==
         "[meta]\nschema_version=1\n\n[overlay]\nbrand=true\n");
  assert(apply("[overlay]\nbrand=false\n", "brand=true") ==
         "[meta]\nschema_version=1\n\n[overlay]\nbrand=true\n");

  /* JSON */
  assert(config_json({"true", "top", "left", "medium", "panel", "85",
                      "classic", "false", "false", "true", "true", "85",
                      "20", "true", "true", "true", "true", "false",
                      "false", "true", "true", "true", "true",
                      "cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,soc_power"}) ==
         "{\"enabled\":true,\"edge\":\"top\",\"align\":\"left\","
         "\"font_size\":\"medium\",\"detail\":\"panel\",\"opacity\":85,"
         "\"theme\":\"classic\",\"brand\":false,\"fps_pill\":false,"
         "\"system\":true,\"frame_graph\":true,\"frame_graph_opacity\":85,"
         "\"margin\":20,\"show_fps\":true,\"show_cpu\":true,"
         "\"show_gpu\":true,\"show_memory\":true,"
         "\"show_ip_address\":false,\"show_fan_duty\":false,\"show_cpu_clock\":true,\"show_gpu_clock\":true,\"show_ssd_temp\":true,\"show_soc_power\":true,"
                         "\"metrics_order\":\"cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,soc_power\"}");

  std::puts("web_config_test: ok");
  return 0;
}
