/* Host tests for the pure overlay layout (overlay_scene.hpp).
 *
 *   scene_test               run the checks; exit 1 on any failure
 *   scene_test --render DIR  also write sample scenes as SVG into DIR
 *
 * Built and run by scripts/test.sh. */
#include "overlay_scene.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace onion::overlay;
using namespace onion::overlay::scene;

static int g_pass = 0, g_fail = 0;
/* The scene combination being checked; failures are grouped by message so
 * one layout bug does not print once per combination. */
static std::string g_combo;
static std::map<std::string, std::pair<int, std::string>> g_failures;

static void check(bool ok, const char *expr, int line, const std::string &msg) {
  if (ok) {
    ++g_pass;
    return;
  }
  ++g_fail;
  auto &f = g_failures["line " + std::to_string(line) + ": " + expr + "  [" +
                       msg + "]"];
  if (f.first++ == 0)
    f.second = g_combo;
}
#define CHECK(c, ctx) check((c), #c, __LINE__, (ctx))

static bool near(float a, float b, float eps = 0.01f) {
  return std::fabs(a - b) <= eps;
}
static bool same(Rgba a, Rgba b) {
  return near(a.r, b.r, 0.002f) && near(a.g, b.g, 0.002f) &&
         near(a.b, b.b, 0.002f) && near(a.a, b.a, 0.002f);
}
static bool starts(const std::string &s, const char *p) {
  return s.rfind(p, 0) == 0;
}

/* ---------------------------------------------------------------- */
/* Fixtures                                                          */
/* ---------------------------------------------------------------- */

enum class State { Locked, Drop, NoGame };
static const char *state_name(State s) {
  return s == State::Locked ? "locked" : s == State::Drop ? "drop" : "nogame";
}

struct Fixture {
  Readings r;
  Histories h;
};

static Fixture make(State st) {
  Fixture f;
  Readings &r = f.r;
  r.title_id = "PPSA01234";
  r.fps_target = 60;
  r.session_s = 252;
  SystemReadings &s = r.sys;
  s.cpu_valid = true;
  const float cores[8] = {38, 51, 44, 29, 62, 35, 40, 37};
  for (int i = 0; i < 8; ++i)
    s.cpu_core[i] = cores[i];
  s.cpu_pct = 42.0f;
  s.cpu_temp_valid = true;
  s.cpu_temp = 58;
  s.soc_temp_valid = true;
  s.soc_temp = 61;
  s.power_valid = true;
  s.gpu_power_w = 74.0f;
  s.soc_power_valid = true;
  s.soc_power_w = 128.0f;
  s.cpu_clock_valid = true;
  s.cpu_clock_mhz = 3600;
  s.gpu_clock_valid = true;
  s.gpu_clock_mhz = 2200;
  s.ssd_temp_valid = true;
  s.ssd_temp = 53;
  s.ram_valid = true;
  s.ram_used_mb = 9830;
  s.ram_total_mb = 12800;
  s.vram_valid = true;
  s.vram_used_mb = 4096;
  s.vram_total_mb = 8192;
  s.fan_valid = true;
  s.fan_duty_pct = 35;
  s.ip_valid = true;
  s.ip_address = "192.168.2.210";
  if (st == State::NoGame)
    return f;

  r.fps_valid = true;
  r.low_per_frame = true;
  if (st == State::Locked) {
    r.fps = 60.0f;
    r.frame_ms = 16.7f;
    r.frame_ms_max = 17.4f;
    r.fps_avg = 59.9f;
    r.fps_low = 58.2f;
    for (int i = 0; i < kFpsBars; ++i)
      f.h.fps.push(59.6f + 0.1f * static_cast<float>(i % 5));
    for (int i = 0; i < kLongBars; ++i) {
      const float ms = 16.4f + 0.1f * static_cast<float>(i % 7);
      f.h.frame_ms.push(ms);
      f.h.long_ms.push(ms);
    }
    for (int i = 0; i < 1000; ++i)
      r.session.add(60.0f, 60);
    return f;
  }

  /* A drop: dips to Warn and Bad, a 240 ms stall, six overflowing frames. */
  r.fps = 38.0f;
  r.frame_ms = 26.3f;
  r.frame_ms_max = 240.0f;
  r.fps_avg = 52.4f;
  r.fps_low = 31.0f;
  for (int i = 0; i < kFpsBars; ++i) {
    float v = 60.0f;
    if (i >= 22 && i < 26)
      v = 50.0f;
    else if (i >= 26 && i < 31)
      v = 31.0f + static_cast<float>(i - 26);
    else if (i >= 36)
      v = 38.0f;
    f.h.fps.push(v);
  }
  const float stalls[6] = {60, 240, 120, 70, 180, 90}; /* all > 50 ms */
  for (int i = 0; i < kLongBars; ++i) {
    float ms = 16.7f + (i % 9 == 0 ? 6.0f : 0.0f);
    if (i % 37 == 20)
      ms = stalls[(i / 37) % 6];
    f.h.long_ms.push(ms);
    f.h.frame_ms.push(i == kLongBars - 3 ? 240.0f : ms);
  }
  for (int i = 0; i < 1000; ++i)
    r.session.add(i % 100 < 6 ? 30.0f : i % 100 < 10 ? 50.0f : 60.0f, 60);
  return f;
}

