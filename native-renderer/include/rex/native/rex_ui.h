// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "frame.h"
#include <rex/ui/immediate_drawer.h>
#include <functional>
namespace rex::native {
class NativeUIContext:public ui::UIDrawContext {
 public:
  NativeUIContext(ui::Presenter& p,Frame& f,Pass& pass):UIDrawContext(p,pass.width,pass.height),frame(f),pass(pass) {}
  // Headless composition uses the same drawer and packet ABI as presentation.
  NativeUIContext(Frame& f,Pass& pass):UIDrawContext(pass.width,pass.height),frame(f),pass(pass) {}
  Frame& frame;
  Pass& pass;
};
std::unique_ptr<ui::ImmediateDrawer> CreateNativeImmediateDrawer(std::function<Identity()> next);
} // namespace rex::native
