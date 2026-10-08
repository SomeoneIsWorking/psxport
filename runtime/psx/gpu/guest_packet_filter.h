// GuestPacketFilter — preserve a guest producer's execution while replacing only its later visual
// packet contribution.  A title scopes the exact guest body it super-calls; OtAttr stores that owner
// beside the packet-pool span, and GP0 execution suppresses only packets whose stored owner is enabled.
// No guest register, scratchpad, GTE, packet-pool, or ordering-table state is changed by this facility.
#pragma once

#include "ot_attr.h"

#include <cstdint>
#include <cstdlib>

class GuestPacketFilter {
public:
  static constexpr int MAX_DEPTH = 16;
  static constexpr int MAX_SUPPRESSED = 16;

  void setSuppressed(uint32_t guestProducer, bool suppressed) {
    if (!guestProducer) {
      std::abort();
    }
    for (int i = 0; i < mSuppressedCount; ++i) {
      if (mSuppressed[i] == guestProducer) {
        if (!suppressed) {
          mSuppressed[i] = mSuppressed[--mSuppressedCount];
        }
        return;
      }
    }
    if (!suppressed) {
      return;
    }
    if (mSuppressedCount == MAX_SUPPRESSED) {
      std::abort();
    }
    mSuppressed[mSuppressedCount++] = guestProducer;
  }

  bool suppresses(uint32_t guestProducer) const {
    for (int i = 0; i < mSuppressedCount; ++i) {
      if (mSuppressed[i] == guestProducer) {
        return true;
      }
    }
    return false;
  }

  // A span that is absent or unowned stays visible.  Suppression must never turn incomplete packet
  // provenance into a guessed producer, because that would silently drop unrelated guest geometry.
  bool suppressesPacket(const OtAttr &attr, uint32_t packetAddress) const {
    OtAttr::Span span{};
    return attr.lookupStore(packetAddress, &span) && suppresses(span.guestProducer);
  }

  uint32_t currentOwner() const {
    return mDepth ? mOwners[mDepth - 1] : 0u;
  }

private:
  friend class GuestPacketOwnerScope;

  void pushOwner(uint32_t guestProducer) {
    if (!guestProducer || mDepth == MAX_DEPTH) {
      std::abort();
    }
    mOwners[mDepth++] = guestProducer;
    ++g_guest_packet_owner_scope_depth;
  }

  void popOwner() {
    if (!mDepth || !g_guest_packet_owner_scope_depth) {
      std::abort();
    }
    --mDepth;
    --g_guest_packet_owner_scope_depth;
  }

  uint32_t mOwners[MAX_DEPTH] = {};
  uint32_t mSuppressed[MAX_SUPPRESSED] = {};
  int mDepth = 0;
  int mSuppressedCount = 0;
};

// Scope the guest body whose packets a native producer may replace.  It is deliberately distinct
// from ProducerScope: the latter attributes native queue submissions, while this scope persists a
// guest packet owner until the delayed DMA/OT walk executes it.
class GuestPacketOwnerScope {
public:
  GuestPacketOwnerScope(GuestPacketFilter *filter, uint32_t guestProducer) : mFilter(filter) {
    if (mFilter) {
      mFilter->pushOwner(guestProducer);
    }
  }
  ~GuestPacketOwnerScope() {
    if (mFilter) {
      mFilter->popOwner();
    }
  }
  GuestPacketOwnerScope(const GuestPacketOwnerScope &) = delete;
  GuestPacketOwnerScope &operator=(const GuestPacketOwnerScope &) = delete;

private:
  GuestPacketFilter *mFilter;
};