static Options options(Detail d, Theme t) {
  Options o;
  o.screen_w = 1920.0f;
  o.screen_h = 1080.0f;
  o.edge = BarEdge::Top;
  o.align = BarAlign::Left;
  o.detail = d;
  o.font_pt = 18; /* medium */
  o.opacity = 85;
  o.margin = 20.0f;
  o.frame_graph = true;
  o.graph_opacity = 85;
  o.theme = t;
  return o;
}

/* ---------------------------------------------------------------- */
/* Scene geometry helpers                                            */
/* ---------------------------------------------------------------- */

struct Box {
  float x0, y0, x1, y1;
};

/* Where the glyphs land: estimated advance, aligned in the cell, one pt tall
 * from the label top (the renderer puts the label at y + margin_top). */
static Box glyphs(const Text &t) {
  const float adv = text_advance(t.text.c_str(), static_cast<float>(t.pt));
  const float top = t.y + t.margin_top;
  if (t.align == TextAlign::Right)
    return {t.x + t.w - adv, top, t.x + t.w, top + static_cast<float>(t.pt)};
  return {t.x, top, t.x + adv, top + static_cast<float>(t.pt)};
}

static bool inside(Box b, Box outer, float slack = 0.5f) {
  return b.x0 >= outer.x0 - slack && b.y0 >= outer.y0 - slack &&
         b.x1 <= outer.x1 + slack && b.y1 <= outer.y1 + slack;
}

static const Rect *find_rect(const Scene &s, const std::string &id) {
  for (const Rect &r : s.rects)
    if (r.id == id)
      return &r;
  return nullptr;
}
static const Text *find_text(const Scene &s, const std::string &id) {
  for (const Text &t : s.texts)
    if (t.id == id)
      return &t;
  return nullptr;
}

static bool is_graph(const std::string &id) { return starts(id, "id_osc_graph_"); }
static bool is_pill(const std::string &id) { return starts(id, "id_osc_pill_"); }

/* ---------------------------------------------------------------- */
/* Scene checks                                                      */
/* ---------------------------------------------------------------- */

