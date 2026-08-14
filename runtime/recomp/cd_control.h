#pragma once

class Core;

// Apply one controller command through the native synchronous CD model and
// report blocking-control success. Game-specific libds wrappers may call this
// after validating which command/result contract they own; it is not a blanket
// command acceptor.
void cd_control_sync(Core *c);
