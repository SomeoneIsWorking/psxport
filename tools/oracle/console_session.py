"""A full-console libretro session, isolated from the PSXPort product and CPU-window shim."""

from __future__ import annotations

import ctypes as ct
import hashlib
from pathlib import Path
from typing import Callable

import console_abi as abi
from console_capture import CaptureHashes, Framebuffer
from console_observer import Observer

EXPERIMENTAL = 0x10000
SYSTEM_RAM = 2
SAVE_RAM = 0
RAM_BYTES = 2 * 1024 * 1024
# Memory-card slot 1 is 16 blocks x 64 frames x 128 bytes. The core owns slot 1 through the
# libretro save-RAM pointer rather than a file (libretro.c: retro_get_memory_data returns the slot-0
# device's NV data whenever use_mednafen_memcard0_method is false, which is its default and ours),
# so this reference's card never reaches the saves directory and is never named by a filename.
CARD_BYTES = 128 * 1024
MAX_STEP_FRAMES = 3600
BUTTONS = {"cross": 0, "square": 1, "select": 2, "start": 3, "up": 4, "down": 5,
           "left": 6, "right": 7, "circle": 8, "triangle": 9, "l1": 10, "r1": 11,
           "l2": 12, "r2": 13, "l3": 14, "r3": 15}

# Values use the pinned core's option contract, not frontend or PSXPort defaults.
REFERENCE_OPTIONS = {
    "cpu_freq_scale": "100%", "cpu_dynarec": "disabled", "cd_access_method": "sync",
    "cd_fastload": "2x(native)", "skip_bios": "disabled", "override_bios": "disabled",
    "internal_resolution": "1x(native)", "renderer": "software", "pgxp_mode": "disabled",
    "widescreen_hack": "disabled", "crop_overscan": "disabled", "image_crop": "disabled",
    "frame_duping": "disabled",
}


