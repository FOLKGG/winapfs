/* winapfs: MSVC has no __packed suffix; pack apfsprogs' on-disk structs. */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
#include "../../../../third_party/apfsprogs/include/apfs/raw.h"
#ifdef _MSC_VER
#pragma pack(pop)
#endif
