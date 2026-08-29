#pragma once

#include "GaokunPen.h"

#include "shared/Channel.h"

// 笔通道的名字。线路格式本身由 shared/Channel.h 的模板决定，写者与读者都实例化同一个
// 模板，因此这里只需要约定名字。
namespace Gaokun::Pen::Wire {

inline constexpr wchar_t kSnapshotName[] = L"Global\\GaokunPenSnapshot";
inline constexpr wchar_t kEventPipeName[] = L"\\\\.\\pipe\\GaokunPenEvents";
inline constexpr wchar_t kCommandPipeName[] = L"\\\\.\\pipe\\GaokunPenCommands";
inline constexpr wchar_t kStopEventPrefix[] = L"GaokunPenHostStop";

using SnapshotWriter = Channel::SeqlockWriter<Snapshot>;
using SnapshotReaderImpl = Channel::SeqlockReader<Snapshot>;
using EventWriter = Channel::EventPipeWriter<Event>;
using EventReaderImpl = Channel::EventPipeReader<Event>;
using CommandReader = Channel::CommandPipeReader<Command>;
using CommandWriterImpl = Channel::CommandPipeWriter<Command>;

} // namespace Gaokun::Pen::Wire