class ConsoleSession:
    def __init__(self, library, system_directory: Path, save_directory: Path,
                 option_overrides: dict[str, str] | None = None):
        self.library = library
        self.system_directory = str(system_directory).encode()
        self.save_directory = str(save_directory).encode()
        # The pinned reference contract above, with any caller override. The only override in use is
        # the picture oracle's `crop_overscan`, which asks the CORE to publish exactly its active
        # display area instead of the padded scanline. Copying the core's own 350->320 offset table
        # into Python would be a second source of truth for it; asking the core is not.
        self.option_overrides = dict(option_overrides or {})
        self.options: dict[bytes, bytes] = {}
        self.requested_options: dict[str, str] = {}
        self.unsupported_environment: dict[int, int] = {}
        self.framebuffer: Framebuffer | None = None
        self.pixel_format = 0
        self.frames = 0
        self.video_refreshes = 0
        self.duplicate_frames = 0
        self.audio_frames = 0
        self.input_polls = 0
        self.input_reads = 0
        self.button_mask = 0
        self.callback_error: BaseException | None = None
        self.initialized = False
        self.loaded = False
        self.av = abi.SystemAvInfo()
        self.observer = Observer(library)
        self.hashes: CaptureHashes | None = None
        # CFUNCTYPE pointers do not keep their Python callables alive through a C owner. These
        # references intentionally outlive retro_unload_game/retro_deinit and every foreign call.
        self.callbacks = {
            "environment": abi.Environment(self._guard(self._environment, False)),
            "video_refresh": abi.VideoRefresh(self._guard(self._video, None)),
            "audio_sample": abi.AudioSample(self._guard(self._audio_sample, None)),
            "audio_sample_batch": abi.AudioBatch(self._guard(self._audio_batch, 0)),
            "input_poll": abi.InputPoll(self._guard(self._input_poll, None)),
            "input_state": abi.InputState(self._guard(self._input_state, 0)),
        }

    def _guard(self, operation: Callable, failure):
        def callback(*arguments):
            if self.callback_error is not None:
                return failure
            try:
                return operation(*arguments)
            except BaseException as error:
                # Exceptions cannot cross a libffi C callback. Poison this session, refuse further
                # callback work, and raise at the immediately enclosing foreign-call boundary. The
                # CLI then unloads/terminates; it never resumes an unknown console state.
                self.callback_error = error
                return failure
        return callback

    def _check_callbacks(self) -> None:
        if self.callback_error is not None:
            raise RuntimeError(f"fatal libretro callback failure: {self.callback_error}") from self.callback_error

    @staticmethod
    def _store(data, c_type, value) -> None:
        if not data:
            raise ValueError("required libretro environment output pointer is NULL")
        ct.cast(data, ct.POINTER(c_type))[0] = value

    def _variables(self, data) -> None:
        if not data:
            raise ValueError("core published a NULL variable table")
        variables = ct.cast(data, ct.POINTER(abi.Variable))
        for index in range(512):
            entry = variables[index]
            if not entry.key:
                return
            if not entry.value:
                raise ValueError("core variable has no value/options")
            key = entry.key.decode()
            _, delimiter, choices = entry.value.decode().partition("; ")
            if not delimiter or not choices:
                raise ValueError(f"malformed core option {key}")
            values = choices.split("|")
            suffix = key.removeprefix("beetle_psx_")
            chosen = self.option_overrides.get(suffix, REFERENCE_OPTIONS.get(suffix, values[0]))
            if chosen not in values:
                raise ValueError(f"pinned core cannot supply reference option {key}={chosen}")
            self.options[entry.key] = chosen.encode()
        raise ValueError("core variable table exceeded 512 entries")

    def _environment(self, command: int, data) -> bool:
        if command == 3:  # GET_CAN_DUPE
            self._store(data, ct.c_bool, True)
        elif command in (9, 31):  # GET_SYSTEM_DIRECTORY, GET_SAVE_DIRECTORY
            self._store(data, ct.c_char_p,
                        self.system_directory if command == 9 else self.save_directory)
        elif command == 10:  # SET_PIXEL_FORMAT
            value = ct.cast(data, ct.POINTER(ct.c_int))[0]
            if value not in (0, 1, 2):
                raise ValueError(f"software core requested unsupported pixel format {value}")
            self.pixel_format = value
        elif command == 15:  # GET_VARIABLE
            variable = ct.cast(data, ct.POINTER(abi.Variable)).contents
            value = self.options.get(variable.key)
            if value is None:
                return False  # Optional unknown setting: the core owns its documented fallback.
            variable.value = value
            self.requested_options[variable.key.decode()] = value.decode()
        elif command == 16:  # SET_VARIABLES: advertise API 0 so definitions use this one owner.
            self._variables(data)
        elif command == 17:  # GET_VARIABLE_UPDATE
            self._store(data, ct.c_bool, False)
        elif command == 52:  # GET_CORE_OPTIONS_VERSION
            self._store(data, ct.c_uint, 0)
        elif command == 39:  # GET_LANGUAGE: English
            self._store(data, ct.c_uint, 0)
        elif command == (47 | EXPERIMENTAL):  # GET_AUDIO_VIDEO_ENABLE: execute both, mute only sink.
            self._store(data, ct.c_int, 3)
        elif command == (51 | EXPERIMENTAL):  # GET_INPUT_BITMASKS
            return True
        elif command == 32:  # SET_SYSTEM_AV_INFO
            ct.memmove(ct.byref(self.av), data, ct.sizeof(self.av))
        elif command == 37:  # SET_GEOMETRY
            self.av.geometry = ct.cast(data, ct.POINTER(abi.Geometry)).contents
        elif command in (6, 8, 11, 18, 35, 44, 55):
            # Optional UI metadata (message/performance/inputs/no-game/controllers/quirks/options).
            return True
        else:
            self.unsupported_environment[command] = self.unsupported_environment.get(command, 0) + 1
            return False
        return True

    def _video(self, pointer, width: int, height: int, pitch: int) -> None:
        self.video_refreshes += 1
        if not pointer:
            if self.framebuffer is None:
                raise ValueError("core duplicated a frame before publishing any framebuffer")
            self.duplicate_frames += 1
            return
        self.framebuffer = Framebuffer.capture(pointer, width, height, pitch, self.pixel_format)

    def _audio_sample(self, left: int, right: int) -> None:
        self.audio_frames += 1
        if self.hashes is not None:
            self.hashes.audio_sample(left, right)

    def _audio_batch(self, data, frames: int) -> int:
        if frames > 1_000_000 or (frames and not data):
            raise ValueError("invalid libretro audio batch")
        self.audio_frames += frames
        if self.hashes is not None:
            self.hashes.audio_batch(ct.string_at(data, frames * 4))
        return frames  # All frames consumed. SPU/audio timing advances without a playback device.

    def _input_poll(self) -> None:
        self.input_polls += 1

    def _input_state(self, port: int, device: int, index: int, button: int) -> int:
        self.input_reads += 1
        if port != 0 or device != 1 or index != 0:
            return 0
        if button == 256:
            return ct.c_int16(self.button_mask).value
        return int(button < 16 and bool(self.button_mask & (1 << button)))

    def open(self, disc: Path) -> None:
        if self.initialized:
            raise ValueError("console session is already initialized")
        for name, callback in self.callbacks.items():
            getattr(self.library, "retro_set_" + name)(callback)
            self._check_callbacks()
        self.library.retro_init()
        self.initialized = True
        self._check_callbacks()
        # Filename storage and the struct remain alive through retro_unload_game.
        self.game_path = str(disc).encode()
        self.game_info = abi.GameInfo(self.game_path, None, 0, None)
        self.loaded = bool(self.library.retro_load_game(ct.byref(self.game_info)))
        self._check_callbacks()
        if not self.loaded:
            raise ValueError(f"full-console core refused disc {disc}")
        self.library.retro_set_controller_port_device(0, 1)  # Real digital PSX pad.
        self.library.retro_get_system_av_info(ct.byref(self.av))
        self._check_callbacks()
        if self.library.retro_get_memory_size(SYSTEM_RAM) != RAM_BYTES:
            raise ValueError("full-console core did not expose its 2 MiB main RAM owner")

    def insert_card(self, image: bytes) -> dict:
        """Replace memory-card slot 1's contents with `image`, so this reference can be started from
        the SAME card state the product was given instead of the blank card a fresh core comes up
        with. Card state changes a title's menu route -- Spyro shows its card-creation page on an
        unformatted card and its save picker on a formatted one -- so when the two cores are given
        different cards, a comparison of that menu measures the harness, not the product.

        This writes the core's own non-volatile buffer; it is the same buffer a libretro frontend
        restores a save into. Slot 1 is deliberately never flushed to disk by the core, so nothing
        here leaves a file behind for the next run to inherit.
        """
        if not self.loaded:
            raise ValueError("inserting a card requires loaded console content")
        if self.frames:
            raise ValueError(f"the card must be inserted before the console is stepped; this session "
                             f"has already run {self.frames} field(s)")
        if len(image) != CARD_BYTES:
            raise ValueError(f"a PSX memory card is exactly {CARD_BYTES} bytes; got {len(image)}")
        size = self.library.retro_get_memory_size(SAVE_RAM)
        pointer = self.library.retro_get_memory_data(SAVE_RAM)
        if not pointer or size != CARD_BYTES:
            raise ValueError(f"core does not expose a {CARD_BYTES}-byte memory card through save RAM "
                             f"(pointer {bool(pointer)}, size {size}); it cannot be given a card")
        ct.memmove(pointer, image, CARD_BYTES)
        written = ct.string_at(pointer, CARD_BYTES)
        if written != image:
            raise ValueError("the core's card buffer did not take the image that was written to it")
        return {"card_bytes": CARD_BYTES, "card_sha256": hashlib.sha256(image).hexdigest(),
                "magic": written[:2].decode("latin-1")}

    def close(self) -> None:
        if self.loaded:
            self.library.retro_psx_observer_disable()
            self.library.retro_unload_game()
            self.loaded = False
        if self.initialized:
            self.library.retro_deinit()
            self.initialized = False

    def set_buttons(self, names: list[str]) -> None:
        if not isinstance(names, list) or any(not isinstance(name, str) for name in names):
            raise ValueError("buttons must be a list of exact button names")
        if len(names) != len(set(names)) or any(name not in BUTTONS for name in names):
            raise ValueError("unknown or duplicate button; allowed: " + ", ".join(BUTTONS))
        self.button_mask = sum(1 << BUTTONS[name] for name in names)

    def step(self, frames: int) -> dict:
        if not self.loaded:
            raise ValueError("no console content is loaded")
        if type(frames) is not int or not 1 <= frames <= MAX_STEP_FRAMES:
            raise ValueError(f"step frames must be an integer between 1 and {MAX_STEP_FRAMES}")
        self._check_callbacks()
        for _ in range(frames):
            before = self.video_refreshes
            self.library.retro_psx_observer_field(self.frames + 1)
            self.library.retro_run()
            self._check_callbacks()
            if self.video_refreshes == before:
                raise RuntimeError("retro_run returned without a video refresh; no field evidence")
            self.frames += 1
            if self.hashes is not None:
                self.hashes.field(self.read_ram(0, RAM_BYTES), self.framebuffer)
        return self.status()

    def begin_hashes(self) -> dict:
        if not self.loaded:
            raise ValueError("hash observation requires loaded console content")
        self.hashes = CaptureHashes()
        return self.hashes.status()

    def read_ram(self, address: int, size: int) -> bytes:
        if not self.loaded:
            raise ValueError("RAM read requires loaded console content")
        physical = address & 0x1FFFFFFF
        if not 0 <= address <= 0xFFFFFFFF or not 0 <= physical < RAM_BYTES:
            raise ValueError("RAM address must resolve inside PSX main RAM")
        if not 0 < size <= RAM_BYTES - physical:
            raise ValueError("RAM read exceeds main RAM bounds")
        pointer = self.library.retro_get_memory_data(SYSTEM_RAM)
        if not pointer or self.library.retro_get_memory_size(SYSTEM_RAM) != RAM_BYTES:
            raise ValueError("core main RAM owner is missing or changed size")
        return ct.string_at(pointer + physical, size)

    def status(self) -> dict:
        frame = self.framebuffer
        return {"frames": self.frames, "video_refreshes": self.video_refreshes,
                "duplicate_frames": self.duplicate_frames, "audio_frames_consumed": self.audio_frames,
                "input_polls": self.input_polls, "input_reads": self.input_reads,
                "buttons": [name for name, bit in BUTTONS.items() if self.button_mask & (1 << bit)],
                "framebuffer": None if frame is None else
                {"width": frame.width, "height": frame.height, "pitch": frame.pitch,
                 "pixel_format": frame.pixel_format},
                "fps": self.av.timing.fps, "sample_rate": self.av.timing.sample_rate,
                "core_options": self.requested_options,
                "unsupported_environment": self.unsupported_environment}