static void check_scene(Detail d, Theme t, State st, BarEdge edge,
                        BarAlign align, bool pill) {
  static const char *dn[] = {"minimal", "compact", "panel", "full"};
  static const char *tn[] = {"classic", "mono", "aurora", "vice"};
  char ctxbuf[160];
  std::snprintf(ctxbuf, sizeof ctxbuf, "%s/%s/%s/edge=%s/align=%d/pill=%d",
                dn[static_cast<int>(d)], tn[static_cast<int>(t)],
                state_name(st), edge == BarEdge::Top ? "top" : "bottom",
                static_cast<int>(align), pill ? 1 : 0);
  g_combo = ctxbuf;
  const std::string ctx = "";

  const Fixture f = make(st);
  Options o = options(d, t);
  o.edge = edge;
  o.align = align;
  o.fps_pill = pill;
  const Scene s = build_scene(o, f.r, f.h);
  const bool panel = d == Detail::Panel || d == Detail::Full;
  const float k = static_cast<float>(o.font_pt) / 18.0f;

  if ((d == Detail::Minimal || d == Detail::Compact) && o.show_fps) {
    const Text *fps = find_text(s, "id_osc_fps_v");
    CHECK(fps && fps->align == TextAlign::Left,
          ctx + " FPS value uses explicit centered-slot placement");
    if (fps) {
      const onion::overlay::scene::Metrics m(o.font_pt);
      float slot_x = 0.0f;
      float slot_width = 0.0f;
      if (d == Detail::Minimal) {
        const Rect *dot = find_rect(s, "id_osc_dot");
        CHECK(dot != nullptr, ctx + " minimal FPS dot exists");
        if (dot)
          slot_x = dot->x + dot->w + std::round(8.0f * k);
        slot_width = onion::overlay::scene::detail::slot_w({"888"}, m.pill_pt);
      } else {
        slot_x = s.x + std::round(14.0f * k);
        slot_width = onion::overlay::scene::detail::slot_w({"888"}, m.strip_pt);
      }
      const float text_w =
          text_advance(fps->text.c_str(), static_cast<float>(fps->pt));
      const float expected_x = slot_x + (slot_width - text_w) * 0.5f;
      CHECK(std::fabs(glyphs(*fps).x0 - expected_x) < 0.51f,
            ctx + " FPS glyphs are horizontally centered in their slot");
    }
  }

  CHECK(!s.rects.empty() && !s.texts.empty(), ctx);
  CHECK(same(g_palette.text, palette(Theme::Classic).text) &&
            g_palette.surface_rgb == palette(Theme::Classic).surface_rgb,
        ctx + " palette restored after build_scene");

  /* Unique ids across rects and texts. */
  std::set<std::string> ids;
  for (const Rect &r : s.rects)
    CHECK(ids.insert(r.id).second, ctx + " dup rect id " + r.id);
  for (const Text &x : s.texts)
    CHECK(ids.insert(x.id).second, ctx + " dup text id " + x.id);
  for (const std::string &id : ids)
    CHECK(starts(id, "id_osc_"), ctx + " id prefix " + id);

  /* Everything on screen. */
  const Box screen = {0, 0, s.screen_w, s.screen_h};
  for (const Rect &r : s.rects)
    CHECK(inside({r.x, r.y, r.x + r.w, r.y + r.h}, screen),
          ctx + " rect off screen " + r.id);
  for (const Text &x : s.texts) {
    CHECK(inside({x.x, x.y, x.x + x.w, x.y + x.h}, screen),
          ctx + " text cell off screen " + x.id);
    CHECK(inside(glyphs(x), screen), ctx + " glyphs off screen " + x.id);
  }

  /* Main content inside its own box (graph and pill are separate boxes). */
  const Box main = {s.x, s.y, s.x + s.w, s.y + s.h};
  for (const Rect &r : s.rects)
    if (!is_graph(r.id) && !is_pill(r.id))
      CHECK(inside({r.x, r.y, r.x + r.w, r.y + r.h}, main),
            ctx + " rect outside box " + r.id);
  for (const Text &x : s.texts)
    if (!is_graph(x.id) && !is_pill(x.id)) {
      const Box g = glyphs(x);
      char b[200];
      std::snprintf(b, sizeof b, " glyphs outside box %s '%s' [%.0f,%.0f..%.0f,%.0f] box [%.0f,%.0f..%.0f,%.0f]",
                    x.id.c_str(), x.text.c_str(), g.x0, g.y0, g.x1, g.y1,
                    main.x0, main.y0, main.x1, main.y1);
      CHECK(inside(g, main), ctx + b);
    }

  /* No two text runs draw over each other. */
  for (size_t i = 0; i < s.texts.size(); ++i)
    for (size_t j = i + 1; j < s.texts.size(); ++j) {
      const Text &a = s.texts[i], &c = s.texts[j];
      const Box ga = glyphs(a), gc = glyphs(c);
      const float ox = std::min(ga.x1, gc.x1) - std::max(ga.x0, gc.x0);
      const float oy = std::min(ga.y1, gc.y1) - std::max(ga.y0, gc.y0);
      const float min_pt = static_cast<float>(std::min(a.pt, c.pt));
      char b[200];
      std::snprintf(b, sizeof b, " text overlap %s '%s' / %s '%s' (%.0f px)",
                    a.id.c_str(), a.text.c_str(), c.id.c_str(),
                    c.text.c_str(), ox);
      CHECK(!(ox > 0.5f && oy > 0.25f * min_pt), ctx + b);
    }

  /* HITCH badge: panel only, only while a frame is that long. */
  const bool hitch = find_text(s, "id_osc_hitch") != nullptr;
  CHECK(hitch == (panel && st == State::Drop), ctx + " hitch badge");

  /* Frame graph: 70 % wide, centred, 24 px off the far edge. */
  const Rect *gbg = find_rect(s, "id_osc_graph_bg");
  CHECK(gbg != nullptr, ctx + " graph box");
  if (gbg) {
    CHECK(near(gbg->w, std::round(0.7f * s.screen_w)), ctx + " graph width");
    CHECK(near(gbg->x + gbg->w * 0.5f, s.screen_w * 0.5f, 1.0f),
          ctx + " graph centred");
    if (edge == BarEdge::Top)
      CHECK(near(gbg->y + gbg->h, s.screen_h - std::round(24.0f * k)),
            ctx + " graph bottom edge");
    else
      CHECK(near(gbg->y, std::round(24.0f * k)), ctx + " graph top edge");
    const Box gb = {gbg->x, gbg->y, gbg->x + gbg->w, gbg->y + gbg->h};
    for (const Rect &r : s.rects)
      if (is_graph(r.id))
        CHECK(inside({r.x, r.y, r.x + r.w, r.y + r.h}, gb),
              ctx + " graph rect outside graph " + r.id);
    for (const Text &x : s.texts)
      if (is_graph(x.id))
        CHECK(inside(glyphs(x), gb), ctx + " graph text outside graph " + x.id);
    const bool apart = main.x1 <= gb.x0 || gb.x1 <= main.x0 ||
                       main.y1 <= gb.y0 || gb.y1 <= main.y0;
    CHECK(apart, ctx + " overlay box overlaps the graph");
  }

  /* Overflow labels: at most 4; the drop fixture has 6 candidates. */
  int ovf = 0;
  for (const Text &x : s.texts)
    if (starts(x.id, "id_osc_graph_ovf")) {
      ++ovf;
      CHECK(same(x.color, palette(t).bad), ctx + " overflow colour");
    }
  CHECK(ovf <= 4, ctx + " overflow count " + std::to_string(ovf));
  CHECK(ovf == (st == State::Drop ? 4 : 0),
        ctx + " overflow count " + std::to_string(ovf));

  /* Watermark: panel and full only. */
  const Text *wm = find_text(s, "id_osc_wm");
  CHECK((wm != nullptr) == panel, ctx + " watermark presence");
  if (wm)
    CHECK(wm->text == kWatermarkText, ctx + " watermark text");

  /* Added telemetry must remain independent from upstream GPU and RAM
   * details. Fan duty has its own dedicated metric and is tested separately
   * by check_metric_visibility(). */
  if (panel) {
    const Text *gpu_detail = find_text(s, "id_osc_gpu_t");
    CHECK(gpu_detail && gpu_detail->text.find("F35%") == std::string::npos,
          ctx + " fan duty is not appended to GPU detail");
    const Text *mem_detail = find_text(s, "id_osc_mem_u");
    CHECK(mem_detail && mem_detail->text.find("V 4.0GB") != std::string::npos,
          ctx + " VRAM usage displayed");
  }

  /* Classic keeps today's colours. */
  if (t == Theme::Classic && panel) {
    const Rect *bg = find_rect(s, "id_osc_bg");
    CHECK(bg && same(bg->color, hex(0x0B0E13, 0.85f)), ctx + " classic bg");
    CHECK(find_rect(s, "id_osc_acc") == nullptr, ctx + " classic has no accent");
    const Text *head = find_text(s, "id_osc_head_l");
    CHECK(head && same(head->color, hex(0xA7B0B8)), ctx + " classic label");
    if (st != State::NoGame) {
      const Text *avg_l = find_text(s, "id_osc_avg_l");
      CHECK(avg_l && same(avg_l->color, hex(0xC9D0D6)), ctx + " classic heading");
      const Text *fps = find_text(s, "id_osc_fps_v");
      CHECK(fps && same(fps->color, st == State::Locked ? hex(0xEEF2F5)
                                                        : hex(0xFF7A59)),
            ctx + " classic fps value");
      const Rect *bar = find_rect(s, "id_osc_fbar0");
      CHECK(bar && same(bar->color, hex(0x5AA9E6)), ctx + " classic good bar");
    }
    if (st == State::Drop) {
      const Text *h = find_text(s, "id_osc_hitch");
      CHECK(h && same(h->color, hex(0xFF7A59)), ctx + " classic bad");
      const Rect *warn = find_rect(s, "id_osc_fbar22");
      CHECK(warn && same(warn->color, hex(0xF5B13D)), ctx + " classic warn");
    }
  }
}

