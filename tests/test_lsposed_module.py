from __future__ import annotations

import pathlib
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE = ROOT / "lsposed-module" / "app" / "src" / "main"


class LsposedModuleContractTests(unittest.TestCase):
    def test_scope_is_system_only(self) -> None:
        scope = (MODULE / "resources/META-INF/xposed/scope.list").read_text(
            encoding="utf-8"
        )
        targets = [line.strip() for line in scope.splitlines() if line.strip()]
        self.assertEqual(["system"], targets)

        props = (MODULE / "resources/META-INF/xposed/module.prop").read_text(
            encoding="utf-8"
        )
        self.assertIn("staticScope=true", props)
        self.assertIn("targetApiVersion=102", props)

    def test_ui_never_requests_per_app_scope(self) -> None:
        activity = (MODULE / "java/com/gushu/longvc/MainActivity.java").read_text(
            encoding="utf-8"
        )
        self.assertIn('Collections.singletonList("system")', activity)
        self.assertNotIn('requestScope(Collections.singletonList("com.', activity)

        module = (MODULE / "java/com/gushu/longvc/ModuleMain.java").read_text(
            encoding="utf-8"
        )
        self.assertIn("onSystemServerStarting", module)
        self.assertNotIn("onPackageLoaded", module)

    def test_microphone_is_explicit_opt_in(self) -> None:
        activity = (MODULE / "java/com/gushu/longvc/MainActivity.java").read_text(
            encoding="utf-8"
        )
        self.assertIn("micSwitch.setChecked(false)", activity)
        self.assertIn("RootController.Action.ARM_MIC", activity)

        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        arm_case = script.index("arm_mic()")
        disarm_case = script.index("disarm_mic()")
        self.assertLess(arm_case, disarm_case)
        self.assertIn("unload_audio_hook", script[disarm_case:])
        self.assertIn("reconcile_stale_leases", script)
        self.assertIn('rm -f "$CONTROL/mic.lease" "$MIC_MODE"', script[disarm_case:])
        audio = (ROOT / "engines/camreplace/android/audhook/src/audhook_main.cpp"
                 ).read_text(encoding="utf-8")
        substitute = audio.split("static void apply_substitute", 1)[1]
        self.assertLess(substitute.index("prepare_virtual_mic_read"),
                        substitute.index("audio_header_snapshot"))

    def test_root_script_keeps_selinux_enforcing(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        lowered = script.lower()
        self.assertNotIn("setenforce 0", lowered)
        self.assertNotIn("chmod 777", lowered)
        self.assertNotIn("chmod 0777", lowered)
        self.assertIn("/proc/$hook_candidate/maps", script)
        self.assertIn("controller_busy", script)
        self.assertIn(".longvc-control.stale.$$", script)
        self.assertIn("trap 'exit 1' HUP INT TERM", script)

    def test_failed_arm_rolls_back_partial_runtime(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        camera = script.split("arm_camera()", 1)[1].split("disarm_camera()", 1)[0]
        photo = script.split("arm_photo()", 1)[1].split("disarm_photo()", 1)[0]
        visual = script.split("arm_visual()", 1)[1].split("arm_camera()", 1)[0]
        mic = script.split("arm_mic()", 1)[1].split("disarm_mic()", 1)[0]
        feeder = script.split("start_feeder()", 1)[1].split("hook_pid()", 1)[0]

        self.assertIn("arm_visual camera video_mode", camera)
        self.assertIn("arm_visual photo photo_mode", photo)
        self.assertIn("stop_video_passthrough", visual)
        self.assertIn("stop_audio_passthrough", mic)
        self.assertIn("unload_audio_hook", mic)
        self.assertIn('stop_feeder "$start_tag"', feeder)

    def test_emergency_disable_does_not_require_native_payload(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        arm_start = script.index("arm-camera|arm-camera-fresh")
        recovery_start = script.index(
            "disarm-camera|disarm-photo|disarm-mic|stop-all)"
        )
        arm_case = script[arm_start:recovery_start]
        self.assertIn("require_payload", arm_case)
        self.assertIn("arm-mic-fresh", arm_case)
        recovery_end = script.index("  uninstall)", recovery_start)
        recovery_case = script[recovery_start:recovery_end]
        self.assertIn("acquire_lock", recovery_case)
        self.assertNotIn("require_payload", recovery_case)

        controller = (
            MODULE / "java/com/gushu/longvc/RootController.java"
        ).read_text(encoding="utf-8")
        installer = (
            MODULE / "java/com/gushu/longvc/PayloadInstaller.java"
        ).read_text(encoding="utf-8")
        self.assertIn('STOP_ALL("stop-all", false)', controller)
        self.assertIn("installer.ensureControlScript()", controller)
        self.assertIn("void ensureControlScript()", installer)
        self.assertIn("isControlScriptCurrent()", installer)
        self.assertIn("result=degraded", controller)
        self.assertIn("Never execute an unknown/corrupt file as root", controller)
        self.assertIn("boolean commitStarted", installer)
        self.assertIn("commit(stage, expected, manifest)", installer)
        self.assertIn("recovery stage=", installer)
        self.assertIn("u:object_r:system_file:s0", installer)

    def test_unavailable_camera_output_pauses_both_start_paths(self) -> None:
        controller = (MODULE / "java/com/gushu/longvc/RootController.java").read_text(encoding="utf-8")
        action = controller.split("RuntimeState run(Action action)", 1)[1].split("RuntimeState stageVideo", 1)[0]
        self.assertIn('case ARM_CAMERA -> "camera_arm_failed"', action)
        self.assertIn('case ARM_PHOTO -> "photo_arm_failed"', action)
        self.assertLess(action.index("cameraError != null && isCameraStartPaused()"),
                        action.index("installer.install()"))
        self.assertIn('initFailed && result.hasLine("camera_init=output_unavailable")', action)
        self.assertLess(action.index("cameraStartPaused = true"), action.index("camera_recovery=unverified"))
        self.assertIn("private static volatile boolean cameraStartPaused", controller)
        self.assertIn("!isCameraStartPaused() && state.cameraHookReady", controller)

        native = ROOT / "engines/camreplace/android"
        camera = (native / "camhook/src/hook_main.cpp").read_text(encoding="utf-8")
        injector = (native / "injector/src/main.cpp").read_text(encoding="utf-8")
        self.assertEqual(camera.count("return cr::media::kCameraOutputUnavailable;"), 1)
        self.assertIn("camera_init=output_unavailable", injector)

    def test_manifest_limits_permissions_and_disables_backup(self) -> None:
        manifest_path = MODULE / "AndroidManifest.xml"
        tree = ET.parse(manifest_path)
        root = tree.getroot()
        android = "{http://schemas.android.com/apk/res/android}"
        permissions = {
            node.attrib.get(android + "name")
            for node in root.findall("uses-permission")
        }
        self.assertEqual(
            {"android.permission.INTERNET"},
            permissions,
        )
        application = root.find("application")
        self.assertIsNotNone(application)
        assert application is not None
        self.assertEqual("false", application.attrib.get(android + "allowBackup"))
        self.assertEqual("false", application.attrib.get(android + "usesCleartextTraffic"))

    def test_build_uses_current_modern_xposed_api(self) -> None:
        build = (ROOT / "lsposed-module/app/build.gradle").read_text(encoding="utf-8")
        wrapper = (
            ROOT / "lsposed-module/gradle/wrapper/gradle-wrapper.properties"
        ).read_text(encoding="utf-8")
        self.assertIn('compileSdk = 37', build)
        self.assertIn('io.github.libxposed:api:102.0.0', build)
        self.assertIn('io.github.libxposed:service:102.0.0', build)
        self.assertIn('gradle-9.5.1-bin.zip', wrapper)
        self.assertIn('distributionSha256Sum=', wrapper)
        self.assertIn('machine != 0xb7', build)
        self.assertIn('LONGVC_VERSION_CODE', build)

    def test_ui_ready_requires_authenticated_media_channel(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        runtime = (
            MODULE / "java/com/gushu/longvc/RuntimeState.java"
        ).read_text(encoding="utf-8")
        audio_header = (
            ROOT / "engines/camreplace/android/feed/src/audio_shm.h"
        ).read_text(encoding="utf-8")

        self.assertIn('channel_has_header "$DATA/feed" 44465243 48', script)
        self.assertIn('channel_has_header "$DATA/audio" 55415243 36', script)
        self.assertIn('say "video_channel_ready=$video_channel"', script)
        self.assertIn('say "audio_channel_ready=$audio_channel"', script)
        self.assertIn("&& videoChannelReady", runtime)
        self.assertIn("&& audioChannelReady", runtime)
        self.assertIn("offsetof(cr_audio_header, channel_state) == 36", audio_header)

    def test_root_operations_survive_activity_recreation_without_racing(self) -> None:
        application = (
            MODULE / "java/com/gushu/longvc/LongVcApplication.java"
        ).read_text(encoding="utf-8")
        activity = (MODULE / "java/com/gushu/longvc/MainActivity.java").read_text(
            encoding="utf-8"
        )
        self.assertIn("static final ExecutorService ROOT_OPERATIONS", application)
        self.assertIn("newSingleThreadExecutor", application)
        self.assertIn("operations.attach(operationListener)", activity)
        self.assertIn("operations.detach(operationListener)", activity)
        self.assertIn("studioOperations()", application)
        self.assertIn("operations.claimCompletion(snapshot)", activity)

    def test_product_has_only_android_entry_points(self) -> None:
        for path in ("desktop/main.py", "long_virtual_camera.py", "build.bat",
                     "engines/camreplace/host/CMakeLists.txt",
                     "tools/package.py", "tools/release.py"):
            with self.subTest(path=path):
                self.assertFalse((ROOT / path).exists())

    def test_development_workflow_cannot_request_production_signing(self) -> None:
        workflows = ROOT / ".github/workflows"
        development = (workflows / "development-build.yml").read_text(encoding="utf-8")
        release = (workflows / "release-build.yml").read_text(encoding="utf-8")
        shared = (workflows / "build-android-apk.yml").read_text(encoding="utf-8")
        self.assertIn("workflow_dispatch:", development)
        self.assertIn("uses: ./.github/workflows/build-android-apk.yml", development)
        self.assertNotIn("production:", development)
        self.assertNotIn("LONGVC_ANDROID_", development)
        self.assertIn("LONGVC_DEVELOPMENT_KEYSTORE_BASE64", development)
        self.assertIn("LONGVC_DEVELOPMENT_KEYSTORE_PASSWORD", development)
        self.assertNotIn("LONGVC_DEVELOPMENT_", release)
        self.assertIn("production: true", release)
        self.assertIn("production:\n        default: false", shared)
        self.assertNotIn("contents: write", shared)
        verifier = (workflows.parent / "scripts/verify-android-apk.sh").read_text()
        self.assertIn("verify-android-apk.sh", shared)
        self.assertIn("development_cert=$(<.github/development-cert.sha256)", verifier)
        self.assertIn('[[ "$signer" == "$development_cert" ]]', verifier)
        fingerprint = (workflows.parent / "development-cert.sha256").read_text().strip()
        self.assertRegex(fingerprint, r"^[a-f0-9]{64}$")
        self.assertNotIn("upload-artifact", (workflows / "quality.yml").read_text())

    def test_obs_runtime_pulls_without_exposing_uri_credentials(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        feed = ROOT / "engines/camreplace/android/feed/src/rtmp_h264_feed.cpp"
        native = feed.read_text(encoding="utf-8")
        self.assertIn("rtmp:pull:config:", script)
        self.assertNotIn("obs_address=", script)
        self.assertIn('say "obs_pull_running=$obs_pull"', script)
        self.assertNotIn("rtmp:listen:", script)
        self.assertNotIn("bind(", native)
        self.assertIn("connect(fd,", native)

    def test_obs_panel_accepts_relay_url_and_separate_audio_selection(self) -> None:
        app = MODULE / "java/com/gushu/longvc"
        activity = (app / "MainActivity.java").read_text(encoding="utf-8")
        controller = (app / "RootController.java").read_text(encoding="utf-8")
        sources = (app / "MediaSourceManager.java").read_text(encoding="utf-8")
        layout = (MODULE / "res/layout/studio_media.xml").read_text(encoding="utf-8")
        self.assertIn("obsUrlInput.getText()", activity)
        self.assertIn("obsAudioSwitch.isChecked()", activity)
        self.assertIn('useObsAudio ? "obs" : "local"', sources)
        self.assertIn("obsSettingsMatch(url, obsAudio)", controller)
        self.assertIn("if (mediaSources.obsAudioSettingsMatch(url)) return status();", controller)
        self.assertIn('new SourceConfig(visualMode, visualKind, visualPath, "obs", audioPath, url)', sources)
        self.assertNotIn("obs_token", sources)
        self.assertNotIn("state.obsAddress", activity)
        self.assertIn('android:inputType="textPassword"', layout)
        self.assertIn("flagNoPersonalizedLearning", layout)

    def test_camera_hook_targets_only_cameraserver(self) -> None:
        script = (MODULE / "payload/longvc-control.sh").read_text(encoding="utf-8")
        native = (ROOT / "engines/camreplace/android/camhook/src/hook_main.cpp"
                  ).read_text(encoding="utf-8")
        self.assertIn('--target cameraserver --lib "$HOOK" --init cr_camhook_init', script)
        self.assertIn('hook_pid cameraserver "$CAM_READY"', script)
        self.assertIn('strcmp(process, "cameraserver") != 0', native)
        self.assertIn("Camera3OutputStream21queueBufferToConsumer", native)
        self.assertIn("AHardwareBuffer_Planes planes{}", native)
        self.assertNotIn("struct CR_AHB_Planes", native)
        init = native.split("int cr_camhook_init(const char* feed_url)", 1)[1].split(
            "int cr_camhook_shutdown()", 1
        )[0]
        self.assertNotIn("resolve_runtime_ops", init)

    def test_picker_result_is_not_blocked_by_startup_refresh(self) -> None:
        activity = (MODULE / "java/com/gushu/longvc/MainActivity.java").read_text(
            encoding="utf-8"
        )
        self.assertIn("if (!documentPickerOpen && !busy && !emergencyBusy && !hasUnseenOperation)", activity)
        self.assertLess(activity.index("documentPickerOpen = true;"),
                        activity.index("startActivityForResult(intent, requestCode)"))
        self.assertIn("outState.putBoolean(PICKER_OPEN, documentPickerOpen)", activity)

    def test_scope_health_requires_current_system_server(self) -> None:
        activity = (MODULE / "java/com/gushu/longvc/MainActivity.java").read_text(
            encoding="utf-8"
        )
        self.assertIn('"system_server".equals(process)', activity)
        self.assertIn('"android".equals(process) || "system".equals(process)', activity)
        self.assertIn("target.getUid() != android.os.Process.SYSTEM_UID", activity)
        self.assertIn("case UP_TO_DATE -> R.string.scope_running", activity)
        self.assertIn("case STALE -> R.string.scope_stale", activity)
        self.assertIn("framework_unbound, getPackageName()", activity)

    def test_install_verifies_before_disarming_rollback(self) -> None:
        installer = (MODULE / "java/com/gushu/longvc/PayloadInstaller.java").read_text(
            encoding="utf-8"
        )
        commit = installer.split("private void commit(", 1)[1]
        self.assertLess(commit.index("appendInstalledChecks(command, expected, manifest)"),
                        commit.index("trap - EXIT HUP INT TERM; echo committed"))
        self.assertNotIn('rm -rf \\"$backup\\"', commit)
        cleanup = installer.split("private void discardStage(", 1)[1]
        self.assertIn("catch (IOException cleanupFailure)", cleanup)
        self.assertIn('DEPLOY_DIR = "/data/cr/payload"', installer)
        self.assertIn("appendTrustedDeploymentChecks(command)", installer)
        staging = installer.split("private void verifyRemoteStage(", 1)[1].split("private void commit(", 1)[0]
        self.assertIn('appendEntryCheck(command, stage + "/" + MANIFEST_ASSET, manifest)', staging)
        self.assertIn('result.hasLine("verified")', staging)
        self.assertIn("appendEntryCheck(command, REMOTE_MANIFEST, manifest)", installer)

    def test_injector_discovers_services_by_executable(self) -> None:
        source = (ROOT / "engines/camreplace/android/injector/src/ptrace_arm64.cpp"
                  ).read_text(encoding="utf-8")
        lookup = source.split("pid_t find_pid(", 1)[1]
        self.assertIn('"/proc/%s/exe"', lookup)
        self.assertNotIn('"/proc/%s/comm"', lookup)


if __name__ == "__main__":
    unittest.main()
