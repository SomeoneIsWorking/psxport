// catalog.h — the test program product's one title.
#pragma once

#include "title_catalog.h"

#include <memory>

namespace psx::testprog {

std::unique_ptr<psx::host::TitleCatalog> createCatalog();

} // namespace psx::testprog