/* ---------------------------------------------------------------- */
/* Pure helpers                                                      */
/* ---------------------------------------------------------------- */

static void check_helpers() {
  /* Classic palette = the pre-theme constants. */
  const Palette c = palette(Theme::Classic);
  CHECK(c.surface_rgb == 0x0B0E13u, "classic surface");
  CHECK(same(c.text, hex(0xEEF2F5)) && same(c.heading, hex(0xC9D0D6)) &&
            same(c.label, hex(0xA7B0B8)) && same(c.good, hex(0x5AA9E6)) &&
            same(c.warn, hex(0xF5B13D)) && same(c.bad, hex(0xFF7A59)) &&
            c.accent == Accent::None,
        "classic colours");
  {
    const ThemeScope scope(Theme::Vice);
    CHECK(same(kGood, palette(Theme::Vice).good), "ThemeScope switches");
  }
  CHECK(same(kGood, hex(0x5AA9E6)), "ThemeScope restores");

  /* TargetTracker: a seeded reset holds the 30 cap from the first bucket. */
  {
    TargetTracker tt(20);
    tt.reset(infer_target(30.0f));
    CHECK(tt.update(30.0f) == 30, "tracker seeded");
    tt.reset();
    CHECK(tt.target() == 60, "tracker default reset");
  }

  /* SessionStats */
  SessionStats s;
  CHECK(s.buckets() == 0 && s.avg() < 0 && s.low() < 0 && s.below_pct() < 0 &&
            s.drops() == 0,
        "session empty");
  for (int i = 0; i < 99; ++i)
    s.add(60.0f, 60);
  s.add(30.0f, 60);
  CHECK(s.buckets() == 100, "session buckets");
  CHECK(near(s.avg(), 59.7f), "session avg " + std::to_string(s.avg()));
  CHECK(near(s.low(), 30.0f), "session low (1 of 100)");
  CHECK(near(s.below_pct(), 1.0f), "session below " + std::to_string(s.below_pct()));
  CHECK(s.drops() == 1, "session one drop");
  s.add(30.0f, 60); /* still in the same drop */
  CHECK(s.drops() == 1, "consecutive bad counts once");
  s.add(50.0f, 60); /* Warn: leaves the drop */
  s.add(20.0f, 60);
  CHECK(s.drops() == 2, "re-entry counts again");
  s.add(NAN, 60);
  s.add(-1.0f, 60);
  CHECK(s.buckets() == 103, "non-finite / negative ignored");
  s.reset();
  CHECK(s.buckets() == 0 && s.avg() < 0 && s.low() < 0 && s.drops() == 0,
        "session reset");
  /* 200 buckets: LOW needs 2, so the second slowest. */
  s.add(20.0f, 60);
  s.add(25.4f, 60);
  for (int i = 0; i < 198; ++i)
    s.add(60.0f, 60);
  CHECK(near(s.low(), 25.0f), "session low (2 of 200) " + std::to_string(s.low()));
  CHECK(near(s.below_pct(), 1.0f), "session below 2/200");
  s.reset();
  s.add(999.0f, 60);
  CHECK(near(s.low(), static_cast<float>(SessionStats::kMaxFps)), "low clamps");

  /* Stability */
  Ring<kFpsBars> fps;
  CHECK(count_stability(fps, 60).total() == 0 && Stability{}.pct(0) == 0,
        "stability empty");
  for (int i = 0; i < kFpsBars; ++i)
    fps.push(i < 30 ? 60.0f : i < 36 ? 50.0f : 30.0f);
  const Stability st = count_stability(fps, 60);
  CHECK(st.on_target == 30 && st.slow == 6 && st.drop == 4, "stability split");
  CHECK(st.total() == fps.size(), "stability sums to bucket count");
  CHECK(st.pct(st.on_target) == 75 && st.pct(st.slow) == 15 &&
            st.pct(st.drop) == 10,
        "stability pct");

  /* overflow_picks */
  const std::vector<float> v = {10, 60, 200, 55, 300, 70, 90, 51};
  CHECK((overflow_picks(v, 50.0f, 4) == std::vector<int>{2, 4, 5, 6}),
        "overflow tallest 4 in index order");
  CHECK((overflow_picks(v, 50.0f, 10) == std::vector<int>{1, 2, 3, 4, 5, 6, 7}),
        "overflow fewer than max");
  CHECK((overflow_picks({60, 60, 60}, 50.0f, 2) == std::vector<int>{0, 1}),
        "overflow tie keeps older");
  CHECK(overflow_picks({50, 49}, 50.0f, 4).empty(), "overflow strictly over");
  CHECK(overflow_picks(v, 50.0f, 0).empty() && overflow_picks({}, 0.0f, 4).empty(),
        "overflow none");

  /* Small helpers the panel relies on. */
  CHECK(std::string(platform_label("CUSA00001")) == "PS4" &&
            std::string(platform_label("PPSA00001")) == "PS5" &&
            std::string(platform_label("NPXS1")).empty(),
        "platform_label");
  CHECK(!is_hitch(100.0f, 60) && is_hitch(100.5f, 60) && is_hitch(240.0f, 30) &&
            !is_hitch(99.0f, 30) && is_hitch(101.0f, 0),
        "is_hitch");
  CHECK(fmt_clock(252) == "4:12" && fmt_clock(3725) == "62:05" &&
            fmt_clock(-5) == "0:00",
        "fmt_clock");
}

