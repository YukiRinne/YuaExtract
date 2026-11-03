// 最小自包含检查：**只**包含 Common/AtomicFile.h，不依赖其它头的包含顺序。
// 编译通过 = AtomicFile.h 自己带齐了 Windows / STL / PathSafety 依赖。

#include "AtomicFile.h"

int AtomicFileHeaderSelfCheck()
{
    Common::AtomicFile::StagedFile staged;
    std::wstring error;
    return static_cast<int>(staged.target.size() + staged.temp.size() + staged.backup.size()
                            + error.size());
}
