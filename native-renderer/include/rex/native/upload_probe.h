// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "frame.h"
namespace rex::native {
Frame UploadProbe(bool indexed,uint32_t samples=1);
bool CheckUploadProbe(std::span<const std::byte>,bool indexed);
}