/* ---------------------------------------------------------------- */
/* SVG render                                                        */
/* ---------------------------------------------------------------- */

static std::string xml(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&')
      o += "&amp;";
    else if (c == '<')
      o += "&lt;";
    else if (c == '>')
      o += "&gt;";
    else
      o += c;
  }
  return o;
}

static std::string fill(Rgba c) {
  char b[96];
  std::snprintf(b, sizeof b, "fill=\"rgb(%d,%d,%d)\" fill-opacity=\"%.3f\"",
                static_cast<int>(std::lround(c.r * 255)),
                static_cast<int>(std::lround(c.g * 255)),
                static_cast<int>(std::lround(c.b * 255)), c.a);
  return b;
}

/* `crop`: only the overlay box plus a margin; otherwise the whole screen. */
static bool write_svg(const std::string &path, const Scene &s, bool crop) {
  std::ofstream o(path);
  if (!o)
    return false;
  const float pad = 24.0f;
  const float vx = crop ? s.x - pad : 0, vy = crop ? s.y - pad : 0;
  const float vw = crop ? s.w + 2 * pad : s.screen_w;
  const float vh = crop ? s.h + 2 * pad : s.screen_h;
  char b[512];
  std::snprintf(b, sizeof b,
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" "
                "height=\"%.0f\" viewBox=\"%.0f %.0f %.0f %.0f\">\n",
                vw * (crop ? 2 : 1), vh * (crop ? 2 : 1), vx, vy, vw, vh);
  o << b;
  /* A dark, game-like backdrop: dusk sky, hills, a lit window or two. */
  std::snprintf(b, sizeof b,
                "<defs><linearGradient id=\"sky\" x1=\"0\" y1=\"0\" x2=\"0\" "
                "y2=\"1\"><stop offset=\"0\" stop-color=\"#1b2433\"/><stop "
                "offset=\"0.55\" stop-color=\"#3a3c4a\"/><stop offset=\"1\" "
                "stop-color=\"#1a1f1a\"/></linearGradient></defs>\n"
                "<rect width=\"%.0f\" height=\"%.0f\" fill=\"url(#sky)\"/>\n",
                s.screen_w, s.screen_h);
  o << b;
  o << "<circle cx=\"1500\" cy=\"260\" r=\"90\" fill=\"#c9a66b\" "
       "fill-opacity=\"0.55\"/>\n"
       "<polygon points=\"0,700 300,520 620,660 900,480 1250,640 1600,500 "
       "1920,620 1920,1080 0,1080\" fill=\"#22301f\"/>\n"
       "<polygon points=\"0,860 420,740 860,830 1300,720 1920,840 1920,1080 "
       "0,1080\" fill=\"#141a13\"/>\n"
       "<rect x=\"180\" y=\"560\" width=\"260\" height=\"140\" "
       "fill=\"#6d5a3c\"/>\n<rect x=\"220\" y=\"600\" width=\"60\" "
       "height=\"40\" fill=\"#f0d58a\"/>\n";
  for (const Rect &r : s.rects) {
    std::snprintf(b, sizeof b,
                  "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" ",
                  r.x, r.y, r.w, r.h);
    o << b << fill(r.color) << "/>\n";
  }
  for (const Text &t : s.texts) {
    const bool right = t.align == TextAlign::Right;
    /* Baseline: label top plus most of one em. */
    const float base = t.y + t.margin_top + 0.95f * static_cast<float>(t.pt);
    std::snprintf(b, sizeof b,
                  "<text x=\"%.1f\" y=\"%.1f\" font-family=\"DejaVu Sans, "
                  "sans-serif\" font-size=\"%d\" font-weight=\"%d\" "
                  "text-anchor=\"%s\" ",
                  right ? t.x + t.w : t.x, base, t.pt,
                  t.weight >= 900 ? 800 : 400, right ? "end" : "start");
    o << b << fill(t.color) << ">" << xml(t.text) << "</text>\n";
  }
  o << "</svg>\n";
  return static_cast<bool>(o);
}

