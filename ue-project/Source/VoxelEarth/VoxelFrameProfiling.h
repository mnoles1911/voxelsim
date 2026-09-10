#pragma once
#include "ProfilingDebugging/CsvProfiler.h"

// Inclusive CPU scopes. Pawn includes movement; clipmap may include roof/ray
// queries. Do not add nested scopes together as independent frame costs.
CSV_DECLARE_CATEGORY_EXTERN(VoxelStream);
