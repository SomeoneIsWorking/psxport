"""Only the libretro C ABI used by the separate full-console diagnostic host."""

from __future__ import annotations

import ctypes as ct
from pathlib import Path

from console_observer import bind as bind_observer

Environment = ct.CFUNCTYPE(ct.c_bool, ct.c_uint, ct.c_void_p)
VideoRefresh = ct.CFUNCTYPE(None, ct.c_void_p, ct.c_uint, ct.c_uint, ct.c_size_t)
AudioSample = ct.CFUNCTYPE(None, ct.c_int16, ct.c_int16)
AudioBatch = ct.CFUNCTYPE(ct.c_size_t, ct.POINTER(ct.c_int16), ct.c_size_t)
InputPoll = ct.CFUNCTYPE(None)
InputState = ct.CFUNCTYPE(ct.c_int16, ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint)


class GameInfo(ct.Structure):
    _fields_ = [("path", ct.c_char_p), ("data", ct.c_void_p),
                ("size", ct.c_size_t), ("meta", ct.c_char_p)]


class Variable(ct.Structure):
    _fields_ = [("key", ct.c_char_p), ("value", ct.c_char_p)]


class Geometry(ct.Structure):
    _fields_ = [("base_width", ct.c_uint), ("base_height", ct.c_uint),
                ("max_width", ct.c_uint), ("max_height", ct.c_uint),
                ("aspect_ratio", ct.c_float)]


class Timing(ct.Structure):
    _fields_ = [("fps", ct.c_double), ("sample_rate", ct.c_double)]


class SystemAvInfo(ct.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


def load_library(path: Path) -> ct.CDLL:
    library = ct.CDLL(str(path))
    declarations = {
        "retro_api_version": (ct.c_uint, []),
        "retro_set_environment": (None, [Environment]),
        "retro_set_video_refresh": (None, [VideoRefresh]),
        "retro_set_audio_sample": (None, [AudioSample]),
        "retro_set_audio_sample_batch": (None, [AudioBatch]),
        "retro_set_input_poll": (None, [InputPoll]),
        "retro_set_input_state": (None, [InputState]),
        "retro_set_controller_port_device": (None, [ct.c_uint, ct.c_uint]),
        "retro_init": (None, []),
        "retro_deinit": (None, []),
        "retro_load_game": (ct.c_bool, [ct.POINTER(GameInfo)]),
        "retro_unload_game": (None, []),
        "retro_run": (None, []),
        "retro_get_system_av_info": (None, [ct.POINTER(SystemAvInfo)]),
        "retro_get_memory_data": (ct.c_void_p, [ct.c_uint]),
        "retro_get_memory_size": (ct.c_size_t, [ct.c_uint]),
    }
    for name, (result, arguments) in declarations.items():
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments
    if library.retro_api_version() != 1:
        raise ValueError("unsupported libretro API version; expected 1")
    bind_observer(library)
    return library
