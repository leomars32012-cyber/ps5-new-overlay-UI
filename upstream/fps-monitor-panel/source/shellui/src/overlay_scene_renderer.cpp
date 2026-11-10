/* Copyright (C) 2025 OnionHEN — game monitor overlay widget reconciler. */

#include "overlay_scene_renderer.hpp"

#include "external_symbols.hpp"
#include "hooked_funcs.hpp"
#include "ipc.hpp"
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
#include "overlay_perf_diagnostics.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

extern MonoObject *Game;

MonoObject *CreateLabel(const char *name, float x, float y, const char *text,
                        MonoObject *fontObj, int horzAlign, int vertAlign,
                        float r, float g, float b, float a);
void Widget_Append_Child(MonoObject *widget, MonoObject *child);
MonoObject *CreateUIFont(int size, int style, int weight);
MonoObject *CreateUIColor(float r, float g, float b, float a);

namespace {

using onion::overlay::scene::Rect;
using onion::overlay::scene::Rgba;
using onion::overlay::scene::Scene;
using onion::overlay::scene::Text;
using onion::overlay::scene::TextAlign;

constexpr float kOff = onion::overlay::kOffscreen;
constexpr int kFontStyleBold = 1;
constexpr int kAlignTop = 0;
/* Text cells are at least this many × the font size tall. */
constexpr float kCellLineFactor = 1.6f;

struct Widget {
  MonoObject *obj = nullptr;   /* Panel (rect) or transparent cell (text) */
  MonoObject *label = nullptr; /* text only */
  uint32_t obj_handle = 0;
  uint32_t label_handle = 0;
  float x = kOff, y = 0.0f, w = -1.0f, h = -1.0f;
  Rgba color{-1.0f, -1.0f, -1.0f, -1.0f};
  std::string text;
  int pt = 0;     /* text only: font the label was created with */
  int weight = 0;
  uint64_t seen_generation = 0;
};

/* Pinned gchandles keep every cached widget alive and unmoved while native
 * code holds the raw pointer. */
std::unordered_map<std::string, Widget> g_widgets;
/* Scene generation replaces a full-cache seen-flag reset on every apply. */
uint64_t g_scene_generation = 0;

/* Two full-screen transparent containers fix the z-order once: anything in
 * the rect layer draws under anything in the text layer, however late it was
 * created (trend columns filling in, a bar appearing, FPS rows returning). */
struct Layer {
  MonoObject *obj = nullptr;
  uint32_t handle = 0;
};
Layer g_rect_layer;
Layer g_text_layer;
/* RootWidget the layers hang off; a different root drops the cache. */
MonoObject *g_layer_root = nullptr;

MonoClass *widget_class() {
  static MonoClass *cls =
      mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Widget");
  return cls;
}

MonoClass *panel_class() {
  static MonoClass *cls =
      mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Panel");
  return cls;
}

MonoClass *label_class() {
  static MonoClass *cls =
      mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Label");
  return cls;
}

MonoObject *find_root_widget() {
  if (!Game)
    return nullptr;
  return Get_Property<MonoObject *>(pui_img, "Sce.PlayStation.PUI.UI2", "Scene",
                                    Game, "RootWidget");
}

/* 0 = not pinned; the object must not be cached then. */
uint32_t pin(MonoObject *obj) {
  return (obj && mono_gchandle_new) ? mono_gchandle_new(obj, /*pinned=*/1) : 0;
}

void unpin(uint32_t handle) {
  if (handle && mono_gchandle_free)
    mono_gchandle_free(handle);
}

/* Rendering runs several times a second and a failed create is retried every
 * tick: log the first failure (and each new reason) once, and the recovery. */
const char *g_apply_error = nullptr;
/* State changes, odd = failing; only the first two are logged (flapping). */
int g_create_flips = 0;

bool apply_failed(const char *why) {
  if (why != g_apply_error) {
    g_apply_error = why;
    LOG_WARN("overlay: scene not drawn: %s", why);
  }
  return false;
}

Widget *create_failed(const char *what, const std::string &id) {
  if (!(g_create_flips & 1) && ++g_create_flips <= 2) {
    LOG_WARN("overlay: widget %s: %s failed; retrying each tick", id.c_str(),
             what);
  }
  return nullptr;
}

Widget *created(Widget &&w, const std::string &id) {
  if ((g_create_flips & 1) && ++g_create_flips <= 2) {
    LOG_INFO("overlay: widget creation recovered (%s)", id.c_str());
  }
  Widget &stored = g_widgets[id] = std::move(w);
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  onion::overlay::perf::record_widget_created(g_widgets.size());
#endif
  return &stored;
}

bool same_color(const Rgba &a, const Rgba &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Shared by rects and text cells: position + size of a Panel. */
void place_panel(Widget &w, float x, float y, float width, float height) {
  MonoClass *cls = panel_class();
  if (w.x != x) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(cls, w.obj, "X", x);
  }
  if (w.y != y) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(cls, w.obj, "Y", y);
  }
  if (w.w != width) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(cls, w.obj, "Width", width);
  }
  if (w.h != height) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(cls, w.obj, "Height", height);
  }
}

