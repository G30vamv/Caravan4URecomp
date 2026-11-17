// Derived from the Mint applet of this tree (mint.h), itself derived from Azahar src/core/hle/applets/mint.h and
// mint.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version").
// Copyright 2016 Citra Emulator Project (the Mint applet's flow)
//
// The Extrapad applet (Windows port, roadmap phase 4), HLE for the ids Extrapad (0x208) and Extrapad2 (0x408): the
// Circle Pad Pro's setup applet, which MH4U starts when the player opens the Circle Pad Pro setting in the options.
// Azahar does not implement it ("Could not create applet 408": the game then breaks with PANIC and the process ends).
// Here it behaves as the Mint applet does: it accepts the parameter and closes at once, answering a buffer of zeros of
// the size of the parameter it got. The emulated Circle Pad Pro (ir:USER device 1, --circle-pad-pro on) is always
// connected, so there is nothing to pair or calibrate.
//
// Departures from Azahar: the applet exists at all. What the real applet writes in its answer is not known (nobody
// has reversed its parameter format): zeros are what the Mint applet answers, and whether the game accepts them is
// checked by playing (docs of the Windows port, PLATFORM_PORTING.md).
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <memory>
#include <vector>

#include "applets/applet.h"

namespace mh4n::apt {

class Extrapad final : public Applet {
public:
    Extrapad(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host, ServiceContext& context)
        : Applet(id, parent, std::move(host)), context_(context) {}

    Result ReceiveParameterImpl(const MessageParameter& parameter) override;
    Result Start(const MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override {}

private:
    ServiceContext& context_;
    // Made when the Request message arrives (the framebuffer info of ImportDisplayCaptureInfo).
    std::shared_ptr<SharedMemory> framebuffer_memory_;
    // Parameter received by the applet on start: only its size matters.
    std::vector<u8> startup_param_;
};

}  // namespace mh4n::apt
