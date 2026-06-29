# VodLink private OBS runtime notes

`obs_reset_video()` does not only need `obs.dll` and the graphics DLL.  Libobs also loads core shader/effect files through `obs_find_data_file()` during video reset, including `default.effect`, `format_conversion.effect`, and scale effects.  VodLink therefore bundles and validates `data/libobs` as part of the private runtime.

On Windows, the app now registers every private libobs data-path candidate before video reset and validates the required effect files before attempting D3D11/OpenGL.  If the original NV12 startup fails on a particular driver, VodLink also tries a safe BGRA startup path before reporting an error.
