// The framework's only process entry: the product's catalog, run by ProductHost.
// psxport_add_product names the catalog factory and the header that declares it.
#include "product_host.h"
#include "title_catalog.h"

#include PSXPORT_PRODUCT_CATALOG_HEADER

#include <memory>

int main(int argc, char **argv) {
  const std::unique_ptr<psx::host::TitleCatalog> catalog = PSXPORT_PRODUCT_CATALOG();
  psx::host::ProductHost host(*catalog);
  return host.run(argc, argv);
}
