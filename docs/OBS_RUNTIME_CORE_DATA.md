# OBS runtime core data rule

VodLink must not cherry-pick individual libobs shader/effect files.

The private OBS runtime must include the full `libobs/data` directory from the same OBS source archive/version used for the libobs headers. On Windows this is staged at:

```text
obs-runtime/data/libobs
```

On macOS this is staged at:

```text
obs-runtime/Resources/data/libobs
```

On Linux this is staged at:

```text
obs-runtime/usr/share/obs/libobs
```

`obs_reset_video()` depends on these core graphics/effect files. The CI scripts validate the key files, but the bundle copies the whole directory to avoid future OBS point-release breakage.