static int render(const std::string &dir) {
  struct Job {
    const char *name;
    Detail d;
    Theme t;
    State st;
    bool crop;
  } jobs[] = {
      {"panel_classic_locked", Detail::Panel, Theme::Classic, State::Locked, true},
      {"panel_classic_drop", Detail::Panel, Theme::Classic, State::Drop, true},
      {"panel_aurora", Detail::Panel, Theme::Aurora, State::Drop, true},
      {"panel_vice", Detail::Panel, Theme::Vice, State::Drop, true},
      {"panel_mono", Detail::Panel, Theme::Mono, State::Drop, true},
      {"panel_classic_nogame", Detail::Panel, Theme::Classic, State::NoGame, true},
      {"compact_classic", Detail::Compact, Theme::Classic, State::Drop, true},
      {"minimal_classic", Detail::Minimal, Theme::Classic, State::Locked, true},
      {"screen_1920x1080", Detail::Panel, Theme::Classic, State::Drop, false},
  };
  int bad = 0;
  for (const Job &j : jobs) {
    const Fixture f = make(j.st);
    const Scene s = build_scene(options(j.d, j.t), f.r, f.h);
    const std::string path = dir + "/" + j.name + ".svg";
    if (!write_svg(path, s, j.crop)) {
      std::printf("cannot write %s\n", path.c_str());
      ++bad;
    }
  }
  return bad;
}

