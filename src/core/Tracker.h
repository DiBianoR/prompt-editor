#pragma once

#include <cstddef>

#include "Index.h"

namespace pe {

// Keeps range offsets in step with edits made in the editor, so anchors never go
// stale from in-app typing. Text inserted strictly inside a range grows it;
// text inserted at a range's start or end stays outside it. A range whose text
// is deleted entirely becomes orphaned. Ranges hit by an edit are marked touched.
void trackInsert(Index& index, size_t pos, size_t length);
void trackDelete(Index& index, size_t pos, size_t length);

}  // namespace pe
