#include "choice_navigator.h"

#include <utility>

namespace psx::ui {

ChoiceNavigator::ChoiceNavigator(std::vector<ChoiceEntry> entries) : mEntries(std::move(entries)) {
  for (int i = 0; i < static_cast<int>(mEntries.size()); ++i) {
    if (mEntries[i].enabled) {
      mSelected = i;
      break;
    }
  }
}

int ChoiceNavigator::enabledCount() const {
  int count = 0;
  for (const ChoiceEntry &entry : mEntries) {
    count += entry.enabled ? 1 : 0;
  }
  return count;
}

bool ChoiceNavigator::move(int direction) {
  const int size = static_cast<int>(mEntries.size());
  if (mSelected < 0 || size == 0 || direction == 0) {
    return false;
  }
  const int step = direction > 0 ? 1 : -1;
  int candidate = mSelected;
  for (int tried = 0; tried < size; ++tried) {
    candidate = (candidate + step + size) % size;
    if (mEntries[candidate].enabled) {
      const bool moved = candidate != mSelected;
      mSelected = candidate;
      return moved;
    }
  }
  return false;
}

bool ChoiceNavigator::select(int index) {
  if (index < 0 || index >= static_cast<int>(mEntries.size()) || !mEntries[index].enabled) {
    return false;
  }
  mSelected = index;
  return true;
}

std::optional<int> ChoiceNavigator::activate() const {
  if (mSelected < 0 || !mEntries[mSelected].enabled) {
    return std::nullopt;
  }
  return mSelected;
}

} // namespace psx::ui