static bool appears_before(const Text *a, const Text *b) {
  if (!a || !b)
    return false;
  return a->y < b->y - 0.01f ||
         (near(a->y, b->y) && a->x < b->x);
}

static void check_metric_order() {
  const Fixture f = make(State::Locked);
  const std::array<int, kSystemMetricCount> reversed = {
      kSystemSocPower, kSystemSsdTemp, kSystemGpuClock, kSystemCpuClock,
      kSystemIp, kSystemFan, kSystemMemory, kSystemGpu, kSystemCpu};
  const char *compact_ids[kSystemMetricCount] = {
      "id_osc_scpu_l", "id_osc_sgpu_l", "id_osc_smem_l",
      "id_osc_sfan_l", "id_osc_sip_l", "id_osc_scclk_l",
      "id_osc_sgclk_l", "id_osc_sssd_l", "id_osc_ssocpwr_l"};
  const char *panel_ids[kSystemMetricCount] = {
      "id_osc_cpu_l", "id_osc_gpu_l", "id_osc_mem_l",
      "id_osc_fan_l", "id_osc_ip_l", "id_osc_cclk_l",
      "id_osc_gclk_l", "id_osc_ssd_l", "id_osc_socpwr_l"};

  Options compact = options(Detail::Compact, Theme::Classic);
  compact.show_fan_duty = true;
  compact.show_ip_address = true;
  compact.system_order = reversed;
  const Scene compact_scene = build_scene(compact, f.r, f.h);
  for (int i = 0; i + 1 < kSystemMetricCount; ++i) {
    const Text *a = find_text(compact_scene, compact_ids[reversed[i]]);
    const Text *b = find_text(compact_scene, compact_ids[reversed[i + 1]]);
    CHECK(a && b && a->x < b->x,
          "compact system metrics follow configured order");
  }

  Options panel = options(Detail::Panel, Theme::Classic);
  panel.show_fan_duty = true;
  panel.show_ip_address = true;
  panel.system_order = reversed;
  const Scene panel_scene = build_scene(panel, f.r, f.h);
  for (int i = 0; i + 1 < kSystemMetricCount; ++i) {
    const Text *a = find_text(panel_scene, panel_ids[reversed[i]]);
    const Text *b = find_text(panel_scene, panel_ids[reversed[i + 1]]);
    CHECK(appears_before(a, b),
          "panel system metrics follow configured order");
  }

  std::array<int, kSystemMetricCount> malformed = {
      kSystemSocPower, kSystemSocPower, -1, kSystemGpu, kSystemGpu,
      kSystemCpuClock, 99, kSystemFan, kSystemFan};
  normalize_system_metric_order(malformed);
  std::set<int> unique(malformed.begin(), malformed.end());
  CHECK(unique.size() == kSystemMetricCount,
        "metric order normalization removes duplicates and invalid IDs");
  CHECK(malformed[0] == kSystemSocPower && malformed[1] == kSystemGpu,
        "metric order normalization preserves valid supplied order");
}

