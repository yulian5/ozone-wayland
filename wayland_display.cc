// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ozone/wayland_display.h"

#include <stdlib.h>
#include <string.h>

#include "base/message_loop/message_loop.h"
#include "ozone/wayland_input_device.h"
#include "ozone/wayland_screen.h"
#include "ozone/wayland_window.h"
#include "ozone/wayland_input_method_event_filter.h"

namespace ui {

WaylandDisplay* g_display = NULL;
static const struct wl_callback_listener syncListener = {
    WaylandDisplay::SyncCallback
};

WaylandDisplay* WaylandDisplay::GetDisplay()
{
  return g_display;
}

void WaylandDisplay::DestroyDisplay()
{
  if (g_display)
    delete g_display;

  g_display = NULL;
}

// static
WaylandDisplay* WaylandDisplay::Connect(char* name)
{
  if (g_display)
    return g_display;

  g_display = new WaylandDisplay(name);

  return g_display;
}

WaylandDisplay::WaylandDisplay(char* name) : display_(NULL),
    compositor_(NULL),
    shell_(NULL),
    shm_(NULL),
    queue_(NULL),
    handle_flush_(false)
{
  display_ = wl_display_connect(name);
  if (!display_)
      return;

  static const struct wl_registry_listener registry_listener = {
    WaylandDisplay::DisplayHandleGlobal
  };

  input_method_filter_ = new WaylandInputMethodEventFilter;
  registry_ = wl_display_get_registry(display_);
  wl_registry_add_listener(registry_, &registry_listener, this);

  if (wl_display_roundtrip(display_) < 0) {
      terminate();
      return;
  }

  wl_display_set_user_data(display_, this);
  queue_ = wl_display_create_queue(display_);
  wl_proxy_set_queue((struct wl_proxy *)registry_, queue_);
}

WaylandDisplay::~WaylandDisplay()
{
  terminate();
}

void WaylandDisplay::AddWindow(WaylandWindow* window)
{
  if(window) {
    window_list_.push_back(window);
    handle_flush_ = true;
  }
}

void WaylandDisplay::AddTask(WaylandTask* task)
{
  if(task)
    task_list_.push_back(task);
}

bool WaylandDisplay::ProcessTasks()
{
  WaylandTask *task = NULL;
  if (task_list_.empty())
      return false;

  while(!task_list_.empty())
  {
    task = task_list_.front();
    task->Run();
    task_list_.pop_front();
    delete task;
  }

  return true;
}

void WaylandDisplay::FlushTasks()
{
  if (!handle_flush_ && task_list_.empty())
      return;

  Flush();
}

void WaylandDisplay::Flush()
{
  ProcessTasks();
  while (wl_display_prepare_read(display_) != 0)
      wl_display_dispatch_pending(display_);

  wl_display_flush(display_);
  wl_display_read_events(display_);
  wl_display_dispatch_pending(display_);
  handle_flush_ = false;
}

void WaylandDisplay::RemoveWindow(WaylandWindow* window)
{
  if(!window)
    return;

  WaylandTask *task = NULL;
  handle_flush_ = true;

  for (std::list<WaylandTask*>::iterator i = task_list_.begin();
      i != task_list_.end(); ++i) {
    if((*i)->GetWindow() == window)
    {
      delete *i;
      i = task_list_.erase(i);
    }
  }

  for (std::list<WaylandWindow*>::iterator i = window_list_.begin();
      i != window_list_.end(); ++i) {
    if((*i) == window)
    {
      i = window_list_.erase(i);
      break;
    }
  }

  if(window_list_.size() < 1)
  {
    base::MessageLoop::current()->PostTask(FROM_HERE, base::MessageLoop::QuitClosure());
  }
}

bool WaylandDisplay::IsWindow(WaylandWindow* window)
{
  for (std::list<WaylandWindow*>::iterator i = window_list_.begin();
      i != window_list_.end(); ++i) {
    if((*i) == window)
    {
      return true;
    }
  }

  return false;
}

InputMethod* WaylandDisplay::GetInputMethod() const
{
  return input_method_filter_ ? input_method_filter_->GetInputMethod(): NULL;
}

void WaylandDisplay::terminate()
{
  if (window_list_.size() > 0)
    fprintf(stderr, "warning: windows exist.\n");

  if (task_list_.size() > 0)
    fprintf(stderr, "warning: deferred tasks exist.\n");

  for (std::list<WaylandInputDevice*>::iterator i = input_list_.begin();
      i != input_list_.end(); ++i) {
      delete *i;
  }

  for (std::list<WaylandScreen*>::iterator i = screen_list_.begin();
      i != screen_list_.end(); ++i) {
      delete *i;
  }

  if (queue_) {
    wl_event_queue_destroy(queue_);
    queue_ = 0;
  }

  if (compositor_)
    wl_compositor_destroy(compositor_);

  if (shell_)
    wl_shell_destroy(shell_);

  if (shm_)
    wl_shm_destroy(shm_);

  if (registry_)
      wl_registry_destroy(registry_);

  delete input_method_filter_;

  if (display_) {
    wl_display_flush(display_);
    wl_display_disconnect(display_);
  }
}

std::list<WaylandScreen*> WaylandDisplay::GetScreenList() const {
  return screen_list_;
}

void WaylandDisplay::SyncCallback(void *data, struct wl_callback *callback, uint32_t serial)
{
    int* done = static_cast<int*>(data);
    *done = 1;
    wl_callback_destroy(callback);
}

int WaylandDisplay::SyncDisplay()
{
  if (!queue_)
    return -1;

  ProcessTasks();
  int done = 0, ret = 0;
  handle_flush_ = false;
  struct wl_callback* callback = wl_display_sync(display_);
  wl_callback_add_listener(callback, &syncListener, &done);
  wl_proxy_set_queue((struct wl_proxy *) callback, queue_);
  while (ret != -1 && !done)
    ret = wl_display_dispatch_queue(display_, queue_);
  wl_display_dispatch_pending(display_);

  return ret;
}

// static
void WaylandDisplay::DisplayHandleGlobal(void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{

  WaylandDisplay* disp = static_cast<WaylandDisplay*>(data);

  if (strcmp(interface, "wl_compositor") == 0) {
    disp->compositor_ = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 1));
  } else if (strcmp(interface, "wl_output") == 0) {
    WaylandScreen* screen = new WaylandScreen(disp, name);
    disp->screen_list_.push_back(screen);
  } else if (strcmp(interface, "wl_seat") == 0) {
    WaylandInputDevice *input_device = new WaylandInputDevice(disp, name);
    disp->input_list_.push_back(input_device);
  } else if (strcmp(interface, "wl_shell") == 0) {
    disp->shell_ = static_cast<wl_shell*>(
        wl_registry_bind(registry, name, &wl_shell_interface, 1));
  } else if (strcmp(interface, "wl_shm") == 0) {
    disp->shm_ = static_cast<wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));
  }
}

}  // namespace ui
