#pragma once

#include "GaokunKeyboard.h"

#include "shared/Channel.h"

// 键盘通道的名字。线路格式由 shared/Channel.h 的模板决定，与笔完全同构。
namespace Gaokun::Keyboard::Wire {

inline constexpr wchar_t kSnapshotName[] = L"Global\\GaokunKbdSnapshot";
inline constexpr wchar_t kEventPipeName[] = L"\\\\.\\pipe\\GaokunKbdEvents";
inline constexpr wchar_t kStopEventPrefix[] = L"GaokunKbdHostStop";

using SnapshotWriter = Channel::SeqlockWriter<Snapshot>;
using SnapshotReaderImpl = Channel::SeqlockReader<Snapshot>;
using EventWriter = Channel::EventPipeWriter<Event>;
using EventReaderImpl = Channel::EventPipeReader<Event>;

} // namespace Gaokun::Keyboard::Wire
