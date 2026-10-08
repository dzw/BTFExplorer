Windows Template Library (WTL) 10.0 — header-only copy

Source: https://sourceforge.net/projects/wtl/ (WTL 10.0, `Include` directory, 20 headers).
License: Microsoft Public License (MS-PL), http://opensource.org/licenses/MS-PL — see the
license header at the top of each file.

WTL 10 does not ship its own window/windowing classes: it builds on ATL's
`ATL::CWindow` / `ATL::CWindowImpl`, so including these headers requires the WTL include
directory to come *before* the ATL `atlmfc\include` directory in the compiler's search path.
See `PagedExplorer.vcxproj` (`AdditionalIncludeDirectories`, `atls.lib`).

Used by `src/UI/PaneHost.*` (the Q-Dir-style pane container window class).
