"""Per-draw GPU time of a RenderDoc capture, run inside qrenderdoc (it carries the `renderdoc` module):

    QT_QPA_PLATFORM=offscreen RDOC_CAPTURE=x.rdc RDOC_OUT=times.txt qrenderdoc --python tools/renderdoc_times.py

Captures come from PSXPORT_RENDERDOC_AT under `renderdoccmd capture` (docs/presentation.md). Replay times each event
on its own, so every draw carries a fixed overhead: the sums run several times the frame's real GPU time and only rank
draws roughly. For the cost of a change, use `gputime` on two builds instead.
"""
import os, sys, collections, traceback
import renderdoc as rd

out = open(os.environ["RDOC_OUT"], "w")
def say(*a):
    print(*a, file=out); out.flush()

try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None) != rd.ResultCode.Succeeded:
        raise SystemExit("cannot open capture")
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise SystemExit("cannot replay: %s" % status)
    sf = ctl.GetStructuredFile()
    counters = ctl.EnumerateCounters()
    say("counters available:", len(counters))
    durations = {r.eventId: r.value.d for r in ctl.FetchCounters([rd.GPUCounter.EventGPUDuration])}
    rows = []
    def walk(actions, passName):
        for a in actions:
            name = a.GetName(sf)
            if a.children:
                walk(a.children, name if a.flags & rd.ActionFlags.PassBoundary or "Pass" in name else passName)
                continue
            if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Copy | rd.ActionFlags.Clear | rd.ActionFlags.Dispatch)):
                continue
            ctl.SetFrameEvent(a.eventId, False)
            ps = ctl.GetPipelineState()
            shader = ps.GetShaderReflection(rd.ShaderStage.Pixel)
            pipe = str(ps.GetGraphicsPipelineObject())
            entry = (shader.debugInfo.files[0].filename if shader and shader.debugInfo.files else (shader.entryPoint if shader else "-"))
            rows.append((a.eventId, durations.get(a.eventId, 0.0) * 1e3, name, pipe, a.numIndices, passName))
    walk(ctl.GetRootActions(), "-")
    total = sum(r[1] for r in rows)
    say("events %d, gpu total %.3f ms" % (len(rows), total))
    by = collections.defaultdict(lambda: [0.0, 0, 0])
    for eid, ms, name, pipe, n, pas in rows:
        k = (pipe, name.split("(")[0])
        by[k][0] += ms; by[k][1] += 1; by[k][2] += n
    say("\nby pipeline:")
    for k, (ms, c, n) in sorted(by.items(), key=lambda kv: -kv[1][0]):
        say("  %8.3f ms %5d events %8d verts  %s %s" % (ms, c, n, k[0], k[1]))
    say("\ntop events:")
    for eid, ms, name, pipe, n, pas in sorted(rows, key=lambda r: -r[1])[:40]:
        say("  eid %6d %8.3f ms %7d verts %s %s" % (eid, ms, n, pipe, name[:60]))
except BaseException:
    say(traceback.format_exc())
out.close()
os._exit(0)
