# iOS (Xcode-first)

Upstream [raylib](https://github.com/raysan5/raylib) **does not** register `PLATFORM=iOS` in its CMake `CMakeOptions.txt`, so this Gramarye template has **no** `cmake` / `./build_*.sh` path for iOS on Linux or macOS. You build the game inside an **Xcode app** on a **Mac**.

Practical starting points:

- [ghera/raylib-iOS](https://github.com/ghera/raylib-iOS) — Xcode-oriented raylib on iOS.
- [raylib PR #3880](https://github.com/raysan5/raylib/pull/3880), [discussion #2681](https://github.com/raysan5/raylib/discussions/2681) — upstream direction and notes.

Copy [ios/Info.plist.sample](ios/Info.plist.sample) into your Xcode project and set your real **bundle identifier** and display name.

---

## What you need

| Requirement | Why |
|-------------|-----|
| **Mac** with **Xcode** (current major version) | Only Apple’s toolchain produces `iphoneos` binaries and signs them. |
| **Apple ID** | Free account works for **personal device** installs (see below). **Paid** [Apple Developer Program](https://developer.apple.com/programs/) is required for TestFlight / App Store and avoids some free-tier limits. |
| **iPhone** + cable (or configured **wireless debugging**) | To install and launch the `.app` on hardware. |

---

## Recommended path: start from raylib-iOS (or clone + adapt)

1. On your Mac, clone **[ghera/raylib-iOS](https://github.com/ghera/raylib-iOS)** (or fork it) and open the **`.xcodeproj` / `.xcworkspace`** in Xcode.
2. Confirm the sample **builds for a simulator** (Product → Destination → some **iPhone** simulator), then switch to **your physical iPhone** (see signing below).
3. Replace or extend the sample game sources with **your** `src/*.c` and headers from this Gramarye template, and add **gramarye-libcore** + **gramarye-ecs** either:
   - as **subprojects** / “Add Files…” with compile flags matching your desktop build, or  
   - as **static libraries** built separately for `iphoneos` and linked into the app target.
4. Keep **OpenGL ES** (or GLES via **ANGLE**) as required on iOS; do not assume desktop OpenGL.

---

## Run on your physical iPhone (“boot it to my phone”)

### 1. Connect and trust the device

- Plug the iPhone into the Mac with USB (simplest first time).
- On the iPhone: unlock, tap **Trust** if prompted, enter passcode.

### 2. Signing (so Xcode may install the app)

1. In Xcode, select the **app target** → **Signing & Capabilities**.
2. Check **Automatically manage signing**.
3. Set **Team** to your **Apple ID** team (Xcode → Settings → Accounts → add Apple ID if needed).
4. Set a unique **Bundle Identifier** (e.g. `com.yourname.yourgame`); it must not collide with another app on the store or another team’s id.

**Free Apple ID:** Xcode creates a **development certificate** and **provisioning profile** for your device; builds installed to the device are typically valid for **about 7 days**, then you must rebuild/reinstall. **Paid program:** longer-lived signing and distribution options (TestFlight, App Store).

### 3. Select the phone and Run

1. In the toolbar **scheme** menu (next to Stop/Run), choose **your iPhone** by name (not a simulator).
2. **Product → Run** (⌘R).  
   Xcode builds, signs, installs, and launches the app on the phone.

### 4. First launch on device (free / personal team)

If the app opens then **closes immediately** or you see **“Untrusted Developer”**:

- On the iPhone: **Settings → General → VPN & Device Management** (or **Profiles & Device Management**) → your developer profile → **Trust**.

Then launch the app again from the home screen or from Xcode.

### 5. Wireless debugging (optional, after USB works)

- iPhone: **Settings → Developer** (visible after Xcode has used the device) → enable **Connect via network** if shown.  
- Xcode: **Window → Devices and Simulators** → select the device → **Connect via network**.  
After pairing, you can often run **⌘R** without USB.

---

## If something fails

- **“Failed to code sign” / “No signing certificate”:** Fix **Team** and **Bundle ID**; in Accounts, download manual profiles if Xcode suggests it.
- **Device not listed:** Cable, trust dialog, iOS version supported by your Xcode (update Xcode or iOS as needed).
- **Link errors for raylib / GLES:** Ensure all sources and frameworks match **iOS** (not macOS) SDK; follow **raylib-iOS** layout.

This template’s **Linux desktop `./build_*.sh` scripts do not produce an iOS app**; use Xcode on a Mac as above.
