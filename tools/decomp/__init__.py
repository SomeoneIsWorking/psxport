"""Guest code to readable C: one bounded, gated pipeline for every title.

The modules, and the one concept each owns:

  ``images``     which bytes a guest address names, per title, as DATA (a manifest) not as code
  ``lock``       the one-Ghidra-at-a-time lock, so a co-tenant's build is never OOM-killed
  ``headless``   how Ghidra is invoked at all (launcher, heap ceiling, refusals)
  ``postscript`` the Ghidra-side post-script: clear the no-return guess, inventory, decompile
  ``report``     what the inventory claims, with denominators, and whether it is trustworthy
  ``pipeline``   the composition: lock -> prepare -> invoke -> report

Ghidra 12.0.4 facts this package encodes, each MEASURED on this machine and each a trap that
produces confident wrong answers rather than absences:

  1. ``analyzeHeadless`` on its own CANNOT run a Python script. It logs
     "Ghidra was not started with PyGhidra. Python is not available", prints "Post-analysis
     succeeded", and EXITS 0. A tool that trusts that exit code reports a run that never happened.
     Python scripts run only under PyGhidra, so the launcher here is the ``pyghidra.ghidra_launch``
     MODULE and not the ``pyghidraRun`` shell wrapper.
  2. The ``pyghidraRun`` wrapper consumes one positional fewer than the headless analyzer needs, so
     a correct command line arrives as "Bad argument: <project name>" AND the process still exits 0.
  3. The heap ceiling is ``-X mx<N>m`` on the PyGhidra launcher. ``MAXMEM`` is read by Ghidra's
     ``launch.sh`` and is IGNORED on the PyGhidra path, which starts a JVM through JPype instead.
  4. The Raw Binary loader defines NO entry point, so "Disassemble Entry Points" analyses nothing
     and a flat import reaches 0 instructions while logging "Analysis succeeded". The text window
     from the manifest is therefore disassembled explicitly, before analysis.
  5. The project directory must already exist; Ghidra's "Directory not found" abort is otherwise a
     confusing failure.
"""

__all__ = ["headless", "images", "lock", "pipeline", "report"]