MonoObject *new_panel(const std::string &name, bool background) {
  MonoClass *cls = panel_class();
  if (!cls)
    return nullptr;
  MonoObject *panel = New_Object(cls);
  if (!panel)
    return nullptr;
  mono_runtime_object_init(panel);
  Set_Property(cls, panel, "Name", mono_string_new(Root_Domain, name.c_str()));
  Set_Property(cls, panel, "BackgroundVisibility", background);
  if (background) {
    Set_Property(cls, panel, "BackgroundOpacity", 1.0f);
    Set_Property(cls, panel, "BackgroundStyle", 1);
  }
  return panel;
}

Widget *create_rect(MonoObject *layer, const Rect &r) {
  MonoObject *panel = new_panel(r.id, true);
  if (!panel)
    return create_failed("Panel create", r.id);
  const uint32_t handle = pin(panel);
  if (!handle)
    return create_failed("gchandle pin", r.id);
  Widget_Append_Child(layer, panel);
  Widget w{};
  w.obj = panel;
  w.obj_handle = handle;
  return created(std::move(w), r.id);
}

Widget *create_text(MonoObject *layer, const Text &t) {
  /* Font first: a failure here must not strand a fresh cell every tick. */
  MonoObject *font = CreateUIFont(t.pt, kFontStyleBold, t.weight);
  if (!font)
    return create_failed("UIFont create", t.id);
  MonoObject *cell = new_panel(t.id + "_cell", false);
  if (!cell)
    return create_failed("cell Panel create", t.id);
  const int halign = t.align == TextAlign::Right
                         ? onion::overlay::scene::kHAlignRight
                         : onion::overlay::scene::kHAlignLeft;
  /* MarginTop stays 0 for the label's lifetime: on hardware a MarginTop
   * changed after creation does not move the text, so the vertical offset is
   * carried by the cell's Y instead (see text_cell). */
  MonoObject *label =
      CreateLabel(t.id.c_str(), 0.0f, 0.0f, t.text.c_str(), font, halign,
                  kAlignTop, t.color.r, t.color.g, t.color.b, t.color.a);
  if (!label)
    return create_failed("Label create", t.id);
  const uint32_t cell_handle = pin(cell);
  const uint32_t label_handle = pin(label);
  if (!cell_handle || !label_handle) {
    unpin(cell_handle);
    unpin(label_handle);
    return create_failed("gchandle pin", t.id);
  }
  MonoClass *lbl = label_class();
  Set_Property(lbl, label, "PositionType", 1);
  Set_Property(lbl, label, "MarginLeft", 0.0f);
  Set_Property(lbl, label, "Width", t.w);
  Widget_Append_Child(cell, label);
  Widget_Append_Child(layer, cell);

  Widget w{};
  w.obj = cell;
  w.label = label;
  w.obj_handle = cell_handle;
  w.label_handle = label_handle;
  w.text = t.text;
  w.color = t.color;
  w.pt = t.pt;
  w.weight = t.weight;
  return created(std::move(w), t.id);
}

/* Where a text's transparent cell goes: its top is the text's top (row Y +
 * margin_top) and it is tall enough that the label never overflows it, so
 * the label can sit at MarginTop 0 inside. */
struct CellBox {
  float y;
  float h;
};

CellBox text_cell(const Text &t) {
  const float top = t.y + t.margin_top;
  const float fit = std::ceil(static_cast<float>(t.pt) * kCellLineFactor);
  return {top, std::max(t.h - t.margin_top, fit)};
}

/* Detach one cached widget and release its pins. */
void destroy_widget(const std::string &id) {
  auto it = g_widgets.find(id);
  if (it == g_widgets.end())
    return;
  if (it->second.obj && widget_class())
    Invoke<void>(pui_img, widget_class(), it->second.obj, "RemoveFromParent");
  unpin(it->second.label_handle);
  unpin(it->second.obj_handle);
  g_widgets.erase(it);
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  onion::overlay::perf::set_widget_cache_size(g_widgets.size());
#endif
}

void apply_rect(MonoObject *layer, const Rect &r) {
  auto it = g_widgets.find(r.id);
  Widget *w = it != g_widgets.end() ? &it->second : create_rect(layer, r);
  if (!w)
    return;
  w->seen_generation = g_scene_generation;
  place_panel(*w, r.x, r.y, r.w, r.h);
  w->x = r.x;
  w->y = r.y;
  w->w = r.w;
  w->h = r.h;
  if (!same_color(w->color, r.color)) {
    MonoObject *c = CreateUIColor(r.color.r, r.color.g, r.color.b, r.color.a);
    if (c) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
      onion::overlay::perf::record_property_update();
#endif
      Set_Property_Invoke(panel_class(), w->obj, "BackgroundColor", c);
    }
    w->color = r.color;
  }
}

