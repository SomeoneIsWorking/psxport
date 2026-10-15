#include "title_disc.h"

#include "saved_disc.h"

namespace psx::host {

TitleDisc::TitleDisc(const TitleIdentity &title) {
  disc_state_init(&disc_);
  SavedDisc(title).bind(disc_);
}

TitleDisc::TitleDisc(const std::filesystem::path &image) {
  disc_state_init(&disc_);
  psx::cd::openDiscImage(disc_, image.c_str());
}

TitleDisc::~TitleDisc() {
  psx::cd::closeDisc(disc_);
}

bool TitleDisc::opened() {
  return disc_.chd != nullptr || disc_open(&disc_) != 0;
}

} // namespace psx::host
