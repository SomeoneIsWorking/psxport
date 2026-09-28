// cd_ready_delivery.h — the framework's own CD-ROM interrupt handler, as far as a title needs one.
//
// WHAT THIS IS. A stock libcd consumer does not learn that a sector arrived from a BIOS hardware
// EVENT CLASS; it learns it because the interrupt handler calls a function POINTER the guest
// installed (`CdReadyCallback`, the slot a title declares as
// `GuestCdStreamCallbackLayout::readyCallbackPointer`). On hardware that call is made by the
// BIOS's own CD-ROM interrupt handler, which is ROM code this framework does not have. So for a
// title that declares `DeliveryOwner::GuestInterrupt` the framework stands in for it: read the
// controller's response, acknowledge the controller, and dispatch the CURRENT value of the
// registered slot. That is the whole mechanism, and it is a different thing from `deliverEvent`:
// no event class is involved, which is why it works for a title that never opens one (psxport issue
// 0123 measured 9 OpenEvent calls and 0 of class 0xF0000003 over 2,324 presented frames).
//
// WHAT IT IS NOT, and the two refusals that keep it from becoming a second CD path:
//   * It NEVER runs for `DeliveryOwner::HostPump`. That is the double-delivery gate. A host-pump
//     title gets its one callback per completion from `Cd::pumpStream` / `cd_drive_stock_read`, and
//     this arm declining is what makes that exactly once rather than twice.
//   * `Hle::irqPoll` calls it only AFTER the guest's own registered interrupt elements declined the
//     interrupt. On hardware the guest's elements and the BIOS's built-in handler are one table and
//     the first matching entry runs, so a guest that services CD through its own element has already
//     delivered this completion when the chain declines nothing.
//
// SCOPE, stated rather than implied: the guest's own interrupt service dispatches every response
// type through a jump table, and this handler serves exactly one of them — the DATA-READY response,
// which is the completion a stream waits for and whose target in that table IS the ready callback.
// Command-acknowledge, command-complete and error responses belong to a title's libcd command state
// machine, which the framework already completes synchronously, and are declined here rather than
// guessed at.
//
// A COMPLETION IS ONLY CONSUMED WHEN IT IS DELIVERED. The controller response is acknowledged as
// part of the dispatch and never before it, so every refusal — masked line, re-entrant poll, no
// callback installed, no data-ready response current — leaves the completion owed, and the next poll
// delivers it. That is the rule the DMA arm's own comments state, and these completions chain the
// same way: the callback issues the next request, whose response becomes current before the handler
// returns, and the delivery must find it owed rather than already taken.
#pragma once

class Core;

// What one call to the handler did with the completion it found, or refused to find.
enum class CdReadyDelivery {
  NotOwned,    // this title keeps host dispatch: the arm is not part of its contract at all
  NothingOwed, // owned, but no data-ready completion is deliverable right now
  Delivered,   // one completion was consumed and its callback ran to return
  GuestExited, // delivered, and the guest carried a typed exit out of the handler
};

// Does this Core's title hand CD ready-callback delivery to its own interrupt path? True only for a
// DIRECT runtime that declares a valid layout owned by the guest interrupt. The legacy
// `GameConfig` shape has no declaration to honour and keeps host dispatch.
[[nodiscard]] bool cdReadyCallbackOwnedByGuestInterrupt(const Core &core);

// Is a CD data-ready response current and the guest's CD interrupt line asserted and unmasked? The
// answer to "is a completion owed at all", kept separate from the delivery so the deferred-work gate
// can treat this handler as a delivery path a title with no SysEnq element still has.
[[nodiscard]] bool cdReadyCompletionOwed(const Core &core);

// Deliver at most ONE owed data-ready completion to the guest's registered ready callback. Every
// `NothingOwed` leaves the completion owed, and the register context is restored on every path.
[[nodiscard]] CdReadyDelivery deliverCdReadyCompletionOnInterrupt(Core &core);
