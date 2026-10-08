"""Bounded, read-only PC observer extension owned by the pinned console core."""

from __future__ import annotations

import ctypes as ct

MAX_TARGETS = 4
MAX_RANGES = 8
MAX_BYTES = 512
MAX_RECORDS = 128


class Target(ct.Structure):
    _fields_ = [("pc", ct.c_uint32), ("follow_return", ct.c_uint32)]


class RamRange(ct.Structure):
    _fields_ = [("address", ct.c_uint32), ("bytes", ct.c_uint32)]


class Config(ct.Structure):
    _fields_ = [("abi", ct.c_uint32), ("target_count", ct.c_uint32),
                ("range_count", ct.c_uint32), ("capacity", ct.c_uint32),
                ("targets", Target * MAX_TARGETS), ("ranges", RamRange * MAX_RANGES)]


class Status(ct.Structure):
    _fields_ = [(name, ct.c_uint64) for name in
                ("scanned", "matched", "retained", "dropped", "pairing_errors")] + [
        ("entries", ct.c_uint64 * MAX_TARGETS), ("returns", ct.c_uint64 * MAX_TARGETS),
        ("enabled", ct.c_uint32), ("queued", ct.c_uint32),
        ("pending", ct.c_uint32), ("complete", ct.c_uint32)]


class Record(ct.Structure):
    _fields_ = [("ordinal", ct.c_uint64), ("field", ct.c_uint64)] + [
        (name, ct.c_uint32) for name in ("target", "kind", "pc", "next_pc", "instruction",
                                       "branch_delay", "load_register", "load_value")] + [
        ("timestamp", ct.c_int32), ("gpr", ct.c_uint32 * 34),
        ("cp0_status", ct.c_uint32), ("cp0_cause", ct.c_uint32), ("cp0_epc", ct.c_uint32),
        ("ram_bytes", ct.c_uint32), ("ram", ct.c_uint8 * MAX_BYTES)]


def bind(library) -> None:
    declarations = {
        "abi": (ct.c_uint32, []), "size": (ct.c_uint32, [ct.c_uint32]),
        "configure": (ct.c_int, [ct.POINTER(Config)]), "disable": (None, []),
        "field": (None, [ct.c_uint64]), "status": (None, [ct.POINTER(Status)]),
        "drain": (ct.c_uint32, [ct.POINTER(Record), ct.c_uint32]),
    }
    for suffix, (result, arguments) in declarations.items():
        try:
            function = getattr(library, "retro_psx_observer_" + suffix)
        except AttributeError as error:
            raise ValueError("pinned console lacks the read-only PC observer ABI; rebuild") from error
        function.restype = result
        function.argtypes = arguments
    if library.retro_psx_observer_abi() != 1:
        raise ValueError("unsupported console PC observer ABI")
    for index, structure in enumerate((Config, Status, Record)):
        if library.retro_psx_observer_size(index) != ct.sizeof(structure):
            raise ValueError(f"console PC observer layout mismatch: {structure.__name__}")


class Observer:
    def __init__(self, library):
        self.library = library
        self.configuration: Config | None = None

    def configure(self, targets: list[tuple[int, bool]], ranges: list[tuple[int, int]],
                  capacity: int) -> dict:
        if not 1 <= len(targets) <= MAX_TARGETS or len(ranges) > MAX_RANGES:
            raise ValueError("observer requires 1..4 PC targets and at most 8 RAM ranges")
        if type(capacity) is not int or not 1 <= capacity <= MAX_RECORDS:
            raise ValueError("observer capacity must be 1..128 records")
        config = Config(1, len(targets), len(ranges), capacity)
        seen = set()
        for i, (pc, follow_return) in enumerate(targets):
            if (type(pc) is not int or not 0 <= pc <= 0xffffffff or pc & 3 or
                    pc in seen or type(follow_return) is not bool):
                raise ValueError("observer PC must be unique/aligned uint32 and return must be boolean")
            seen.add(pc)
            config.targets[i] = Target(pc, follow_return)
        total = 0
        for i, (address, size) in enumerate(ranges):
            if (type(address) is not int or not 0 <= address <= 0xffffffff or type(size) is not int or
                    (address & 0xe0000000) not in (0, 0x80000000, 0xa0000000) or
                    not 0 < size <= 0x200000 - (address & 0x1fffffff)):
                raise ValueError("observer range must be physical main RAM or its KSEG0/KSEG1 alias")
            total += size
            config.ranges[i] = RamRange(address, size)
        if total > MAX_BYTES:
            raise ValueError("observer ranges exceed the 512-byte record budget")
        if not self.library.retro_psx_observer_configure(ct.byref(config)):
            raise ValueError("console refused observer configuration; previous configuration preserved")
        self.configuration = config
        return self.status()

    def status(self) -> dict:
        state = Status()
        self.library.retro_psx_observer_status(ct.byref(state))
        result = {name: getattr(state, name) for name in
                  ("scanned", "matched", "retained", "dropped", "pairing_errors",
                   "enabled", "queued", "pending", "complete")}
        count = self.configuration.target_count if self.configuration is not None else 0
        result["targets"] = [{"pc": self.configuration.targets[i].pc,
                              "entries": state.entries[i], "returns": state.returns[i]}
                             for i in range(count)]
        result["observation"] = "complete" if state.complete else "incomplete"
        return result

    def drain(self) -> dict:
        records = (Record * MAX_RECORDS)()
        count = self.library.retro_psx_observer_drain(records, MAX_RECORDS)
        if count > MAX_RECORDS:
            raise RuntimeError("console observer exceeded its record bound")
        output = []
        for record in records[:count]:
            if record.ram_bytes > MAX_BYTES or record.kind > 1:
                raise RuntimeError("console observer returned an invalid record")
            values = {name: getattr(record, name) for name, _ in Record._fields_
                      if name not in ("gpr", "ram")}
            values["gpr"] = list(record.gpr)
            values["ram"] = bytes(record.ram[:record.ram_bytes]).hex()
            output.append(values)
        config = self.configuration
        ranges = [] if config is None else [
            {"address": config.ranges[i].address, "bytes": config.ranges[i].bytes}
            for i in range(config.range_count)]
        return {"phase": "before-opcode-after-fetch-and-cycle-bookkeeping",
                "ranges": ranges, "records": output, "status": self.status()}
