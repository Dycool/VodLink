# VodLink libobs streaming checklist

VodLink embeds a private OBS runtime and uses libobs as the streaming backend. The intended runtime flow is:

1. Prepare the private OBS runtime and private config/cache/log directories.
2. Call `obs_startup(locale, privateModuleConfigPath, nullptr)`.
3. Configure video with `obs_reset_video()` and audio with `obs_reset_audio()`.
4. Register only matched OBS plugin/data path pairs from VodLink's private runtime.
5. Load modules with `obs_load_all_modules2()` and finish with `obs_post_load_modules()`.
6. Create a private scene and sources.
7. Assign the scene source to output channel 0 with `obs_set_output_source(0, sceneSource)`.
8. Create hardware-only video encoder and AAC audio encoder, attach them to `obs_get_video()` / `obs_get_audio()`.
9. Create the `rtmp_common` service, create `rtmp_output`, set service and encoders on the output, then call `obs_output_start()`.
10. On stop/failure, disconnect output signals, release all OBS objects, then call `obs_shutdown()`.

Important stability rules:

- Never register dependency folders such as `bin`, `Frameworks`, or plain `usr/lib` as OBS module folders. Only real OBS plugin folders are module binary paths.
- Keep each plugin binary path paired with its matching plugin data path. Do not cross-product every binary path with every data path.
- Settings UI must not start libobs just to populate a dropdown.
- Windows must link against `obs.lib`, never `obs.dll`; the runtime DLL is embedded separately and extracted before use.
- Linux packaging must upload `VodLink-x86_64.AppImage`, not `linuxdeploy-x86_64.AppImage` or `linuxdeploy-plugin-qt-x86_64.AppImage`.

Windows graphics-runtime notes:

- `obs_reset_video()` must be able to load `bin/64bit/libobs-d3d11.dll` and its private dependencies before any capture/output source is created.
- The private extraction in `%LOCALAPPDATA%/VodLink/VodLink/obs-runtime` is stamp-checked against the embedded manifest. When the embedded OBS runtime changes, VodLink replaces the stale extracted runtime instead of mixing old DLLs with a new exe.
- The Windows CI dependency closure must copy transitive OBS DLL dependencies into `bin/64bit`, not a typo folder. Missing `d3dcompiler_47.dll`, `dxcompiler.dll`, or `dxil.dll` can show up as `OBS video initialization failed (-1)`.
- If D3D11 fails, VodLink now reports the exact graphics DLL preflight status and tries the available DXGI adapters before falling back to the bundled OpenGL graphics module.
