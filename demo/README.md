# Mustermark feature-video runner

Run `scripts/run-demo.sh`. It builds the demo driver, switches to workspace 2, and opens a fullscreen Mustermark window with captions. Start screen recording, then press **F8**. The five-second countdown leads into a roughly five-minute tour. **F10** closes the demo at any time. The final caption tells you when to stop recording.

The tour demonstrates contextual help, subtree movement and undo, task completion, heading promotion, numbered-list append, Insert mode, image attachments, Visual editing, a few CSS accent colors without changing the theme or layout, and a real HTTP API update reflected in the same document. There is no Feed the Flock integration in this runner.

Each run uses a new `/tmp/mustermark-demo-*` directory. Your documents, global stylesheet, and existing windows are preserved. The temporary directory is retained so you can inspect the Markdown, local image, stylesheet, and saved results. The demo uses the same QML editor and document controller as Mustermark, with a separate driver for keyboard events and captions; it does not add automation controls to the installed app.

For unattended playback, use `scripts/run-demo.sh --autoplay`. For an accelerated offscreen verification, use `scripts/run-demo.sh --smoke`. Set `MUSTERMARK_DEMO_BUILD_DIR` to reuse an existing CMake build. The desktop run requires Hyprland; the smoke run does not switch workspaces. The runner does not start or stop your screen recorder. The default pacing factor is 2: narration holds for about 6–10 seconds with additional pauses after actions. Set `MUSTERMARK_DEMO_PACE=3` for an even slower run.