void apply_text(MonoObject *layer, const Text &t) {
  auto it = g_widgets.find(t.id);
  /* A label's font is fixed at creation (setting Font in place is untested
   * on hardware), so a size or weight change rebuilds just that label. */
  if (it != g_widgets.end() &&
      (it->second.pt != t.pt || it->second.weight != t.weight)) {
    destroy_widget(t.id);
    it = g_widgets.end();
  }
  Widget *w = it != g_widgets.end() ? &it->second : create_text(layer, t);
  if (!w)
    return;
  w->seen = true;
  MonoClass *lbl = label_class();
  if (w->w != t.w) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(lbl, w->label, "Width", t.w);
  }
  const CellBox box = text_cell(t);
  place_panel(*w, t.x, box.y, t.w, box.h);
  w->x = t.x;
  w->y = box.y;
  w->w = t.w;
  w->h = box.h;
  if (w->text != t.text) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(lbl, w->label, "Text",
                 mono_string_new(Root_Domain, t.text.c_str()));
    w->text = t.text;
  }
  if (!same_color(w->color, t.color)) {
    MonoObject *c = CreateUIColor(t.color.r, t.color.g, t.color.b, t.color.a);
    if (c) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
      onion::overlay::perf::record_property_update();
#endif
      Set_Property_Invoke(lbl, w->label, "TextColor", c);
    }
    w->color = t.color;
  }
}

bool ensure_layer(Layer &layer, MonoObject *root, const char *name,
                  const Scene &scene) {
  if (layer.obj)
    return true;
  MonoObject *panel = new_panel(name, false);
  if (!panel)
    return false;
  const uint32_t handle = pin(panel);
  if (!handle)
    return false; /* logged once by overlay_scene_apply */
  MonoClass *cls = panel_class();
  Set_Property(cls, panel, "X", 0.0f);
  Set_Property(cls, panel, "Y", 0.0f);
  Set_Property(cls, panel, "Width", scene.screen_w);
  Set_Property(cls, panel, "Height", scene.screen_h);
  Widget_Append_Child(root, panel);
  layer.obj = panel;
  layer.handle = handle;
  return true;
}

void release_layer(Layer &layer) {
  if (layer.obj && widget_class())
    Invoke<void>(pui_img, widget_class(), layer.obj, "RemoveFromParent");
  unpin(layer.handle);
  layer = Layer{};
}

} // namespace

bool overlay_scene_apply(const Scene &scene) {
  MonoObject *root = find_root_widget();
  if (!root)
    return apply_failed("Game scene has no RootWidget");
  if (!widget_class() || !panel_class() || !label_class())
    return apply_failed("UI2 Widget/Panel/Label class missing");

  /* Game scene rebuilt under us: the cached widgets hang off the old root. */
  if (g_layer_root && g_layer_root != root) {
    LOG_INFO("overlay: RootWidget changed; rebuilding widgets");
    overlay_scene_remove_all();
  }
  if (!ensure_layer(g_rect_layer, root, "id_osc_layer_rects", scene) ||
      !ensure_layer(g_text_layer, root, "id_osc_layer_texts", scene))
    return apply_failed("cannot create or pin a layer panel");
  g_layer_root = root;
  ++g_scene_generation;
  if (g_scene_generation == 0)
    ++g_scene_generation;
  if (g_apply_error) {
    g_apply_error = nullptr;
    LOG_INFO("overlay: scene drawing recovered");
  }

  for (const Rect &r : scene.rects)
    apply_rect(g_rect_layer.obj, r);
  for (const Text &t : scene.texts)
    apply_text(g_text_layer.obj, t);

  /* Anything this frame did not mention (e.g. FPS detail rows after a game
   * exits, graph columns not yet filled) parks off-screen. */
  for (auto &entry : g_widgets) {
    Widget &w = entry.second;
    if (w.seen_generation == g_scene_generation || w.x == kOff)
      continue;
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
    onion::overlay::perf::record_property_update();
#endif
    Set_Property(panel_class(), w.obj, "X", kOff);
    w.x = kOff;
  }
  return true;
}

void overlay_scene_remove_all() {
  if (g_widgets.empty() && !g_rect_layer.obj && !g_text_layer.obj)
    return;
  /* Detaching a layer takes all of its children along; they only need their
   * pins released. */
  release_layer(g_text_layer);
  release_layer(g_rect_layer);
  for (auto &entry : g_widgets) {
    unpin(entry.second.label_handle);
    unpin(entry.second.obj_handle);
  }
  LOG_DEBUG("overlay: removed %zu widgets", g_widgets.size());
  g_widgets.clear();
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  onion::overlay::perf::set_widget_cache_size(0);
#endif
  g_layer_root = nullptr;
}