static void check_metric_visibility() {
  const Fixture f = make(State::Locked);
  Options o = options(Detail::Panel, Theme::Classic);
  o.frame_graph = false;
  o.show_system = true;
  o.show_fps = false;
  o.show_cpu = false;
  o.show_gpu = true;
  o.show_memory = false;
  o.show_ip_address = true;
  o.show_fan_duty = true;
  o.show_cpu_clock = true;
  o.show_gpu_clock = true;
  o.show_ssd_temp = true;
  o.show_soc_power = true;
  const Scene s = build_scene(o, f.r, f.h);
  CHECK(find_text(s, "id_osc_fps_v") == nullptr, "FPS hidden");
  CHECK(find_text(s, "id_osc_cpu_l") == nullptr, "CPU hidden");
  CHECK(find_text(s, "id_osc_gpu_l") != nullptr, "GPU visible");
  CHECK(find_text(s, "id_osc_mem_l") == nullptr, "memory hidden");
  CHECK(find_text(s, "id_osc_fan_l") != nullptr, "fan has its own heading");
  CHECK(find_text(s, "id_osc_fan_v") != nullptr, "fan has its own value");
  CHECK(find_text(s, "id_osc_ip_l") != nullptr, "IP has its own heading");
  CHECK(find_text(s, "id_osc_ip_v") != nullptr, "IP has its own value");
  const Text *gpu = find_text(s, "id_osc_gpu_t");
  CHECK(gpu && gpu->text == "74 W", "GPU detail contains GPU core power only");
  CHECK(find_text(s, "id_osc_cclk_v") && find_text(s, "id_osc_cclk_v")->text == "3600 MHz",
        "CPU clock is independently rendered");
  CHECK(find_text(s, "id_osc_gclk_v") && find_text(s, "id_osc_gclk_v")->text == "2200 MHz",
        "GPU GFXCLK is independently rendered");
  CHECK(find_text(s, "id_osc_ssd_v") && find_text(s, "id_osc_ssd_v")->text == "53°",
        "M.2 SSD temperature is independently rendered");
  CHECK(find_text(s, "id_osc_socpwr_v") && find_text(s, "id_osc_socpwr_v")->text == "128 W",
        "summed SoC rail power is independently rendered");
  const Text *fan = find_text(s, "id_osc_fan_v");
  CHECK(fan && fan->text == "35%", "fan value is independently readable");
  const Text *ip = find_text(s, "id_osc_ip_v");
  CHECK(ip && ip->text == "192.168.2.210", "IP address is rendered");

  o.detail = Detail::Minimal;
  o.frame_graph = false;
  const Scene minimal_hidden = build_scene(o, f.r, f.h);
  CHECK(minimal_hidden.screen_w == 1920.0f &&
            minimal_hidden.texts.empty() && minimal_hidden.rects.empty(),
        "minimal mode produces a valid empty scene when FPS is hidden");

  o.detail = Detail::Compact;
  o.show_fps = false;
  o.show_memory = true;
  const Scene compact = build_scene(o, f.r, f.h);
  CHECK(find_text(compact, "id_osc_fps_v") == nullptr,
        "compact FPS hidden");
  CHECK(find_text(compact, "id_osc_sfan_v") != nullptr,
        "compact fan has a dedicated cell");
  CHECK(find_text(compact, "id_osc_sip_v") != nullptr,
        "compact IP has a dedicated cell");
  const Text *compact_memory = find_text(compact, "id_osc_smem_v");
  CHECK(compact_memory && compact_memory->text == "9.6/4.0",
        "compact memory cell displays RAM and VRAM usage");

  o.detail = Detail::Panel;
  o.show_system = false;
  const Scene master_off = build_scene(o, f.r, f.h);
  CHECK(find_text(master_off, "id_osc_fan_l") == nullptr,
        "system master hides fan");
  CHECK(find_text(master_off, "id_osc_ip_l") == nullptr,
        "system master hides IP");
  CHECK(find_text(master_off, "id_osc_cclk_l") == nullptr &&
            find_text(master_off, "id_osc_gclk_l") == nullptr &&
            find_text(master_off, "id_osc_ssd_l") == nullptr &&
            find_text(master_off, "id_osc_socpwr_l") == nullptr,
        "system master hides added hardware metrics");
}

int main(int argc, char **argv) {
  /* Every supported theme resolves to a complete palette. */
  const Theme all_themes[] = {
      Theme::Classic, Theme::Mono, Theme::Aurora, Theme::Vice,
      Theme::Emerald, Theme::Sunset, Theme::Arctic, Theme::Crimson, Theme::Gold};
  for (const Theme theme : all_themes) {
    const Palette p = palette(theme);
    CHECK(p.text.a > 0.0f && p.heading.a > 0.0f && p.label.a > 0.0f,
          "theme has readable text colours");
    CHECK(p.good.a > 0.0f && p.warn.a > 0.0f && p.bad.a > 0.0f,
          "theme has all status colours");
  }
  check_helpers();
  g_combo = "metric visibility";
  check_metric_visibility();
  g_combo = "metric ordering";
  check_metric_order();
  const Detail details[] = {Detail::Minimal, Detail::Compact, Detail::Panel,
                            Detail::Full};
  const Theme themes[] = {Theme::Classic, Theme::Mono, Theme::Aurora,
                          Theme::Vice};
  const State states[] = {State::Locked, State::Drop, State::NoGame};
  for (Detail d : details)
    for (Theme t : themes)
      for (State st : states)
        for (BarEdge e : {BarEdge::Top, BarEdge::Bottom})
          for (BarAlign a : {BarAlign::Left, BarAlign::Center, BarAlign::Right})
            for (bool pill : {false, true})
              check_scene(d, t, st, e, a, pill);

  g_combo.clear();
  if (argc == 3 && std::string(argv[1]) == "--render" && render(argv[2]))
    ++g_fail;
  for (const auto &[msg, f] : g_failures)
    std::printf("FAIL %s  x%d, first in %s\n", msg.c_str(), f.first,
                f.second.empty() ? "-" : f.second.c_str());
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
