# Instrument a generated SDK source copy. Fail configuration if pinned hook sites drift.
file(READ "${AJA_SDK_DIR}/demos/ntv2burn/ntv2burn.cpp" _aja_burn)
string(REPLACE "\r\n" "\n" _aja_burn "${_aja_burn}")
function(aja_hook before after)
    string(FIND "${_aja_burn}" "${before}" _hook_position)
    if(_hook_position EQUAL -1)
        message(FATAL_ERROR "AJA profiling hook not found: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" _aja_burn "${_aja_burn}")
    set(_aja_burn "${_aja_burn}" PARENT_SCOPE)
endfunction()
aja_hook("if (goodXfers == 3)" "if (goodXfers == (outputStatus.GetFrameCount() <= 3 ? 2u : 3u))")
# The upstream example subscribes only to frame store 1, then waits on its
# actual output frame store (which may differ from the physical SDI jack).
aja_hook("mDevice.EnableChannel(mConfig.fOutputChannel);"
    "if (!mDevice.EnableOutputInterrupt(mConfig.fOutputChannel) || !mDevice.SubscribeOutputVerticalEvent(mConfig.fOutputChannel)) return AJA_STATUS_INITIALIZE; mDevice.EnableChannel(mConfig.fOutputChannel);")
aja_hook("mDevice.UnsubscribeInputVerticalEvent(mConfig.fInputChannel);"
    "mDevice.UnsubscribeInputVerticalEvent(mConfig.fInputChannel); mDevice.UnsubscribeOutputVerticalEvent(mConfig.fOutputChannel); if (mConfig.fOutputChannel != NTV2_CHANNEL1) mDevice.UnsubscribeOutputVerticalEvent(NTV2_CHANNEL1);")
# HasAvailableInputFrame (>1) protects the audio write head. This application
# disables audio, so one completed video frame is eligible for DMA capture.
aja_hook("acStatus.IsRunning()  &&  acStatus.HasAvailableInputFrame()"
    "acStatus.IsRunning() && (mConfig.WithAudio() ? acStatus.HasAvailableInputFrame() : acStatus.GetBufferLevel() >= 1)")
aja_hook("mFrameDataRing.Add(&frameData);" "mFrameDataRing.Add(&frameData); sdi_profile::registerFrame(&frameData);")
aja_hook("void NTV2Burn::PlayFrames (void)\n{" "void NTV2Burn::PlayFrames (void)\n{ sdi_profile::lane = 1;")
aja_hook("void NTV2Burn::CaptureFrames (void)\n{" "void NTV2Burn::CaptureFrames (void)\n{ sdi_profile::lane = 0;")
aja_hook("mFrameDataRing.StartProduceNextBuffer ()" "sdi_profile::acquire(sdi_profile::Kind::CaptureHostWait, 1, [&] { return mFrameDataRing.StartProduceNextBuffer (); })")
aja_hook("mFrameDataRing.StartConsumeNextBuffer()" "sdi_profile::acquire(sdi_profile::Kind::OutputHostWait, 3, [&] { return mFrameDataRing.StartConsumeNextBuffer(); })")
aja_hook("mDevice.AutoCirculateTransfer (mConfig.fInputChannel, inputXferInfo)" "sdi_profile::timed(sdi_profile::Kind::CaptureDma, [&] { return mDevice.AutoCirculateTransfer (mConfig.fInputChannel, inputXferInfo); })")
aja_hook("mDevice.AutoCirculateTransfer (mConfig.fOutputChannel, outputXferInfo)" "sdi_profile::timed(sdi_profile::Kind::OutputDma, [&] { return mDevice.AutoCirculateTransfer (mConfig.fOutputChannel, outputXferInfo); })")
aja_hook("else badXfers++;" "else badXfers++; const auto profileProcessingStart = sdi_profile::Clock::now();")
aja_hook("mFrameDataRing.EndProduceNextBuffer();" "sdi_profile::record(sdi_profile::Kind::Processing, profileProcessingStart); sdi_profile::stage(pFrameData, 2); sdi_profile::record(sdi_profile::Kind::HostPublish, sdi_profile::Clock::now()); mFrameDataRing.EndProduceNextBuffer();")
aja_hook("mFrameDataRing.EndConsumeNextBuffer ();" "sdi_profile::stage(pFrameData, 0); sdi_profile::record(sdi_profile::Kind::HostRelease, sdi_profile::Clock::now()); mFrameDataRing.EndConsumeNextBuffer ();")
aja_hook("mDevice.WaitForInputFieldID (NTV2_FIELD0, mConfig.fInputChannel);" "sdi_profile::timed(sdi_profile::Kind::InputDeviceWait, [&] { return mDevice.WaitForInputFieldID (NTV2_FIELD0, mConfig.fInputChannel); });")
aja_hook("mDevice.WaitForOutputVerticalInterrupt(mConfig.fOutputChannel);" "sdi_profile::timed(sdi_profile::Kind::OutputDeviceWait, [&] { return mDevice.WaitForOutputVerticalInterrupt(mConfig.fOutputChannel); });")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/aja-burn.cpp" "#include \"sdi_profile.hpp\"\n${_aja_burn}")
