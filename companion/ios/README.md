# Nightglass iOS companion boundary

Generate the Xcode project with [XcodeGen](https://github.com/yonaskolb/XcodeGen):

```sh
xcodegen generate
open NightglassCompanion.xcodeproj
```

The app connects to protocol v1, reads status, and subscribes to watch commands.
See `../../docs/IOS_COMPANION.md` for the supported boundary and provisioning
requirements. A physical iPhone and a signed development build are required for
BLE validation.
