/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/focus_peaking.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/module.h"
#include "common/selection.h"
#include "common/undo.h"
#include "common/usermanual_url.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DECORATION_SIZE_LIMIT 40

static void dt_view_manager_load_modules(dt_view_manager_t *vm);
static int dt_view_load_module(void *v, const char *libname, const char *module_name);
static void dt_view_unload_module(dt_view_t *view);

void dt_view_manager_init(dt_view_manager_t *vm)
{
  /* prepare statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images "
                              "WHERE imgid = ?1", -1, &vm->statements.is_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid = ?1",
                              -1, &vm->statements.delete_from_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1,
                              &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT num FROM main.history WHERE imgid = ?1", -1,
                              &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT id FROM main.images WHERE group_id = (SELECT group_id FROM main.images WHERE id=?1) AND id != ?2",
      -1, &vm->statements.get_grouped, NULL);

  dt_view_manager_load_modules(vm);

  // Modules loaded, let's handle specific cases
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(!strcmp(view->module_name, "darkroom"))
    {
      darktable.develop = (dt_develop_t *)view->data;
      break;
    }
  }

  vm->current_view = NULL;
  vm->audio.audio_player_id = -1;
}

void dt_view_manager_gui_init(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(view->gui_init) view->gui_init(view);
  }
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter)) dt_view_unload_module((dt_view_t *)iter->data);
}

const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm)
{
  return vm->current_view;
}

// we want a stable order of views, for example for viewswitcher.
// anything not hardcoded will be put alphabetically wrt. localised names.
static gint sort_views(gconstpointer a, gconstpointer b)
{
  static const char *view_order[] = {"lighttable", "darkroom"};
  static const int n_view_order = G_N_ELEMENTS(view_order);

  dt_view_t *av = (dt_view_t *)a;
  dt_view_t *bv = (dt_view_t *)b;
  const char *aname = av->name(av);
  const char *bname = bv->name(bv);
  int apos = n_view_order;
  int bpos = n_view_order;

  for(int i = 0; i < n_view_order; i++)
  {
    if(!strcmp(av->module_name, view_order[i])) apos = i;
    if(!strcmp(bv->module_name, view_order[i])) bpos = i;
  }

  // order will be zero iff apos == bpos which can only happen when both views are not in view_order
  const int order = apos - bpos;
  return order ? order : strcmp(aname, bname);
}

static void dt_view_manager_load_modules(dt_view_manager_t *vm)
{
  vm->views = dt_module_load_modules("/views", sizeof(dt_view_t), dt_view_load_module, NULL, sort_views);
}

/* default flags for view which does not implement the flags() function */
static uint32_t default_flags()
{
  return 0;
}

/** load a view module */
static int dt_view_load_module(void *v, const char *libname, const char *module_name)
{
  dt_view_t *view = (dt_view_t *)v;

  view->data = NULL;
  view->vscroll_size = view->vscroll_viewport_size = 1.0;
  view->hscroll_size = view->hscroll_viewport_size = 1.0;
  view->vscroll_pos = view->hscroll_pos = 0.0;
  view->height = view->width = 100; // set to non-insane defaults before first expose/configure.
  g_strlcpy(view->module_name, module_name, sizeof(view->module_name));
  dt_print(DT_DEBUG_CONTROL, "[view_load_module] loading view `%s' from %s\n", module_name, libname);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    goto error;
  }
  int (*version)();
  if(!g_module_symbol(view->module, "dt_module_dt_version", (gpointer) & (version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[view_load_module] `%s' is compiled for another version of dt (module %d != dt %d) !\n",
            libname, version(), dt_version());
    goto error;
  }
  if(!g_module_symbol(view->module, "name", (gpointer) & (view->name))) view->name = NULL;
  if(!g_module_symbol(view->module, "view", (gpointer) & (view->view))) view->view = NULL;
  if(!g_module_symbol(view->module, "flags", (gpointer) & (view->flags))) view->flags = default_flags;
  if(!g_module_symbol(view->module, "init", (gpointer) & (view->init))) view->init = NULL;
  if(!g_module_symbol(view->module, "gui_init", (gpointer) & (view->gui_init))) view->gui_init = NULL;
  if(!g_module_symbol(view->module, "cleanup", (gpointer) & (view->cleanup))) view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose", (gpointer) & (view->expose))) view->expose = NULL;
  if(!g_module_symbol(view->module, "try_enter", (gpointer) & (view->try_enter))) view->try_enter = NULL;
  if(!g_module_symbol(view->module, "enter", (gpointer) & (view->enter))) view->enter = NULL;
  if(!g_module_symbol(view->module, "leave", (gpointer) & (view->leave))) view->leave = NULL;
  if(!g_module_symbol(view->module, "reset", (gpointer) & (view->reset))) view->reset = NULL;
  if(!g_module_symbol(view->module, "mouse_enter", (gpointer) & (view->mouse_enter)))
    view->mouse_enter = NULL;
  if(!g_module_symbol(view->module, "mouse_leave", (gpointer) & (view->mouse_leave)))
    view->mouse_leave = NULL;
  if(!g_module_symbol(view->module, "mouse_moved", (gpointer) & (view->mouse_moved)))
    view->mouse_moved = NULL;
  if(!g_module_symbol(view->module, "button_released", (gpointer) & (view->button_released)))
    view->button_released = NULL;
  if(!g_module_symbol(view->module, "button_pressed", (gpointer) & (view->button_pressed)))
    view->button_pressed = NULL;
  if(!g_module_symbol(view->module, "key_pressed", (gpointer) & (view->key_pressed)))
    view->key_pressed = NULL;
  if(!g_module_symbol(view->module, "key_released", (gpointer) & (view->key_released)))
    view->key_released = NULL;
  if(!g_module_symbol(view->module, "configure", (gpointer) & (view->configure))) view->configure = NULL;
  if(!g_module_symbol(view->module, "scrolled", (gpointer) & (view->scrolled))) view->scrolled = NULL;
  if(!g_module_symbol(view->module, "scrollbar_changed", (gpointer) & (view->scrollbar_changed)))
    view->scrollbar_changed = NULL;
  if(!g_module_symbol(view->module, "init_key_accels", (gpointer) & (view->init_key_accels)))
    view->init_key_accels = NULL;
  if(!g_module_symbol(view->module, "connect_key_accels", (gpointer) & (view->connect_key_accels)))
    view->connect_key_accels = NULL;
  if(!g_module_symbol(view->module, "mouse_actions", (gpointer) & (view->mouse_actions)))
    view->mouse_actions = NULL;

  view->accel_closures = NULL;

  if(!strcmp(view->module_name, "darkroom")) darktable.develop = (dt_develop_t *)view->data;

#ifdef USE_LUA
  dt_lua_register_view(darktable.lua_state.state, view);
#endif

  if(view->init) view->init(view);
  if(darktable.gui && view->init_key_accels) view->init_key_accels(view);

  return 0;

error:
  if(view->module) g_module_close(view->module);
  return 1;
}

/** unload, cleanup */
static void dt_view_unload_module(dt_view_t *view)
{
  if(view->cleanup) view->cleanup(view);

  g_slist_free(view->accel_closures);

  if(view->module) g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

/*
   When expanders get destroyed, they destroy the child
   so remove the child before that
   */
static void _remove_child(GtkWidget *child,GtkContainer *container)
{
    if(DTGTK_IS_EXPANDER(child))
    {
      GtkWidget * evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(child));
      gtk_container_remove(GTK_CONTAINER(evb),dtgtk_expander_get_body(DTGTK_EXPANDER(child)));
      gtk_widget_destroy(child);
    }
    else
    {
      gtk_container_remove(container,child);
    }
}

int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name)
{
  gboolean switching_to_none = *view_name == '\0';
  dt_view_t *new_view = NULL;

  if(!switching_to_none)
  {
    for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    {
      dt_view_t *v = (dt_view_t *)iter->data;
      if(!strcmp(v->module_name, view_name))
      {
        new_view = v;
        break;
      }
    }
    if(!new_view) return 1; // the requested view doesn't exist
  }

  return dt_view_manager_switch_by_view(vm, new_view);
}

int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *nv)
{
  dt_view_t *old_view = vm->current_view;
  dt_view_t *new_view = (dt_view_t *)nv; // views belong to us, we can de-const them :-)

  // Before switching views, restore accelerators if disabled
  if(!darktable.control->key_accelerators_on) dt_control_key_accelerators_on(darktable.control);

  // reset the cursor to the default one
  dt_control_change_cursor(GDK_LEFT_PTR);

  // also ignore what scrolling there was previously happening
  memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));

  // destroy old module list

  /*  clear the undo list, for now we do this unconditionally. At some point we will probably want to clear
     only part
      of the undo list. This should probably done with a view proxy routine returning the type of undo to
     remove. */
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);

  /* Special case when entering nothing (just before leaving dt) */
  if(!new_view)
  {
    if(old_view)
    {
      /* leave the current view*/
      if(old_view->leave) old_view->leave(old_view);

      /* iterator plugins and cleanup plugins in current view */
      for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
      {
        dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

        /* does this module belong to current view ?*/
        if(dt_lib_is_visible_in_view(plugin, old_view))
        {
          if(plugin->view_leave) plugin->view_leave(plugin, old_view, NULL);
          plugin->gui_cleanup(plugin);
          plugin->data = NULL;
          dt_accel_disconnect_list(plugin->accel_closures);
          plugin->accel_closures = NULL;
          plugin->widget = NULL;
        }
      }
    }

    /* remove all widgets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_destroy_children(darktable.gui->ui, l);
    vm->current_view = NULL;

    /* remove sticky accels window */
    if(vm->accels_window.window) dt_view_accels_hide(vm);
    return 0;
  }

  // invariant: new_view != NULL after this point
  assert(new_view != NULL);

  if(new_view->try_enter)
  {
    int error = new_view->try_enter(new_view);
    if(error) return error;
  }

  /* cleanup current view before initialization of new  */
  if(old_view)
  {
    /* leave current view */
    if(old_view->leave) old_view->leave(old_view);
    dt_accel_disconnect_list(old_view->accel_closures);
    old_view->accel_closures = NULL;

    /* iterator plugins and cleanup plugins in current view */
    for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

      /* does this module belong to current view ?*/
      if(dt_lib_is_visible_in_view(plugin, old_view))
      {
        if(plugin->view_leave) plugin->view_leave(plugin, old_view, new_view);
        dt_accel_disconnect_list(plugin->accel_closures);
        plugin->accel_closures = NULL;
      }
    }

    /* remove all widets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_foreach(darktable.gui->ui, l,(GtkCallback)_remove_child);
  }

  /* change current view to the new view */
  vm->current_view = new_view;

  /* update thumbtable accels */
  dt_thumbtable_update_accels_connection(dt_ui_thumbtable(darktable.gui->ui), new_view->view(new_view));

  /* restore visible stat of panels for the new view */
  dt_ui_restore_panels(darktable.gui->ui);

  /* lets add plugins related to new view into panels.
   * this has to be done in reverse order to have the lowest position at the bottom! */
  for(GList *iter = g_list_last(darktable.lib->plugins); iter; iter = g_list_previous(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {

      /* try get the module expander  */
      GtkWidget *w = dt_lib_gui_get_expander(plugin);

      if(plugin->connect_key_accels) plugin->connect_key_accels(plugin);
      dt_lib_connect_common_accels(plugin);

      /* if we didn't get an expander let's add the widget */
      if(!w) w = plugin->widget;

      dt_gui_add_help_link(w, dt_get_help_url(plugin->plugin_name));
      // some plugins help links depend on the view
      if(!strcmp(plugin->plugin_name,"module_toolbox")
        || !strcmp(plugin->plugin_name,"view_toolbox"))
      {
        dt_view_type_flags_t view_type = new_view->view(new_view);
        if(view_type == DT_VIEW_LIGHTTABLE)
          dt_gui_add_help_link(w,"lighttable_chapter.html#lighttable_overview");
        if(view_type == DT_VIEW_DARKROOM)
          dt_gui_add_help_link(w,"darkroom_bottom_panel.html#darkroom_bottom_panel");
      }


      /* add module to its container */
      dt_ui_container_add_widget(darktable.gui->ui, plugin->container(plugin), w);
    }
  }

  /* hide/show modules as last config */
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {
      /* set expanded if last mode was that */
      char var[1024];
      gboolean expanded = FALSE;
      gboolean visible = dt_lib_is_visible(plugin);
      if(plugin->expandable(plugin))
      {
        snprintf(var, sizeof(var), "plugins/%s/%s/expanded", new_view->module_name, plugin->plugin_name);
        expanded = dt_conf_get_bool(var);

        dt_lib_gui_set_expanded(plugin, expanded);
      }
      else
      {
        /* show/hide plugin widget depending on expanded flag or if plugin
            not is expandeable() */
        if(visible)
          gtk_widget_show_all(plugin->widget);
        else
          gtk_widget_hide(plugin->widget);
      }
      if(plugin->view_enter) plugin->view_enter(plugin, old_view, new_view);
    }
  }

  /* enter view. crucially, do this before initing the plugins below,
      as e.g. modulegroups requires the dr stuff to be inited. */
  if(new_view->enter) new_view->enter(new_view);
  if(new_view->connect_key_accels) new_view->connect_key_accels(new_view);

  /* update the scrollbars */
  dt_ui_update_scrollbars(darktable.gui->ui);

  /* update sticky accels window */
  if(vm->accels_window.window && vm->accels_window.sticky) dt_view_accels_refresh(vm);

  /* raise view changed signal */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, old_view, new_view);

  // update log visibility
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW);

  return 0;
}

const char *dt_view_manager_name(dt_view_manager_t *vm)
{
  if(!vm->current_view) return "";
  if(vm->current_view->name)
    return vm->current_view->name(vm->current_view);
  else
    return vm->current_view->module_name;
}

void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery)
{
  if(!vm->current_view)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_BG);
    cairo_paint(cr);
    return;
  }
  vm->current_view->width = width;
  vm->current_view->height = height;

  if(vm->current_view->expose)
  {
    /* expose the view */
    cairo_rectangle(cr, 0, 0, vm->current_view->width, vm->current_view->height);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_save(cr);
    float px = pointerx, py = pointery;
    if(pointery > vm->current_view->height)
    {
      px = 10000.0;
      py = -1.0;
    }
    vm->current_view->expose(vm->current_view, cr, vm->current_view->width, vm->current_view->height, px, py);

    cairo_restore(cr);
    /* expose plugins */
    GList *plugins = g_list_last(darktable.lib->plugins);
    while(plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      /* does this module belong to current view ?*/
      if(plugin->gui_post_expose && dt_lib_is_visible_in_view(plugin, vm->current_view))
        plugin->gui_post_expose(plugin, cr, vm->current_view->width, vm->current_view->height, px, py);

      /* get next plugin */
      plugins = g_list_previous(plugins);
    }
  }
}

void dt_view_manager_reset(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  if(vm->current_view->reset) vm->current_view->reset(vm->current_view);
}

void dt_view_manager_mouse_leave(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_leave && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_leave(plugin)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_leave) v->mouse_leave(v);
}

void dt_view_manager_mouse_enter(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  if(vm->current_view->mouse_enter) vm->current_view->mouse_enter(vm->current_view);
}

void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_moved && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_moved(plugin, x, y, pressure, which)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_moved) v->mouse_moved(v, x, y, pressure, which);
}

int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(!vm->current_view) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_released && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_released(plugin, x, y, which, state)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  if(handled) return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_released)
    v->button_released(v, x, y, which, state);

  return 0;
}

int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state)
{
  if(!vm->current_view) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins && !handled)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_pressed(plugin, x, y, pressure, which, type, state)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  if(handled) return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_pressed)
    return v->button_pressed(v, x, y, pressure, which, type, state);

  return 0;
}

int dt_view_manager_key_pressed(dt_view_manager_t *vm, guint key, guint state)
{
  if(!vm->current_view) return 0;
  if(vm->current_view->key_pressed) return vm->current_view->key_pressed(vm->current_view, key, state);
  return 0;
}

int dt_view_manager_key_released(dt_view_manager_t *vm, guint key, guint state)
{
  if(!vm->current_view) return 0;
  if(vm->current_view->key_released) return vm->current_view->key_released(vm->current_view, key, state);
  return 0;
}

void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    // this is necessary for all
    dt_view_t *v = (dt_view_t *)iter->data;
    v->width = width;
    v->height = height;
    if(v->configure) v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state)
{
  if(!vm->current_view) return;
  if(vm->current_view->scrolled) vm->current_view->scrolled(vm->current_view, x, y, up, state);
}

void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm, double x, double y)
{
  if(!vm->current_view) return;
  if(vm->current_view->scrollbar_changed) vm->current_view->scrollbar_changed(vm->current_view, x, y);
}

void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hlower, float hsize, float hwinsize, float vpos,
                           float vlower, float vsize, float vwinsize)
{
  if (view->vscroll_pos == vpos && view->vscroll_lower == vlower && view->vscroll_size == vsize &&
      view->vscroll_viewport_size == vwinsize && view->hscroll_pos == hpos && view->hscroll_lower == hlower &&
      view->hscroll_size == hsize && view->hscroll_viewport_size == hwinsize)
    return;

  view->vscroll_pos = vpos;
  view->vscroll_lower = vlower;
  view->vscroll_size = vsize;
  view->vscroll_viewport_size = vwinsize;
  view->hscroll_pos = hpos;
  view->hscroll_lower = hlower;
  view->hscroll_size = hsize;
  view->hscroll_viewport_size = hwinsize;

  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.right_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.bottom_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.top_border;
  gtk_widget_queue_draw(widget);

  if (!darktable.gui->scrollbars.dragging) dt_ui_update_scrollbars(darktable.gui->ui);

}

static inline void dt_view_draw_altered(cairo_t *cr, const float x, const float y, const float r)
{
  cairo_new_sub_path(cr);
  cairo_arc(cr, x, y, r, 0, 2.0f * M_PI);
  const float dx = r * cosf(M_PI / 8.0f), dy = r * sinf(M_PI / 8.0f);
  cairo_move_to(cr, x - dx, y - dy);
  cairo_curve_to(cr, x, y - 2 * dy, x, y + 2 * dy, x + dx, y + dy);
  cairo_move_to(cr, x - .20 * dx, y + .8 * dy);
  cairo_line_to(cr, x - .80 * dx, y + .8 * dy);
  cairo_move_to(cr, x + .20 * dx, y - .8 * dy);
  cairo_line_to(cr, x + .80 * dx, y - .8 * dy);
  cairo_move_to(cr, x + .50 * dx, y - .8 * dy - 0.3 * dx);
  cairo_line_to(cr, x + .50 * dx, y - .8 * dy + 0.3 * dx);
  cairo_stroke(cr);
}

static inline void dt_view_draw_audio(cairo_t *cr, const float x, const float y, const float r)
{
  const float d = 2.0 * r;

  cairo_save(cr);

  cairo_translate(cr, x - (d / 2.0), y - (d / 2.0));
  cairo_scale(cr, d, d);

  cairo_rectangle(cr, 0.05, 0.4, 0.2, 0.2);
  cairo_move_to(cr, 0.25, 0.6);
  cairo_line_to(cr, 0.45, 0.77);
  cairo_line_to(cr, 0.45, 0.23);
  cairo_line_to(cr, 0.25, 0.4);

  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.45, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.6, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.75, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);

  cairo_restore(cr);
  cairo_stroke(cr);
}

static int _images_to_act_on_find_custom(gconstpointer a, gconstpointer b)
{
  return (GPOINTER_TO_INT(a) != GPOINTER_TO_INT(b));
}
static void _images_to_act_on_insert_in_list(GList **list, const int imgid, gboolean only_visible)
{
  if(only_visible)
  {
    if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _images_to_act_on_find_custom))
      *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    return;
  }

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    const int img_group_id = image->group_id;
    dt_image_cache_read_release(darktable.image_cache, image);

    if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id
       || !dt_selection_get_collection(darktable.selection))
    {
      if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _images_to_act_on_find_custom))
        *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    }
    else
    {
      sqlite3_stmt *stmt;
      gchar *query = dt_util_dstrcat(
          NULL,
          "SELECT id "
          "FROM main.images "
          "WHERE group_id = %d AND id IN (%s)",
          img_group_id, dt_collection_get_query_no_group(dt_selection_get_collection(darktable.selection)));
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        if(!g_list_find_custom(*list, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)), _images_to_act_on_find_custom))
          *list = g_list_append(*list, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      }
      sqlite3_finalize(stmt);
      g_free(query);
    }
  }
}

// get the list of images to act on during global changes (libs, accels)
GList *dt_view_get_images_to_act_on(gboolean only_visible)
{
  /** Here's how it works
   *
   *             mouse over| x | x | x |   |   |
   *     mouse inside table| x | x |   |   |   |
   * mouse inside selection| x |   |   |   |   |
   *          active images| ? | ? | x |   | x |
   *                       |   |   |   |   |   |
   *                       | S | O | O | S | A |
   *  S = selection ; O = mouseover ; A = active images
   *  the mouse can be outside thumbtable in case of filmstrip + mouse in center widget
   *
   *  if only_visible is FALSE, then it will add also not visible images because of grouping
   **/

  GList *l = NULL;
  const int mouseover = dt_control_get_mouse_over_id();

  if(mouseover > 0)
  {
    // collumn 1,2,3
    if(dt_ui_thumbtable(darktable.gui->ui)->mouse_inside)
    {
      // collumn 1,2
      sqlite3_stmt *stmt;
      gboolean inside_sel = FALSE;
      gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM main.selected_images WHERE imgid =%d", mouseover);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        inside_sel = TRUE;
        sqlite3_finalize(stmt);
      }
      g_free(query);

      if(inside_sel)
      {
        // collumn 1
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                    "WHERE m.imgid=s.imgid "
                                    "ORDER BY m.rowid",
                                    -1, &stmt, NULL);
        while(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
        {
          _images_to_act_on_insert_in_list(&l, sqlite3_column_int(stmt, 0), only_visible);
        }
        if(stmt) sqlite3_finalize(stmt);
      }
      else
      {
        // collumn 2
        _images_to_act_on_insert_in_list(&l, mouseover, only_visible);
      }
    }
    else
    {
      // collumn 3
      _images_to_act_on_insert_in_list(&l, mouseover, only_visible);
    }
  }
  else
  {
    // collumn 4,5
    if(g_slist_length(darktable.view_manager->active_images) > 0)
    {
      // collumn 5
      GSList *ll = darktable.view_manager->active_images;
      while(ll)
      {
        const int id = GPOINTER_TO_INT(ll->data);
        _images_to_act_on_insert_in_list(&l, id, only_visible);
        ll = g_slist_next(ll);
      }
    }
    else
    {
      // collumn 4
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                  "WHERE m.imgid=s.imgid "
                                  "ORDER BY m.rowid",
                                  -1, &stmt, NULL);
      while(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        _images_to_act_on_insert_in_list(&l, sqlite3_column_int(stmt, 0), only_visible);
      }
      if(stmt) sqlite3_finalize(stmt);
    }
  }

  return l;
}

// get the main image to act on during global changes (libs, accels)
int dt_view_get_image_to_act_on()
{
  /** Here's how it works -- same as for list, except we don't care about mouse inside selection or table
   *
   *             mouse over| x |   |   |
   *          active images| ? |   | x |
   *                       |   |   |   |
   *                       | O | S | A |
   *  First image of ...
   *  S = selection ; O = mouseover ; A = active images
   **/

  int ret = -1;
  const int mouseover = dt_control_get_mouse_over_id();

  if(mouseover > 0)
  {
    ret = mouseover;
  }
  else
  {
    if(g_slist_length(darktable.view_manager->active_images) > 0)
    {
      ret = GPOINTER_TO_INT(g_slist_nth_data(darktable.view_manager->active_images, 0));
    }
    else
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT s.imgid FROM main.selected_images as s, memory.collected_images as c "
                                  "WHERE s.imgid=c.imgid ORDER BY c.rowid LIMIT 1",
                                  -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        ret = sqlite3_column_int(stmt, 0);
      }
      if(stmt) sqlite3_finalize(stmt);
    }
  }

  return ret;
}

// Draw one of the controls that overlay thumbnails (e.g. stars) and check if the pointer is hovering it.
// cr == NULL --> only check for pointer hovering
// active --> non zero if the control can be activated by the mouse hovering it
// return value non zero --> mouse is hovering

int dt_view_process_image_over(dt_view_image_over_t what, int active, cairo_t *cr, const dt_image_t *img,
                               int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py,
                               dt_gui_color_t outlinecol, dt_gui_color_t fontcol)
{
  int ret = 0; // return value

  // we need to squeeze 5 stars + 2 symbols on a thumbnail width
  // each of them having a width of 2 * r1 and spaced by r1
  // that's 14 * r1 of content + 6 * r1 of spacing
  // inner margins are 0.045 * width
  const float r1 = fminf(DT_PIXEL_APPLY_DPI(20.0f) / 2.0f, 0.91 * width / 20.0f);
  const float r2 = r1 / 2.5f;

  if(cr)
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  }

  gboolean extended_thumb_overlay = dt_conf_get_bool("plugins/lighttable/extended_thumb_overlay");
  float x, y;
  if(zoom != 1)
    y = (extended_thumb_overlay ? 0.93 * height : 0.955 * height - r1);
  else
    y = 9.0f * r1;

  const int rejected = img && (img->flags & DT_IMAGE_REJECTED) == DT_IMAGE_REJECTED;

  // Search which star is hovered by cursor
  int star = -1;

  if(active)
  {
    for(int i = DT_VIEW_STAR_1; i < DT_VIEW_STAR_5 + 1; ++i)
    {
      if(zoom != 1)
        x = 0.5f * width - 5.0f * r1 + (i - DT_VIEW_STAR_1) * 2.5f * r1;
      else
        x = 3.0f * r1 + (what - DT_VIEW_STAR_1 + 1.5f) * 2.5f * r1;

      if((px - x) * (px - x) + (py - y) * (py - y) < r1 * r1) star = i;
    }
  }

  switch(what)
  {
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
      if(zoom != 1)
        x = 0.5f * width - 5.0f * r1 + (what - DT_VIEW_STAR_1) * 2.5f * r1;
      else
        x = 3.0f * r1 + (what - DT_VIEW_STAR_1 + 1.5f) * 2.5f * r1;

      if(cr) dt_draw_star(cr, x, y, r1, r2);

      if(active && star > what - DT_VIEW_STAR_1)
      {
        // hovering display
        ret = 1;
        if(cr)
        {
          cairo_fill_preserve(cr);
          dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER);
          cairo_stroke(cr);
          dt_gui_gtk_set_source_rgb(cr, outlinecol);
        }
      }
      else if(cr && img && (img->flags & 0x7) > what - DT_VIEW_STAR_1 && ((star > what - DT_VIEW_STAR_1) || star == -1))
      {
        // static display with stars set
        cairo_fill_preserve(cr);
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER);
        cairo_stroke(cr);
        dt_gui_gtk_set_source_rgb(cr, outlinecol);
      }
      else if(cr)
        // empty static display
        cairo_stroke(cr);

      break;

    case DT_VIEW_REJECT:
      if(zoom != 1)
        x = 0.045f * width + r1;
      else
        x = 3.0f * r1;

      if(cr && rejected) cairo_set_source_rgb(cr, 1., 0., 0.);

      if(active && (px - x) * (px - x) + (py - y) * (py - y) < r1 * r1)
      {
        ret = 1;
        if(cr)
        {
          cairo_new_sub_path(cr);
          cairo_arc(cr, x, y, r1, 0, 2.0f * M_PI);
          cairo_stroke(cr);
        }
      }

      if(cr)
      {
        if(rejected) cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));

        const float r3 = (r1 / sqrtf(2.0f)) * 0.95f;

        // reject cross:
        cairo_move_to(cr, x - r3, y - r3);
        cairo_line_to(cr, x + r3, y + r3);
        cairo_move_to(cr, x + r3, y - r3);
        cairo_line_to(cr, x - r3, y + r3);
        cairo_close_path(cr);
        cairo_stroke(cr);
        dt_gui_gtk_set_source_rgb(cr, outlinecol);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
      }

      break;

    case DT_VIEW_GROUP:
    {
      // draw grouping icon and border if the current group is expanded
      // align to the right, left of altered
      if(zoom != 1)
      {
        x = width * 0.955 - r1 * 4.5;
        y = height * 0.045;
      }
      else
      {
        x = (3.0 + 2.0 + 1.0 + 5 * 2.5 + 2.0 + 2.0) * r1;
        y = y - r1;
      }
      if(cr)
      {
        cairo_save(cr);
        if(img && (img->id != img->group_id)) dt_gui_gtk_set_source_rgb(cr, fontcol);
        dtgtk_cairo_paint_grouping(cr, x, y, 2 * r1, 2 * r1, 23, NULL);
        cairo_restore(cr);
      }

      // mouse is over the audio icon
      if(active && fabs(px - x - r1) <= .9 * r1 && fabs(py - y - r1) <= .9 * r1) ret = 1;

      break;
    }

    case DT_VIEW_AUDIO:
    {
      // align to right
      if(zoom != 1)
      {
        x = width * 0.955 - r1 * 6;
        y = height * 0.045 + r1;
      }
      else
        x = (3.0 + 2.0 + 1.0 + 5 * 2.5 + 2.0 + 6.0) * r1;
      if(cr)
      {
        dt_gui_gtk_set_source_rgb(cr, fontcol);
        dt_view_draw_audio(cr, x, y, r1);
      }

      // mouse is over the audio icon
      if(active && fabsf(px - x) <= 1.2 * r1 && fabsf(py - y) <= 1.2 * r1) ret = 1;

      break;
    }

    case DT_VIEW_ALTERED:
    {
      // align to right
      if(zoom != 1)
      {
        x = width * 0.955 - r1;
        y = height * 0.045 + r1;
      }
      else
        x = (3.0 + 2.0 + 1.0 + 5 * 2.5 + 2.0) * r1;
      if(cr)
      {
        dt_gui_gtk_set_source_rgb(cr, fontcol);
        dt_view_draw_altered(cr, x, y, r1);
      }
      if(active && fabsf(px - x) <= 1.2 * r1 && fabsf(py - y) <= 1.2 * r1) ret = 1;

      break;
    }

    default: // if what == DT_VIEW_DESERT just return 0
      return 0;
  }

  return ret;
}

dt_view_image_over_t dt_view_guess_image_over(int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py)
{
  // active if zoom>1 or in the proper area
  const gboolean in_metadata_zone = (px < width && py < height / 2) || (zoom > 1);
  const gboolean draw_metadata = darktable.gui->show_overlays || in_metadata_zone;

  if(draw_metadata && width > DECORATION_SIZE_LIMIT)
  {
    for(dt_view_image_over_t i = DT_VIEW_ERR; i < DT_VIEW_END; i++)
      if(dt_view_process_image_over(i, 1, NULL, NULL, width, height, zoom, px, py, 0, 0)) return i;
  }

  return DT_VIEW_DESERT;
}

int dt_view_image_get_surface(int imgid, int width, int height, cairo_surface_t **surface)
{
  // if surface not null, clean it up
  if(*surface && cairo_surface_get_reference_count(*surface) > 0) cairo_surface_destroy(*surface);
  *surface = NULL;

  // get mipmap cahe image
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(cache, width, height);

  // if needed, we load the mimap buffer
  dt_mipmap_buffer_t buf;
  gboolean buf_ok = TRUE;
  int buf_wd = 0;
  int buf_ht = 0;

  dt_mipmap_cache_get(cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');
  buf_wd = buf.width;
  buf_ht = buf.height;
  if(!buf.buf)
    buf_ok = FALSE;
  else if(mip != buf.size)
    buf_ok = FALSE;

  // if we got a different mip than requested, and it's not a skull (8x8 px), we count
  // this thumbnail as missing (to trigger re-exposure)
  if(!buf_ok && buf_wd != 8 && buf_ht != 8)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return 1;
  }

  // so we create a new image surface to return
  const float scale = fminf(width / (float)buf_wd, height / (float)buf_ht);
  const int img_width = buf_wd * scale;
  const int img_height = buf_ht * scale;
  *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, img_width, img_height);

  // we transfer cahed image on a cairo_surface (with colorspace transform if needed)
  cairo_surface_t *tmp_surface = NULL;
  uint8_t *rgbbuf = (uint8_t *)calloc(buf_wd * buf_ht * 4, sizeof(uint8_t));
  if(rgbbuf)
  {
    gboolean have_lock = FALSE;
    cmsHTRANSFORM transform = NULL;

    if(dt_conf_get_bool("cache_color_managed"))
    {
      pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
      have_lock = TRUE;

      // we only color manage when a thumbnail is sRGB or AdobeRGB. everything else just gets dumped to the
      // screen
      if(buf.color_space == DT_COLORSPACE_SRGB && darktable.color_profiles->transform_srgb_to_display)
      {
        transform = darktable.color_profiles->transform_srgb_to_display;
      }
      else if(buf.color_space == DT_COLORSPACE_ADOBERGB && darktable.color_profiles->transform_adobe_rgb_to_display)
      {
        transform = darktable.color_profiles->transform_adobe_rgb_to_display;
      }
      else
      {
        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
        have_lock = FALSE;
        if(buf.color_space == DT_COLORSPACE_NONE)
        {
          fprintf(stderr, "oops, there seems to be a code path not setting the color space of thumbnails!\n");
        }
        else if(buf.color_space != DT_COLORSPACE_DISPLAY && buf.color_space != DT_COLORSPACE_DISPLAY2)
        {
          fprintf(stderr,
                  "oops, there seems to be a code path setting an unhandled color space of thumbnails (%s)!\n",
                  dt_colorspaces_get_name(buf.color_space, "from file"));
        }
      }
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(buf, rgbbuf, transform)
#endif
    for(int i = 0; i < buf.height; i++)
    {
      const uint8_t *in = buf.buf + i * buf.width * 4;
      uint8_t *out = rgbbuf + i * buf.width * 4;

      if(transform)
      {
        cmsDoTransform(transform, in, out, buf.width);
      }
      else
      {
        for(int j = 0; j < buf.width; j++, in += 4, out += 4)
        {
          out[0] = in[2];
          out[1] = in[1];
          out[2] = in[0];
        }
      }
    }
    if(have_lock) pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

    const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_wd);
    tmp_surface = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_wd, buf_ht, stride);
  }

  // draw the image scaled:
  if(tmp_surface)
  {
    cairo_t *cr = cairo_create(*surface);
    cairo_scale(cr, scale, scale);

    cairo_set_source_surface(cr, tmp_surface, 0, 0);
    // set filter no nearest:
    // in skull mode, we want to see big pixels.
    // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
    // in between, filtering just makes stuff go unsharp.
    if((buf_wd <= 8 && buf_ht <= 8) || fabsf(scale - 1.0f) < 0.01f)
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    else
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);

    cairo_paint(cr);

    if(darktable.gui->show_focus_peaking)
      dt_focuspeaking(cr, img_width, img_height, cairo_image_surface_get_data(*surface),
                      cairo_image_surface_get_width(*surface), cairo_image_surface_get_height(*surface));

    cairo_surface_destroy(tmp_surface);
    cairo_destroy(cr);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  if(rgbbuf) free(rgbbuf);
  return 0;
}

char* dt_view_extend_modes_str(const char * name, const gboolean is_hdr, const gboolean is_bw)
{
  char* upcase = g_ascii_strup(name, -1);  // extension in capital letters to avoid character descenders

  if(is_hdr)
  {
    gchar* fullname = g_strdup_printf("%s HDR",upcase);
    g_free(upcase);
    upcase = fullname;
  }
  if(is_bw)
  {
    gchar* fullname = g_strdup_printf("%s B&W",upcase);
    g_free(upcase);
    upcase = fullname;
  }
  return upcase;
}

int dt_view_image_expose(dt_view_image_expose_t *vals)
{
  int missing = 0;
  const double start = dt_get_wtime();
// some performance tuning stuff, for your pleasure.
// on my machine with 7 image per row it seems grouping has the largest
// impact from around 400ms -> 55ms per redraw.

  dt_view_image_over_t *image_over = vals->image_over;
  const uint32_t imgid = vals->imgid;
  cairo_t *cr = vals->cr;
  const float width = vals->width;
  const float height = vals->height;
  const int32_t zoom = vals->zoom;
  const int32_t px = vals->px;
  const int32_t py = vals->py;
  const gboolean full_preview = vals->full_preview;
  const gboolean image_only = vals->image_only;
  const gboolean no_deco = image_only ? TRUE : vals->no_deco;
  const float full_zoom = vals->full_zoom;
  const float full_x = vals->full_x;
  const float full_y = vals->full_y;

  // active if zoom>1 or in the proper area
  const gboolean in_metadata_zone = (px < width && py < height / 2) || (zoom > 1);

  const gboolean draw_thumb = TRUE;
  const gboolean draw_colorlabels = !no_deco && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_local_copy = !no_deco && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_grouping = !no_deco;
  const gboolean draw_selected = !no_deco;
  const gboolean draw_history = !no_deco;
  const gboolean draw_metadata = !no_deco && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_audio = !no_deco;

  cairo_save(cr);
  dt_gui_color_t bgcol = DT_GUI_COLOR_THUMBNAIL_BG;
  dt_gui_color_t fontcol = DT_GUI_COLOR_THUMBNAIL_FONT;
  dt_gui_color_t outlinecol = DT_GUI_COLOR_THUMBNAIL_OUTLINE;

  int selected = 0, is_grouped = 0;

  if (draw_selected)
  {
    /* clear and reset statements */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
    /* bind imgid to prepared statements */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
    /* lets check if imgid is selected */
    if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW) selected = 1;
  }

  // do we need to surround the image ?
  gboolean surrounded = FALSE;
  const dt_view_t *cur_view = dt_view_manager_get_current_view(darktable.view_manager);
  if(!full_preview && cur_view->view(cur_view) == DT_VIEW_DARKROOM)
  {
    // in darkroom, surrounded image is the one shown in main view
    surrounded = (darktable.develop->image_storage.id == imgid);
  }

  dt_image_t buffered_image;
  const dt_image_t *img;
  // if darktable.gui->show_overlays is set or the user points at this image, we really want it:
  if(darktable.gui->show_overlays || vals->mouse_over || zoom == 1)
    img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  else
    img = dt_image_cache_testget(darktable.image_cache, imgid, 'r');

  if(selected == 1 && zoom != 1) // If zoom == 1 there is no need to set colors here
  {
    outlinecol = DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE;
    bgcol = DT_GUI_COLOR_THUMBNAIL_SELECTED_BG;
    fontcol = DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT;
  }
  if(vals->mouse_over || zoom == 1)
  {
    // mouse over
    bgcol = DT_GUI_COLOR_THUMBNAIL_HOVER_BG;
    fontcol = DT_GUI_COLOR_THUMBNAIL_HOVER_FONT;
    outlinecol = DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE;
  }
  // release image cache lock as early as possible, to avoid deadlocks (mipmap cache might need to lock it, too)
  if(img)
  {
    buffered_image = *img;
    dt_image_cache_read_release(darktable.image_cache, img);
    img = &buffered_image;
  }

  gboolean draw_thumb_background = FALSE;
  float imgwd = 0.91f;
  if (image_only)
  {
    imgwd = 1.0;
  }
  else if(zoom == 1)
  {
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    draw_thumb_background = TRUE;
  }

  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  float fz = 1.0f;
  if(full_zoom > 0.0f) fz = full_zoom;
  if(vals->full_zoom100 > 0.0f) fz = fminf(vals->full_zoom100, fz);
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(cache, imgwd * width * fz, imgwd * height * fz);

  // if needed, we load the mimap buffer
  dt_mipmap_buffer_t buf;
  gboolean buf_sizeok = TRUE;
  gboolean buf_ok = TRUE;
  gboolean buf_mipmap = FALSE;
  int buf_wd = 0;
  int buf_ht = 0;
  if(vals->full_surface && *(vals->full_surface) && !*(vals->full_surface_w_lock)
     && (*(vals->full_surface_id) != imgid || *(vals->full_surface_mip) != mip || !full_preview))
  {
    cairo_surface_destroy(*(vals->full_surface));
    if(vals->full_rgbbuf && *(vals->full_rgbbuf))
    {
      free(*(vals->full_rgbbuf));
      *(vals->full_rgbbuf) = NULL;
    }
    *(vals->full_surface) = NULL;
  }
  if(!vals->full_surface || !*(vals->full_surface) || *(vals->full_surface_w_lock))
  {
    dt_mipmap_cache_get(cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');
    buf_wd = buf.width;
    buf_ht = buf.height;
    if(!buf.buf)
    {
      buf_ok = FALSE;
      buf_sizeok = FALSE;
    }
    if(mip != buf.size) buf_sizeok = FALSE;
    buf_mipmap = TRUE;
  }
  else
  {
    buf_wd = *(vals->full_surface_wd);
    buf_ht = *(vals->full_surface_ht);
  }

  if(draw_thumb_background)
  {
    cairo_rectangle(cr, 0, 0, width, height);
    dt_gui_gtk_set_source_rgb(cr, bgcol);
    cairo_fill_preserve(cr);
    if(vals->filmstrip)
    {
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
      if(surrounded)
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER);
      else
        dt_gui_gtk_set_source_rgb(cr, outlinecol);
      cairo_stroke(cr);
    }

    if(img)
    {
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      const int fontsize = fminf(DT_PIXEL_APPLY_DPI(20.0), .09 * width);
      pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);
      const char *ext = img->filename + strlen(img->filename);
      while(ext > img->filename && *ext != '.') ext--;
      ext++;
      dt_gui_gtk_set_source_rgb(cr, fontcol);

      char* upcase_ext = dt_view_extend_modes_str(ext, dt_image_is_hdr(img), dt_image_is_monochrome(img));

      if(buf_ht > buf_wd)
      {
        int max_chr_width = 0;
        for (int i = 0; upcase_ext[i] != 0; i++)
        {
          pango_layout_set_text(layout, &upcase_ext[i], 1);
          pango_layout_get_pixel_extents(layout, &ink, NULL);
          max_chr_width = MAX(max_chr_width, ink.width);
        }

        for (int i = 0, yoffs = fontsize;  upcase_ext[i] != 0; i++,  yoffs -= fontsize)
        {
          pango_layout_set_text(layout, &upcase_ext[i], 1);
          pango_layout_get_pixel_extents(layout, &ink, NULL);
          cairo_move_to(cr, .045 * width - ink.x + (max_chr_width - ink.width) / 2, .045 * height - yoffs + fontsize);
          pango_cairo_show_layout(cr, layout);
        }
      }
      else
      {
        pango_layout_set_text(layout, upcase_ext, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, .045 * width - ink.x, .045 * height);
        pango_cairo_show_layout(cr, layout);
      }
      g_free(upcase_ext);
      pango_font_description_free(desc);
      g_object_unref(layout);

    }
  }

  // if we got a different mip than requested, and it's not a skull (8x8 px), we count
  // this thumbnail as missing (to trigger re-exposure)
  if(!buf_sizeok && buf_wd != 8 && buf_ht != 8) missing = 1;

  if (draw_thumb)
  {
    float scale = 1.0;
    cairo_surface_t *surface = NULL;
    uint8_t *rgbbuf = NULL;

    if(vals->full_surface && *(vals->full_surface) && !*(vals->full_surface_w_lock))
    {
      surface = *(vals->full_surface);
      rgbbuf = *(vals->full_rgbbuf);
    }
    else
    {
      if(buf_ok)
      {
        rgbbuf = (uint8_t *)calloc(buf_wd * buf_ht * 4, sizeof(uint8_t));
        if(rgbbuf)
        {
          gboolean have_lock = FALSE;
          cmsHTRANSFORM transform = NULL;

          if(dt_conf_get_bool("cache_color_managed"))
          {
            pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
            have_lock = TRUE;

            // we only color manage when a thumbnail is sRGB or AdobeRGB. everything else just gets dumped to the
            // screen
            if(buf.color_space == DT_COLORSPACE_SRGB && darktable.color_profiles->transform_srgb_to_display)
            {
              transform = darktable.color_profiles->transform_srgb_to_display;
            }
            else if(buf.color_space == DT_COLORSPACE_ADOBERGB
                    && darktable.color_profiles->transform_adobe_rgb_to_display)
            {
              transform = darktable.color_profiles->transform_adobe_rgb_to_display;
            }
            else
            {
              pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
              have_lock = FALSE;
              if(buf.color_space == DT_COLORSPACE_NONE)
              {
                fprintf(stderr,
                        "oops, there seems to be a code path not setting the color space of thumbnails!\n");
              }
              else if(buf.color_space != DT_COLORSPACE_DISPLAY && buf.color_space != DT_COLORSPACE_DISPLAY2)
              {
                fprintf(
                    stderr,
                    "oops, there seems to be a code path setting an unhandled color space of thumbnails (%s)!\n",
                    dt_colorspaces_get_name(buf.color_space, "from file"));
              }
            }
          }

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(buf, rgbbuf, transform)
#endif
          for(int i = 0; i < buf.height; i++)
          {
            const uint8_t *in = buf.buf + i * buf.width * 4;
            uint8_t *out = rgbbuf + i * buf.width * 4;

            if(transform)
            {
              cmsDoTransform(transform, in, out, buf.width);
            }
            else
            {
              for(int j = 0; j < buf.width; j++, in += 4, out += 4)
              {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
              }
            }
          }
          if(have_lock) pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

          const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_wd);
          surface = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_wd, buf_ht, stride);

          // we save the surface for later use
          if(!missing && vals->full_surface && !*(vals->full_surface_w_lock))
          {
            *(vals->full_surface_w_lock) = 1;
            *(vals->full_surface) = surface;
            *(vals->full_rgbbuf) = rgbbuf;
            *(vals->full_surface_ht) = buf_ht;
            *(vals->full_surface_wd) = buf_wd;
            *(vals->full_surface_mip) = mip;
            *(vals->full_surface_id) = imgid;
            *(vals->full_surface_w_lock) = 0;
          }
        }
      }
    }

    if(surface)
    {
      if(zoom == 1 && !image_only)
      {
        const int32_t tb = darktable.develop->border_size;
        scale = fminf((width - 2 * tb) / (float)buf_wd, (height - 2 * tb) / (float)buf_ht) * fz;
      }
      else
        scale = fminf(width * imgwd / (float)buf_wd, height * imgwd / (float)buf_ht) * fz;
    }
    // draw centered and fitted:
    cairo_save(cr);

    if (image_only) // in this case we want to display the picture exactly at (px, py)
      cairo_translate(cr, px, py);
    else
      cairo_translate(cr, width / 2.0, height / 2.0);

    cairo_scale(cr, scale, scale);

    float rectw = width;
    float recth = height;
    float rectx = 0.0f;
    float recty = 0.0f;
    if(buf_ok)
    {
      rectw = buf_wd;
      recth = buf_ht;
    }

    if(surface)
    {
      // we move the full preview
      float fx = 0.0f;
      float fy = 0.0f;
      if(fz > 1.0f)
      {
        int w = width;
        int h = height;
        if(zoom == 1 && !image_only)
        {
          const int32_t tb = darktable.develop->border_size;
          w -= 2 * tb;
          h -= 2 * tb;
        }
        // we want to be sure the image stay in the window
        if(buf_sizeok && vals->full_maxdx && vals->full_maxdy)
        {
          *(vals->full_maxdx) = fmaxf(0.0f, (buf_wd * scale - w) / 2);
          *(vals->full_maxdy) = fmaxf(0.0f, (buf_ht * scale - h) / 2);
        }
        fx = fminf((buf_wd * scale - w) / 2, fabsf(full_x));
        if(full_x < 0) fx = -fx;
        if(buf_wd * scale <= w) fx = 0;
        fy = fminf((buf_ht * scale - h) / 2, fabsf(full_y));
        if(full_y < 0) fy = -fy;
        if(buf_ht * scale <= h) fy = 0;

        // and we determine the rectangle where the image is display
        rectw = fminf(w / scale, rectw);
        recth = fminf(h / scale, recth);
        rectx = 0.5 * buf_wd - fx / scale - 0.5 * rectw;
        recty = 0.5 * buf_ht - fy / scale - 0.5 * recth;
      }

      if(buf_ok && fz == 1.0f && vals->full_w1 && vals->full_h1)
      {
        *(vals->full_w1) = buf_wd * scale;
        *(vals->full_h1) = buf_ht * scale;
      }

      if(!image_only) cairo_translate(cr, -0.5 * buf_wd + fx / scale, -0.5 * buf_ht + fy / scale);
      cairo_set_source_surface(cr, surface, 0, 0);
      // set filter no nearest:
      // in skull mode, we want to see big pixels.
      // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
      // in between, filtering just makes stuff go unsharp.
      if((buf_wd <= 8 && buf_ht <= 8) || fabsf(scale - 1.0f) < 0.01f)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);

      cairo_rectangle(cr, rectx, recty, rectw, recth);
      cairo_fill(cr);

      if(darktable.gui->show_focus_peaking)
      {
        cairo_save(cr);
        cairo_rectangle(cr, rectx, recty, rectw, recth);
        cairo_clip(cr);
        dt_focuspeaking(cr, width, height, cairo_image_surface_get_data(surface),
                                           cairo_image_surface_get_width(surface),
                                           cairo_image_surface_get_height(surface));
        cairo_restore(cr);
      }
      if(!vals->full_surface || !*(vals->full_surface)) cairo_surface_destroy(surface);
    }

    if(!vals->full_rgbbuf || !*(vals->full_rgbbuf)) free(rgbbuf);

    if(no_deco)
    {
      cairo_restore(cr);
      cairo_save(cr);
      cairo_new_path(cr);
    }
    else if(buf_ok)
    {
      // border around image
      if(selected && !vals->filmstrip && darktable.gui->colors[DT_GUI_COLOR_CULLING_SELECTED_BORDER].alpha > 0.0)
      {
        const float border = DT_PIXEL_APPLY_DPI(4.0 / scale);
        cairo_set_line_width(cr, border);
        cairo_rectangle(cr, rectx - border / 1.98, recty - border / 1.98, rectw + 0.99 * border,
                        recth + 0.99 * border);
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_CULLING_SELECTED_BORDER);
        cairo_stroke(cr);
      }

      // border around image filmstrip
      if(selected && vals->filmstrip
         && darktable.gui->colors[DT_GUI_COLOR_CULLING_FILMSTRIP_SELECTED_BORDER].alpha > 0.0)
      {
        const float border = DT_PIXEL_APPLY_DPI(4.0 / scale);
        cairo_set_line_width(cr, border);
        cairo_rectangle(cr, rectx - border / 1.98, recty - border / 1.98, rectw + 0.99 * border,
                        recth + 0.99 * border);
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_CULLING_FILMSTRIP_SELECTED_BORDER);
        cairo_stroke(cr);
      }

      // draw hover border if it's not transparent
      if(vals->mouse_over && darktable.gui->colors[DT_GUI_COLOR_PREVIEW_HOVER_BORDER].alpha > 0.0)
      {
        const float border = DT_PIXEL_APPLY_DPI(2.0 / scale);
        cairo_set_line_width(cr, border);
        cairo_rectangle(cr, rectx - border / 1.98, recty - border / 1.98, rectw + 0.99 * border,
                        recth + 0.99 * border);
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_PREVIEW_HOVER_BORDER);
        cairo_stroke(cr);
      }
    }
  }

  cairo_restore(cr);

  cairo_save(cr);
  const int z1_fontsize = fminf(DT_PIXEL_APPLY_DPI(20.0f), 0.91 * width / 10.0f);
  if(vals->mouse_over && zoom != 1)
  {
    // overlay a dark transparent background on thumbs to help legibility of overlays
    cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0.8528749999999999 * height,  0, height);
    cairo_pattern_add_color_stop_rgba(pat, 0, 0.5, 0.5, 0.5, 0);
    cairo_pattern_add_color_stop_rgba(pat, 0.25, 0.5, 0.5, 0.5, 0.25);
    cairo_pattern_add_color_stop_rgba(pat, 1, 0.5, 0.5, 0.5, 1);
    cairo_rectangle(cr, 0, 0.8528749999999999 * height, width, (1.0 - 0.8528749999999999) * height);
    cairo_set_source (cr, pat);
    cairo_fill (cr);
    cairo_pattern_destroy(pat);
  }
  cairo_restore(cr);

  if(buf_mipmap) dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  if(buf_mipmap && !missing && vals->full_surface && !*(vals->full_surface_w_lock) && mip >= DT_MIPMAP_7)
  {
    // we don't need this in the cache anymore, as we already have it in memory for zoom&pan
    // let's drop it to free space. This reduce the risk of getting out of space...
    dt_mipmap_cache_evict_at_size(cache, imgid, mip);
  }

  cairo_save(cr);

  if(vals->mouse_over || full_preview || darktable.gui->show_overlays || zoom == 1)
  {
    if(draw_metadata && width > DECORATION_SIZE_LIMIT)
    {
      // draw mouseover hover effects, set event hook for mouse button down!
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
      dt_gui_gtk_set_source_rgb(cr, outlinecol);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

      const gboolean extended_thumb_overlay = dt_conf_get_bool("plugins/lighttable/extended_thumb_overlay");
      const gboolean image_is_rejected = (img && ((img->flags & 0x7) == 6));

      // for preview, no frame except if rejected
      if(zoom == 1 && !image_is_rejected) cairo_new_path(cr);

      if(img)
      {
        if(zoom != 1 && (!darktable.gui->show_overlays || vals->mouse_over) && extended_thumb_overlay)
        {
          // size of stars overlays
          const double r1 = MIN(DT_PIXEL_APPLY_DPI(20.0), 0.91 * width / 10.0);
          const double fontsize = MIN(DT_PIXEL_APPLY_DPI(16.0), 0.67 * 0.91 * width / 10.0);
          const double exif_offset = 0.045 * width;
          const double line_offs = 1.25 * fontsize;
          const double overlay_height = 2 * exif_offset + r1 + 1.75 * line_offs;

          const double x0 = 0;
          const double y0 = height - overlay_height;
          const double rect_width = width;
          const double rect_height = overlay_height;

          cairo_save(cr);
          cairo_rectangle(cr, x0, y0, rect_width, rect_height);
          dt_gui_gtk_set_source_rgb(cr, bgcol);
          cairo_fill(cr);

          // some exif data
          PangoLayout *layout;
          PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
          pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
          layout = pango_cairo_create_layout(cr);
          pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
          pango_layout_set_font_description(layout, desc);
          dt_gui_gtk_set_source_rgb(cr, outlinecol);

          cairo_move_to(cr, x0 + exif_offset, y0 + exif_offset / 2.0);
          pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
          pango_layout_set_width(layout, (int)(PANGO_SCALE * (width - 2 * exif_offset)));
          pango_layout_set_text(layout, img->filename, -1);
          pango_cairo_show_layout(cr, layout);
          char exifline[50];
          cairo_move_to(cr, x0 + exif_offset, y0 + exif_offset / 2.0 + line_offs);
          dt_image_print_exif(img, exifline, sizeof(exifline));
          pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
          pango_layout_set_text(layout, exifline, -1);
          pango_cairo_show_layout(cr, layout);

          pango_font_description_free(desc);
          g_object_unref(layout);
          cairo_restore(cr);
        }

        if(!image_is_rejected) // if rejected: draw no stars
        {
          for(int k = 0; k < 5; k++)
          {
            const dt_view_image_over_t star = DT_VIEW_STAR_1 + k;
            if(dt_view_process_image_over(star, vals->mouse_over || zoom == 1, cr, img, width, height, zoom, px,
                                          py, outlinecol, fontcol))
              *image_over = star;
          }
        }
      }

      if(dt_view_process_image_over(DT_VIEW_REJECT, vals->mouse_over || zoom == 1, cr, img, width, height, zoom,
                                    px, py, outlinecol, fontcol))
        *image_over = DT_VIEW_REJECT;

      if(draw_audio && img && (img->flags & DT_IMAGE_HAS_WAV))
      {
        if(dt_view_process_image_over(DT_VIEW_AUDIO, vals->mouse_over || zoom == 1, cr, img, width, height, zoom,
                                      px, py, outlinecol, fontcol))
          *image_over = DT_VIEW_AUDIO;
      }

      if(draw_grouping)
      {
        DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
        DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);
        DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, imgid);
        DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, imgid);

        /* lets check if imgid is in a group */
        if(sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW)
          is_grouped = 1;
        else if(img && darktable.gui->expanded_group_id == img->group_id)
          darktable.gui->expanded_group_id = -1;
      }

      // image part of a group?
      if(is_grouped && darktable.gui && darktable.gui->grouping)
      {
        if(dt_view_process_image_over(DT_VIEW_GROUP, img != NULL, cr, img,
                                      width, height, zoom, px, py, outlinecol, fontcol))
          *image_over = DT_VIEW_GROUP;
      }

      // image altered?
      if(draw_history && dt_image_altered(imgid))
      {
        if(dt_view_process_image_over(DT_VIEW_ALTERED, img != NULL, cr, img,
                                      width, height, zoom, px, py, outlinecol, fontcol))
          darktable.gui->center_tooltip = 1;
      }
    }
  }
  cairo_restore(cr);

  // kill all paths, in case img was not loaded yet, or is blocked:
  cairo_new_path(cr);

  if(draw_colorlabels && (darktable.gui->show_overlays || vals->mouse_over || full_preview || zoom == 1))
  {
    // TODO: cache in image struct!

    // TODO: there is a branch that sets the bg == colorlabel
    //       this might help if zoom > 15
    if(width > DECORATION_SIZE_LIMIT)
    {
      // color labels:
      const float r = 0.0455 * width / 2.0;
      const float x[5] = {0.86425, 0.9325, 0.8983749999999999, 0.86425, 0.9325};
      const float y[5] = {0.86425, 0.86425, 0.8983749999999999, 0.9325, 0.9325};
      const int max_col = sizeof(x) / sizeof(x[0]);

      gboolean colorlabel_painted = FALSE;
      gboolean painted_col[] = {FALSE, FALSE, FALSE, FALSE, FALSE};

      /* clear and reset prepared statement */
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_color);
      DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_color);

      /* setup statement and iterate rows */
      DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_color, 1, imgid);
      while(sqlite3_step(darktable.view_manager->statements.get_color) == SQLITE_ROW)
      {
        cairo_save(cr);
        const int col = sqlite3_column_int(darktable.view_manager->statements.get_color, 0);
        if(col < max_col || zoom == 1)
        {
          // see src/dtgtk/paint.c
          if (zoom != 1)
            dtgtk_cairo_paint_label(cr, x[col]  * width, y[col] * height, r * 2, r * 2, col, NULL);
          else
            dtgtk_cairo_paint_label(cr, z1_fontsize + col * 0.75f * 1.5f * z1_fontsize, 6.0 * z1_fontsize,
                                    0.75f * z1_fontsize, 0.75f * z1_fontsize, col, NULL);
          colorlabel_painted = colorlabel_painted || TRUE;
          painted_col[col] = TRUE;
        }
        cairo_restore(cr);
      }
      if(colorlabel_painted && zoom != 1)
      {
        const int dont_fill_col = 7;
        for(int i = 0; i < max_col; i++)
        {
          if (!painted_col[i])
          {
            cairo_save(cr);
            dtgtk_cairo_paint_label(cr, x[i] * width, y[i] * height, r * 2, r * 2, dont_fill_col, NULL);
            cairo_restore(cr);
          }
        }
      }
    }
  }

  if (draw_local_copy)
  {
    if(img && width > DECORATION_SIZE_LIMIT)
    {
      const gboolean has_local_copy = (img && (img->flags & DT_IMAGE_LOCAL_COPY));

      if(has_local_copy)
      {
        cairo_save(cr);

        if (zoom != 1)
        {
          const double x0 = 0, y0 = 0;
          const double x1 = x0 + width;

          cairo_move_to(cr, x1 - z1_fontsize, y0);
          cairo_line_to(cr, x1, y0);
          cairo_line_to(cr, x1, y0 + z1_fontsize);
          cairo_close_path(cr);
          cairo_set_source_rgb(cr, 1, 1, 1);
          cairo_fill(cr);
        }
        else
        {
          cairo_move_to(cr, 0, 0);
          cairo_line_to(cr, 1.5 * z1_fontsize, 0);
          cairo_line_to(cr, 0, 1.5 * z1_fontsize);
          cairo_close_path(cr);
          cairo_set_source_rgb(cr, 1, 1, 1);
          cairo_fill(cr);
        }
        cairo_restore(cr);
      }
    }
  }

  if(draw_metadata && img && (zoom == 1))
  {
    // some exif data
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cr);
    pango_font_description_set_absolute_size(desc, z1_fontsize * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);

    cairo_move_to(cr, z1_fontsize, z1_fontsize);
    pango_layout_set_text(layout, img->filename, -1);
    pango_cairo_layout_path(cr, layout);
    char exifline[50];
    cairo_move_to(cr, z1_fontsize, 2.25f * z1_fontsize);
    dt_image_print_exif(img, exifline, sizeof(exifline));
    pango_layout_set_text(layout, exifline, -1);
    pango_cairo_layout_path(cr, layout);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_fill(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

  }

  // draw custom metadata from accompanying text file:
  if(draw_metadata && img && (img->flags & DT_IMAGE_HAS_TXT) && dt_conf_get_bool("plugins/lighttable/draw_custom_metadata")
     && (zoom == 1))
  {
    char *path = dt_image_get_text_path(img->id);
    if(path)
    {
      FILE *f = g_fopen(path, "rb");
      if(f)
      {
        char line[2048];
        PangoLayout *layout;
        PangoFontDescription *desc = pango_font_description_from_string("monospace bold");
        layout = pango_cairo_create_layout(cr);
        pango_font_description_set_absolute_size(desc, z1_fontsize * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);
        // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        int k = 0;
        while(!feof(f))
        {
          gchar *line_pattern = g_strdup_printf("%%%zu[^\n]", sizeof(line) - 1);
          const int read = fscanf(f, line_pattern, line);
          g_free(line_pattern);
          if(read != 1) break;
          fgetc(f); // munch \n

          cairo_move_to(cr, z1_fontsize, (k + 7.0) * z1_fontsize);
          cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
          pango_layout_set_text(layout, line, -1);
          pango_cairo_layout_path(cr, layout);
          cairo_stroke_preserve(cr);
          cairo_set_source_rgb(cr, .7, .7, .7);
          cairo_fill(cr);
          k++;
        }
        fclose(f);
        pango_font_description_free(desc);
        g_object_unref(layout);

      }
      g_free(path);
    }
  }

  cairo_restore(cr);
  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] image expose took %0.04f sec\n", end - start);
  return missing;
}

void
dt_view_image_only_expose(
  uint32_t imgid,
  cairo_t *cr,
  int32_t width,
  int32_t height,
  int32_t offsetx,
  int32_t offsety)
{
  dt_view_image_over_t image_over;
  dt_view_image_expose_t params = { 0 };
  params.image_over = &image_over;
  params.imgid = imgid;
  params.cr = cr;
  params.width = width;
  params.height = height;
  params.px = offsetx;
  params.py = offsety;
  params.zoom = 1;
  params.image_only = TRUE;
  params.full_preview = TRUE;

  dt_view_image_expose(&params);
}


/**
 * \brief Set the selection bit to a given value for the specified image
 * \param[in] imgid The image id
 * \param[in] value The boolean value for the bit
 */
void dt_view_set_selection(int imgid, int value)
{
  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);

  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    if(!value)
    {
      /* Value is set and should be unset; get rid of it */

      /* clear and reset statement */
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
      DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);

      /* setup statement and execute */
      DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
      sqlite3_step(darktable.view_manager->statements.delete_from_selected);
    }
  }
  else if(value)
  {
    /* Select bit is unset and should be set; add it */

    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
}

/**
 * \brief Toggle the selection bit in the database for the specified image
 * \param[in] imgid The image id
 */
void dt_view_toggle_selection(int imgid)
{

  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.delete_from_selected);
  }
  else
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
}

/**
 * \brief Reset filter
 */
void dt_view_filter_reset(const dt_view_manager_t *vm, gboolean smart_filter)
{
  if(vm->proxy.filter.module && vm->proxy.filter.reset_filter)
    vm->proxy.filter.reset_filter(vm->proxy.filter.module, smart_filter);
}

void dt_view_active_images_reset(gboolean raise)
{
  if(g_slist_length(darktable.view_manager->active_images) < 1) return;
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  if(raise) dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}
void dt_view_active_images_add(int imgid, gboolean raise)
{
  darktable.view_manager->active_images
      = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  if(raise) dt_control_signal_raise(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}
GSList *dt_view_active_images_get()
{
  return darktable.view_manager->active_images;
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.view_toolbox.module) vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module, tool, views);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.module_toolbox.module) vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module, tool, views);
}

dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.darkroom.view)
    return vm->proxy.darkroom.get_layout(vm->proxy.darkroom.view);
  else
    return DT_DARKROOM_LAYOUT_EDITING;
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom)
{
  if(vm->proxy.lighttable.module) vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

gint dt_view_lighttable_get_zoom(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_zoom(vm->proxy.lighttable.module);
  else
    return 10;
}

dt_lighttable_culling_zoom_mode_t dt_view_lighttable_get_culling_zoom_mode(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_zoom_mode(vm->proxy.lighttable.module);
  else
    return DT_LIGHTTABLE_ZOOM_FIXED;
}

dt_lighttable_layout_t dt_view_lighttable_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_layout(vm->proxy.lighttable.module);
  else
    return DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
}

gboolean dt_view_lighttable_preview_state(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_preview_state(vm->proxy.lighttable.view);
  else
    return FALSE;
}

void dt_view_lighttable_change_offset(dt_view_manager_t *vm, gboolean reset, gint imgid)
{
  if(vm->proxy.lighttable.module) vm->proxy.lighttable.change_offset(vm->proxy.lighttable.view, reset, imgid);
}

void dt_view_collection_update(const dt_view_manager_t *vm)
{
  if(vm->proxy.module_collect.module) vm->proxy.module_collect.update(vm->proxy.module_collect.module);
}


int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view) return vm->proxy.tethering.get_selected_imgid(vm->proxy.tethering.view);

  return -1;
}

void dt_view_tethering_set_job_code(const dt_view_manager_t *vm, const char *name)
{
  if(vm->proxy.tethering.view) vm->proxy.tethering.set_job_code(vm->proxy.tethering.view, name);
}

const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view) return vm->proxy.tethering.get_job_code(vm->proxy.tethering.view);
  return NULL;
}

#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom)
{
  if(vm->proxy.map.view) vm->proxy.map.center_on_location(vm->proxy.map.view, lon, lat, zoom);
}

void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2)
{
  if(vm->proxy.map.view) vm->proxy.map.center_on_bbox(vm->proxy.map.view, lon1, lat1, lon2, lat2);
}

void dt_view_map_show_osd(const dt_view_manager_t *vm, gboolean enabled)
{
  if(vm->proxy.map.view) vm->proxy.map.show_osd(vm->proxy.map.view, enabled);
}

void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source)
{
  if(vm->proxy.map.view) vm->proxy.map.set_map_source(vm->proxy.map.view, map_source);
}

GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points)
{
  if(vm->proxy.map.view) return vm->proxy.map.add_marker(vm->proxy.map.view, type, points);
  return NULL;
}

gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker)
{
  if(vm->proxy.map.view) return vm->proxy.map.remove_marker(vm->proxy.map.view, type, marker);
  return FALSE;
}
#endif

#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo)
{
  if (vm->proxy.print.view)
    vm->proxy.print.print_settings(vm->proxy.print.view, pinfo);
}
#endif


static gchar *_mouse_action_get_string(dt_mouse_action_t *ma)
{
  gchar *atxt = dt_util_dstrcat(NULL, "%s", gtk_accelerator_get_label(ma->key.accel_key, ma->key.accel_mods));
  if(strcmp(atxt, "")) atxt = dt_util_dstrcat(atxt, "+");
  switch(ma->action)
  {
    case DT_MOUSE_ACTION_LEFT:
      atxt = dt_util_dstrcat(atxt, _("Left click"));
      break;
    case DT_MOUSE_ACTION_RIGHT:
      atxt = dt_util_dstrcat(atxt, _("Right click"));
      break;
    case DT_MOUSE_ACTION_MIDDLE:
      atxt = dt_util_dstrcat(atxt, _("Middle click"));
      break;
    case DT_MOUSE_ACTION_SCROLL:
      atxt = dt_util_dstrcat(atxt, _("Scroll"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_LEFT:
      atxt = dt_util_dstrcat(atxt, _("Left double-click"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_RIGHT:
      atxt = dt_util_dstrcat(atxt, _("Right double-click"));
      break;
    case DT_MOUSE_ACTION_DRAG_DROP:
      atxt = dt_util_dstrcat(atxt, _("Drag and drop"));
      break;
    case DT_MOUSE_ACTION_LEFT_DRAG:
      atxt = dt_util_dstrcat(atxt, _("Left click+Drag"));
      break;
    case DT_MOUSE_ACTION_RIGHT_DRAG:
      atxt = dt_util_dstrcat(atxt, _("Right click+Drag"));
      break;
  }

  return atxt;
}

static void _accels_window_destroy(GtkWidget *widget, dt_view_manager_t *vm)
{
  // set to NULL so we can rely on it after
  vm->accels_window.window = NULL;
}

static void _accels_window_sticky(GtkWidget *widget, GdkEventButton *event, dt_view_manager_t *vm)
{
  if(!vm->accels_window.window) return;

  // creating new window
  GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(win));
  gtk_style_context_add_class(context, "accels_window");
  gtk_window_set_title(win, _("darktable - accels window"));
  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);

  gtk_window_set_resizable(win, TRUE);
  gtk_window_set_icon_name(win, "darktable");
  gtk_window_set_default_size(win, alloc.width * 0.7, alloc.height * 0.7);
  g_signal_connect(win, "destroy", G_CALLBACK(_accels_window_destroy), vm);

  GtkWidget *sw
      = (GtkWidget *)g_list_first(gtk_container_get_children(GTK_CONTAINER(vm->accels_window.window)))->data;
  g_object_ref(sw);

  gtk_container_remove(GTK_CONTAINER(vm->accels_window.window), sw);
  gtk_container_add(GTK_CONTAINER(win), sw);
  g_object_unref(sw);
  gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = GTK_WIDGET(win);
  gtk_widget_show_all(vm->accels_window.window);
  gtk_widget_hide(vm->accels_window.sticky_btn);

  vm->accels_window.sticky = TRUE;
}

void dt_view_accels_show(dt_view_manager_t *vm)
{
  if(vm->accels_window.window) return;

  vm->accels_window.sticky = FALSE;
  vm->accels_window.prevent_refresh = FALSE;

  GtkStyleContext *context;
  vm->accels_window.window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(vm->accels_window.window);
#endif
  context = gtk_widget_get_style_context(vm->accels_window.window);
  gtk_style_context_add_class(context, "accels_window");

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  context = gtk_widget_get_style_context(sw);
  gtk_style_context_add_class(context, "accels_window_scroll");

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  vm->accels_window.flow_box = gtk_flow_box_new();
  context = gtk_widget_get_style_context(vm->accels_window.flow_box);
  gtk_style_context_add_class(context, "accels_window_box");
  gtk_orientable_set_orientation(GTK_ORIENTABLE(vm->accels_window.flow_box), GTK_ORIENTATION_HORIZONTAL);

  gtk_box_pack_start(GTK_BOX(hb), vm->accels_window.flow_box, TRUE, TRUE, 0);

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  vm->accels_window.sticky_btn
      = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(vm->accels_window.sticky_btn), "tooltip-text",
               _("switch to a classic window which will stay open after key release."), (char *)NULL);
  g_signal_connect(G_OBJECT(vm->accels_window.sticky_btn), "button-press-event", G_CALLBACK(_accels_window_sticky),
                   vm);
  context = gtk_widget_get_style_context(vm->accels_window.sticky_btn);
  gtk_style_context_add_class(context, "accels_window_stick");
  gtk_box_pack_start(GTK_BOX(vb), vm->accels_window.sticky_btn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, FALSE, 0);

  dt_view_accels_refresh(vm);

  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);
  // gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(sw), alloc.width);
  gtk_container_add(GTK_CONTAINER(sw), hb);
  gtk_container_add(GTK_CONTAINER(vm->accels_window.window), sw);

  gtk_window_set_resizable(GTK_WINDOW(vm->accels_window.window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(vm->accels_window.window), alloc.width, alloc.height);
  gtk_window_set_transient_for(GTK_WINDOW(vm->accels_window.window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_keep_above(GTK_WINDOW(vm->accels_window.window), TRUE);
  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(vm->accels_window.window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_window_set_gravity(GTK_WINDOW(vm->accels_window.window), GDK_GRAVITY_STATIC);
  gtk_window_set_position(GTK_WINDOW(vm->accels_window.window), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(vm->accels_window.window);
}

void dt_view_accels_hide(dt_view_manager_t *vm)
{
  if(vm->accels_window.window && vm->accels_window.sticky) return;
  if(vm->accels_window.window) gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = NULL;
}

void dt_view_accels_refresh(dt_view_manager_t *vm)
{
  if(!vm->accels_window.window || vm->accels_window.prevent_refresh) return;

  // drop all existing tables
  GList *lw = gtk_container_get_children(GTK_CONTAINER(vm->accels_window.flow_box));
  while(lw)
  {
    GtkWidget *w = (GtkWidget *)lw->data;
    gtk_widget_destroy(w);
    lw = g_list_next(lw);
  }

  // get the list of valid accel for this view
  const dt_view_t *cv = dt_view_manager_get_current_view(vm);
  const dt_view_type_flags_t v = cv->view(cv);
  GtkStyleContext *context;

  typedef struct _bloc_t
  {
    gchar *base;
    gchar *title;
    GtkListStore *list_store;
  } _bloc_t;

  // go through all accels to populate categories with valid ones
  GList *blocs = NULL;
  GList *bl = NULL;
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *da = (dt_accel_t *)l->data;
    if(da && (da->views & v) == v)
    {
      GtkAccelKey ak;
      if(gtk_accel_map_lookup_entry(da->path, &ak) && ak.accel_key > 0)
      {
        // we want the base path
        gchar **elems = g_strsplit(da->translated_path, "/", -1);
        if(elems[0] && elems[1] && elems[2])
        {
          // do we already have a category ?
          bl = blocs;
          _bloc_t *b = NULL;
          while(bl)
          {
            _bloc_t *bb = (_bloc_t *)bl->data;
            if(strcmp(elems[1], bb->base) == 0)
            {
              b = bb;
              break;
            }
            bl = g_list_next(bl);
          }
          // if not found, we create it
          if(!b)
          {
            b = (_bloc_t *)calloc(1, sizeof(_bloc_t));
            b->base = dt_util_dstrcat(NULL, "%s", elems[1]);
            if(g_str_has_prefix(da->path, "<Darktable>/views/"))
              b->title = dt_util_dstrcat(NULL, "%s", cv->name(cv));
            else
              b->title = dt_util_dstrcat(NULL, "%s", elems[1]);
            b->list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
            blocs = g_list_prepend(blocs, b);
          }
          // we add the new line
          GtkTreeIter iter;
          gtk_list_store_prepend(b->list_store, &iter);
          gchar *txt;
          // for views accels, no need to specify the view name, it's in the category title
          if(g_str_has_prefix(da->path, "<Darktable>/views/"))
            txt = da->translated_path + strlen(elems[0]) + strlen(elems[1]) + strlen(elems[2]) + 3;
          else
            txt = da->translated_path + strlen(elems[0]) + strlen(elems[1]) + 2;
          // for dynamic accel, we need to add the "+scroll"
          gchar *atxt = dt_util_dstrcat(NULL, "%s", gtk_accelerator_get_label(ak.accel_key, ak.accel_mods));
          if(g_str_has_prefix(da->path, "<Darktable>/image operations/") && g_str_has_suffix(da->path, "/dynamic"))
            atxt = dt_util_dstrcat(atxt, _("+Scroll"));
          gtk_list_store_set(b->list_store, &iter, 0, atxt, 1, txt, -1);
          g_free(atxt);
          g_strfreev(elems);
        }
      }
    }
    l = g_slist_next(l);
  }

  // we add the mouse actions too
  if(cv->mouse_actions)
  {
    _bloc_t *bm = (_bloc_t *)calloc(1, sizeof(_bloc_t));
    bm->base = NULL;
    bm->title = dt_util_dstrcat(NULL, _("mouse actions"));
    bm->list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    blocs = g_list_prepend(blocs, bm);

    GSList *lm = cv->mouse_actions(cv);
    while(lm)
    {
      dt_mouse_action_t *ma = (dt_mouse_action_t *)lm->data;
      if(ma)
      {
        GtkTreeIter iter;
        gtk_list_store_append(bm->list_store, &iter);
        gchar *atxt = _mouse_action_get_string(ma);
        gtk_list_store_set(bm->list_store, &iter, 0, atxt, 1, ma->name, -1);
        g_free(atxt);
      }
      lm = g_slist_next(lm);
    }
    g_slist_free_full(lm, free);
  }

  // now we create and insert the widget to display all accels by categories
  bl = blocs;
  while(bl)
  {
    const _bloc_t *bb = (_bloc_t *)bl->data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // the title
    GtkWidget *lb = gtk_label_new(bb->title);
    context = gtk_widget_get_style_context(lb);
    gtk_style_context_add_class(context, "accels_window_cat_title");
    gtk_box_pack_start(GTK_BOX(box), lb, FALSE, FALSE, 0);

    // the list of accels
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(bb->list_store));
    context = gtk_widget_get_style_context(list);
    gtk_style_context_add_class(context, "accels_window_list");
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("Accel"), renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
    column = gtk_tree_view_column_new_with_attributes(_("Action"), renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);

    gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);

    gtk_flow_box_insert(GTK_FLOW_BOX(vm->accels_window.flow_box), box, -1);
    g_free(bb->base);
    g_free(bb->title);

    bl = g_list_next(bl);
  }
  g_list_free_full(blocs, free);

  gtk_widget_show_all(vm->accels_window.flow_box);
}

static void _audio_child_watch(GPid pid, gint status, gpointer data)
{
  dt_view_manager_t *vm = (dt_view_manager_t *)data;
  vm->audio.audio_player_id = -1;
  g_spawn_close_pid(pid);
}

void dt_view_audio_start(dt_view_manager_t *vm, int imgid)
{
  char *player = dt_conf_get_string("plugins/lighttable/audio_player");
  if(player && *player)
  {
    char *filename = dt_image_get_audio_path(imgid);
    if(filename)
    {
      char *argv[] = { player, filename, NULL };
      gboolean ret = g_spawn_async(NULL, argv, NULL,
                                   G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                                       | G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL, &vm->audio.audio_player_pid, NULL);

      if(ret)
      {
        vm->audio.audio_player_id = imgid;
        vm->audio.audio_player_event_source
            = g_child_watch_add(vm->audio.audio_player_pid, (GChildWatchFunc)_audio_child_watch, vm);
      }
      else
        vm->audio.audio_player_id = -1;

      g_free(filename);
    }
  }
  g_free(player);
}
void dt_view_audio_stop(dt_view_manager_t *vm)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(vm->audio.audio_player_id == -1) return;
  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(vm->audio.audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(vm->audio.audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(vm->audio.audio_player_pid))
      kill(-vm->audio.audio_player_pid, SIGKILL);
    else
      kill(vm->audio.audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(vm->audio.audio_player_pid);
  vm->audio.audio_player_id = -1;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
