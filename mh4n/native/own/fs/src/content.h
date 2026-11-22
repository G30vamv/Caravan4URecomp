// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: 3dbrew Filesystem Services, NCCH, CIA and RomFS.

#pragma once

#include "storage.h"

namespace mh4n::wp30 {

u64 GameTitle(const fs::path& game);
Result OpenContent(const fs::path& game, const fs::path& user, const Archive& archive,
                   LowPath path, FileData& data);

}  // namespace mh4n::wp30
